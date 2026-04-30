#include "app_ms_client.h"
#include "app_state.h"
#include "secrets.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_websocket_client.h"

static const char *TAG = "ms_ws";

// TODO(unbox): the exact WS URI and subscribe-frame format need to be
// confirmed against the user's data-explorer at http://CONFIG_APP_MS_HOST:CONFIG_APP_MS_PORT/#/data-explorer
// — these placeholders match the public Mixing Station docs hint
// (POST /console/data/subscribe, path "ch.*.mix.lvl") but the exact
// frame envelope on the WS channel may differ.
#define MS_WS_PATH      "/api/ws"
#define MS_REST_LVL_FMT "/api/data/ch.%d.mix.lvl"
#define MS_SUBSCRIBE_FRAME \
    "{\"op\":\"subscribe\",\"path\":\"ch.*.mix.lvl\",\"format\":\"val\"}"

static esp_websocket_client_handle_t s_ws;

static bool ws_start(void);
static void ws_set_level(int ms_channel_id, float level);
static void ws_stop(void);
static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data);

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
    snprintf(uri, sizeof(uri), "ws://%s:%d%s",
             APP_MS_HOST, APP_MS_PORT, MS_WS_PATH);

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
    return true;
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *evt = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected; subscribing");
        esp_websocket_client_send_text(s_ws, MS_SUBSCRIBE_FRAME,
                                       strlen(MS_SUBSCRIBE_FRAME), portMAX_DELAY);
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (evt->op_code != 0x1 /* text */ || evt->data_len <= 0) break;

        // Expected shape (per public docs hint, verify in data-explorer):
        //   {"path":"ch.1.mix.lvl","val":0.85}
        cJSON *root = cJSON_ParseWithLength(evt->data_ptr, evt->data_len);
        if (!root) break;
        cJSON *jpath = cJSON_GetObjectItem(root, "path");
        cJSON *jval  = cJSON_GetObjectItem(root, "val");
        if (cJSON_IsString(jpath) && cJSON_IsNumber(jval)) {
            int ch = 0;
            if (sscanf(jpath->valuestring, "ch.%d.mix.lvl", &ch) == 1) {
                int idx = app_state_idx_for_id(ch);
                if (idx >= 0) {
                    app_state_set_level((size_t)idx, (float)jval->valuedouble, true);
                }
            }
        }
        cJSON_Delete(root);
        break;
    }

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "error");
        break;

    default:
        break;
    }
}

static void ws_set_level(int ms_channel_id, float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d" MS_REST_LVL_FMT,
             APP_MS_HOST, APP_MS_PORT, ms_channel_id);

    char body[64];
    int  body_len = snprintf(body, sizeof(body), "{\"val\":%.4f}", (double)level);

    esp_http_client_config_t http_cfg = {
        .url    = url,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t h = esp_http_client_init(&http_cfg);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_post_field(h, body, body_len);

    esp_err_t err = esp_http_client_perform(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(h);
}

static void ws_stop(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
}
