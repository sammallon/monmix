#include "app_logd.h"
#include "app_ms_client.h"
#include "app_state.h"
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
//   "Mix 1" in the MS UI = MIX index 0. Runtime-selectable via ws_set_mix;
//   defaults to 0 on boot.
#define WS_GET_PREFIX          "/console/data/get/"
#define WS_GET_PREFIX_LEN      (sizeof(WS_GET_PREFIX) - 1)

static int s_mix_bus_idx;

static esp_websocket_client_handle_t s_ws;
static app_ms_state_t                s_state = APP_MS_STATE_BOOT;

#define MAX_SUBSCRIBERS 4
static struct {
    app_ms_on_change_t cb;
    void              *ctx;
} s_subscribers[MAX_SUBSCRIBERS];
static size_t s_subscriber_count;

static void notify_subscribers(void)
{
    for (size_t i = 0; i < s_subscriber_count; ++i) {
        if (s_subscribers[i].cb) s_subscribers[i].cb(s_subscribers[i].ctx);
    }
}

static void set_state(app_ms_state_t s)
{
    if (s_state == s) return;
    s_state = s;
    notify_subscribers();
}

static bool ws_start(void);
static void ws_set_level(int ms_channel_id, float level);
static void ws_set_mute (int ms_channel_id, bool mute);
static void ws_set_name (int ms_channel_id, const char *name);
static void ws_stop(void);
static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data);
static void send_envelope(const char *method, const char *path, const char *body_json);
static void subscribe_path(const char *dotted, const char *format);
static void on_connected_subscribe_all(void);
static void handle_broadcast(const char *json, size_t len);

static app_ms_state_t ws_get_state(void)             { return s_state; }
static const char    *ws_get_host(void)              { return APP_MS_HOST; }
static int            ws_get_port(void)              { return APP_MS_PORT; }
static void           ws_register_on_change(app_ms_on_change_t cb, void *ctx)
{
    if (!cb || s_subscriber_count >= MAX_SUBSCRIBERS) return;
    s_subscribers[s_subscriber_count].cb  = cb;
    s_subscribers[s_subscriber_count].ctx = ctx;
    s_subscriber_count++;
}

static int ws_get_mix(void) { return s_mix_bus_idx; }

static void ws_resubscribe(void)
{
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        on_connected_subscribe_all();
    }
}

static void ws_set_mix(int mix_idx)
{
    if (mix_idx < 0)               return;
    if (s_mix_bus_idx == mix_idx)  return;
    s_mix_bus_idx = mix_idx;
    ESP_LOGI(TAG, "mix bus -> %d (Mix %d)", mix_idx, mix_idx + 1);
    // Re-subscribe per-channel under the new mix index. MS dedupes on
    // path, so the old mix's subscriptions remain active server-side and
    // continue broadcasting; handle_broadcast filters those by current
    // s_mix_bus_idx and silently drops them. True unsubscribe support is
    // a follow-up — the leak is bounded (N channels per mix selected this
    // session) and resets on WS reconnect.
    ws_resubscribe();
    notify_subscribers();
}

static const ms_client_iface_t s_iface = {
    .start              = ws_start,
    .set_level          = ws_set_level,
    .set_mute           = ws_set_mute,
    .set_name           = ws_set_name,
    .stop               = ws_stop,
    .get_state          = ws_get_state,
    .get_host           = ws_get_host,
    .get_port           = ws_get_port,
    .register_on_change = ws_register_on_change,
    .get_mix            = ws_get_mix,
    .set_mix            = ws_set_mix,
    .resubscribe        = ws_resubscribe,
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
    set_state(APP_MS_STATE_CONNECTING);
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
             ms_channel_id, s_mix_bus_idx);

    char body[48];
    snprintf(body, sizeof(body), "{\"value\":%.6f}", (double)level);

    send_envelope("POST", path, body);
}

static void ws_set_mute(int ms_channel_id, bool mute)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "set_mute dropped: ws not connected");
        return;
    }

    char path[80];
    snprintf(path, sizeof(path),
             "/console/data/set/ch.%d.levelData.%d.on/val",
             ms_channel_id, s_mix_bus_idx);

    // MS `.on` is a bool: true = audible (NOT muted), false = muted. We
    // expose `mute` to the rest of the firmware as the user-facing boolean
    // (true = "this channel is silenced"), so flip it on the wire.
    const char *body = mute ? "{\"value\":false}" : "{\"value\":true}";
    send_envelope("POST", path, body);
}

static void ws_set_name(int ms_channel_id, const char *name)
{
    if (!name) return;
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "set_name dropped: ws not connected");
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), "/console/data/set/ch.%d.cfg.name/val", ms_channel_id);

    // Build the JSON body via cJSON so the name string is escaped properly
    // (quotes, backslashes, control chars) — a hand-rolled snprintf would
    // miscompose any name with a quote in it.
    cJSON *body = cJSON_CreateObject();
    if (!body) return;
    cJSON_AddStringToObject(body, "value", name);
    char *body_text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_text) return;

    send_envelope("POST", path, body_text);
    free(body_text);
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
        APP_LOGD_E("ms_ws", "envelope too large path=%s", path);
        return;
    }
    APP_LOGD_T("ms_ws", "tx %s %s", method, path);
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
    // For each tracked channel, subscribe to its fader (norm 0..1), scribble
    // strip name, and mute state. The initial-value broadcasts populate
    // app_state before the user touches anything.
    for (size_t i = 0; i < app_state_count(); ++i) {
        int ch_id = app_state_id_for_idx(i);
        if (ch_id < 0) continue;

        char dotted[48];
        snprintf(dotted, sizeof(dotted),
                 "ch.%d.levelData.%d.lvl", ch_id, s_mix_bus_idx);
        subscribe_path(dotted, "norm");

        // Same level node, different alias + format → MS sends dB. We need
        // both because norm drives the slider linearly and dB drives the
        // user-facing readout (with non-linear MS-specific mapping that we
        // can't compute locally).
        snprintf(dotted, sizeof(dotted),
                 "ch.%d.levelData.%d.level", ch_id, s_mix_bus_idx);
        subscribe_path(dotted, "val");

        snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", ch_id);
        subscribe_path(dotted, "val");

        snprintf(dotted, sizeof(dotted),
                 "ch.%d.levelData.%d.on", ch_id, s_mix_bus_idx);
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

    // Single-pass dispatch on the trailing key. sscanf with `== N` checks
    // ONLY confirm that the %d conversions filled — the rest of the format
    // string can still mismatch and sscanf returns the same count, so a
    // path like `levelData.0.level` would falsely match
    // `levelData.%d.lvl`. Matching the prefix once and switching on the
    // suffix avoids that whole class of bug.
    int  ch = 0, mix = 0;
    char suffix[16] = {0};
    if (sscanf(dotted, "ch.%d.levelData.%d.%15s", &ch, &mix, suffix) == 3 &&
        mix == s_mix_bus_idx) {
        int idx = app_state_idx_for_id(ch);
        if (idx >= 0) {
            if (strcmp(suffix, "lvl") == 0 && cJSON_IsNumber(jvalue)) {
                app_state_set_level((size_t)idx, (float)jvalue->valuedouble, true);
            } else if (strcmp(suffix, "level") == 0 && cJSON_IsNumber(jvalue)) {
                app_state_set_level_db((size_t)idx, (float)jvalue->valuedouble, true);
            } else if (strcmp(suffix, "on") == 0 && cJSON_IsBool(jvalue)) {
                // MS `.on` true = audible, false = muted. Flip to our
                // user-facing boolean (true = "this channel is silenced").
                bool ms_on = cJSON_IsTrue(jvalue);
                app_state_set_mute((size_t)idx, !ms_on, true);
            }
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
        APP_LOGD_I("ms_ws", "connected to %s:%d", APP_MS_HOST, APP_MS_PORT);
        set_state(APP_MS_STATE_CONNECTED);
        on_connected_subscribe_all();
        break;
    }

    case WEBSOCKET_EVENT_DATA:
        if (evt->op_code == 0x1 /* text */ && evt->data_len > 0) {
            APP_LOGD_T("ms_ws", "rx %.*s",
                       (int) (evt->data_len > 90 ? 90 : evt->data_len),
                       evt->data_ptr);
            handle_broadcast(evt->data_ptr, (size_t)evt->data_len);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        APP_LOGD_W("ms_ws", "disconnected");
        set_state(APP_MS_STATE_DISCONNECTED);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "error");
        APP_LOGD_E("ms_ws", "error event");
        set_state(APP_MS_STATE_ERROR);
        break;

    default:
        break;
    }
}
