#include "app_ms_client.h"
#include "app_state.h"
#include "app_ui.h"
#include "secrets.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"

static const char *TAG = "ms_ws";

// Mixing Station protocol — verified against the offline test instance
// (see reference_mixing_station_protocol memory, repro_ms_probe*.py).
//
//   Single WS to ws://<host>:<port>/  carries subscribe + set as HTTP-style
//   JSON envelopes. Subscribe immediately emits the current value, then
//   re-emits on every change. Set snaps to the mixer's quantized step and
//   broadcasts the snapped value back over any active subscription.
//
//   We subscribe per-tracked-channel rather than wildcard — fewer messages
//   on stage WiFi, and it scales naturally to per-musician channel
//   selection later.
//
//   "Mix 1" in the MS UI = MIX index 0. M1 hardcodes a single mix bus.
#define APP_MS_MIX_BUS         0
#define WS_GET_PREFIX          "/console/data/get/"
#define WS_GET_PREFIX_LEN      (sizeof(WS_GET_PREFIX) - 1)

static esp_websocket_client_handle_t s_ws;

static bool ws_start(void);
static void ws_set_level(int ms_channel_id, float level);
static void ws_stop(void);
static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data);
static void send_envelope(const char *method, const char *path, const char *body_json);
static void subscribe_path(const char *dotted, const char *format);
static void on_connected_subscribe_all(void);
static void handle_broadcast(const char *json, size_t len);

static const ms_client_iface_t s_iface = {
    .start     = ws_start,
    .set_level = ws_set_level,
    .stop      = ws_stop,
};

const ms_client_iface_t *app_ms_client_ws(void)
{
    return &s_iface;
}

static bool ws_start(void)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "ws://%s:%d/", APP_MS_HOST, APP_MS_PORT);

    esp_websocket_client_config_t cfg = {
        .uri                  = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "ws init failed");
        return false;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);

    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ws start failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "ws starting -> %s", uri);
    char buf[80];
    snprintf(buf, sizeof(buf), "MS: connecting to %s:%d...", APP_MS_HOST, APP_MS_PORT);
    app_ui_set_status(buf);
    return true;
}

static void ws_set_level(int ms_channel_id, float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "set_level dropped: ws not connected");
        return;
    }

    char path[80];
    snprintf(path, sizeof(path),
             "/console/data/set/ch.%d.levelData.%d.lvl/norm",
             ms_channel_id, APP_MS_MIX_BUS);

    char body[48];
    snprintf(body, sizeof(body), "{\"value\":%.6f}", (double)level);

    send_envelope("POST", path, body);
}

static void ws_stop(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
}

static void send_envelope(const char *method, const char *path, const char *body_json)
{
    char frame[256];
    int n = snprintf(frame, sizeof(frame),
                     "{\"method\":\"%s\",\"path\":\"%s\",\"body\":%s}",
                     method, path, body_json);
    if (n <= 0 || n >= (int)sizeof(frame)) {
        ESP_LOGE(TAG, "envelope too large for path %s", path);
        return;
    }
    esp_websocket_client_send_text(s_ws, frame, n, portMAX_DELAY);
}

static void subscribe_path(const char *dotted, const char *format)
{
    char body[80];
    snprintf(body, sizeof(body),
             "{\"path\":\"%s\",\"format\":\"%s\"}", dotted, format);
    send_envelope("POST", "/console/data/subscribe", body);
}

static void on_connected_subscribe_all(void)
{
    // For each tracked channel, subscribe to its fader (norm 0..1) and its
    // scribble strip name. The initial-value broadcasts populate app_state
    // before the user touches anything.
    for (size_t i = 0; i < app_state_count(); ++i) {
        int ch_id = app_state_id_for_idx(i);
        if (ch_id < 0) continue;

        char dotted[48];
        snprintf(dotted, sizeof(dotted),
                 "ch.%d.levelData.%d.lvl", ch_id, APP_MS_MIX_BUS);
        subscribe_path(dotted, "norm");

        snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", ch_id);
        subscribe_path(dotted, "val");
    }
    ESP_LOGI(TAG, "subscribed %d channels", (int)app_state_count());
}

static void handle_broadcast(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;

    cJSON *jpath = cJSON_GetObjectItem(root, "path");
    cJSON *jbody = cJSON_GetObjectItem(root, "body");
    cJSON *jerr  = cJSON_GetObjectItem(root, "error");

    if (cJSON_IsString(jerr)) {
        ESP_LOGW(TAG, "ms error: %s", jerr->valuestring);
        cJSON_Delete(root);
        return;
    }

    if (!cJSON_IsString(jpath) || !cJSON_IsObject(jbody)) {
        cJSON_Delete(root);
        return;
    }

    // Strip the "/console/data/get/" prefix. Anything that doesn't carry it
    // isn't a value broadcast we care about.
    const char *p = jpath->valuestring;
    if (strncmp(p, WS_GET_PREFIX, WS_GET_PREFIX_LEN) != 0) {
        cJSON_Delete(root);
        return;
    }
    const char *dotted = p + WS_GET_PREFIX_LEN;

    cJSON *jvalue = cJSON_GetObjectItem(jbody, "value");

    int ch = 0, mix = 0;
    if (sscanf(dotted, "ch.%d.levelData.%d.lvl", &ch, &mix) == 2 &&
        mix == APP_MS_MIX_BUS && cJSON_IsNumber(jvalue)) {
        int idx = app_state_idx_for_id(ch);
        if (idx >= 0) {
            app_state_set_level((size_t)idx, (float)jvalue->valuedouble, true);
        }
        cJSON_Delete(root);
        return;
    }

    if (sscanf(dotted, "ch.%d.cfg.name", &ch) == 1 && cJSON_IsString(jvalue)) {
        int idx = app_state_idx_for_id(ch);
        if (idx >= 0) {
            app_state_set_name((size_t)idx, jvalue->valuestring, true);
        }
    }

    cJSON_Delete(root);
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *evt = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected");
        char buf[64];
        snprintf(buf, sizeof(buf), "MS: connected %s:%d", APP_MS_HOST, APP_MS_PORT);
        app_ui_set_status(buf);
        on_connected_subscribe_all();
        break;
    }

    case WEBSOCKET_EVENT_DATA:
        if (evt->op_code == 0x1 /* text */ && evt->data_len > 0) {
            handle_broadcast(evt->data_ptr, (size_t)evt->data_len);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        app_ui_set_status("MS: disconnected, retrying...");
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "error");
        app_ui_set_status("MS: error, retrying...");
        break;

    default:
        break;
    }
}
