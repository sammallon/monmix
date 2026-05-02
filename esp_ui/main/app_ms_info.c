#include "app_ms_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "ms_info";

// 16 KB caps the response. The example payload from the test instance
// (Si Expression 2, ~80 channels) is around 7-8 KB. Other consoles run
// larger; if a response is bigger than the buffer we truncate and the
// JSON parse fails — at which point we log and the caller falls back
// to defaults.
#define HTTP_BUF_LEN 16384

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} http_sink_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    http_sink_t *sink = (http_sink_t *) evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && sink && evt->data && evt->data_len > 0) {
        if (sink->len + (size_t) evt->data_len < sink->cap) {
            memcpy(sink->buf + sink->len, evt->data, evt->data_len);
            sink->len += evt->data_len;
        } else {
            // Stop appending once the buffer is full; the JSON parse
            // will fail and the caller will know.
            ESP_LOGW(TAG, "response truncated at %u bytes", (unsigned) sink->len);
        }
    }
    return ESP_OK;
}

bool app_ms_info_fetch(const char *host, int port, app_ms_info_t *out)
{
    if (!host || !out) return false;
    memset(out, 0, sizeof(*out));

    char *buf = malloc(HTTP_BUF_LEN);
    if (!buf) {
        ESP_LOGE(TAG, "alloc %d bytes failed", HTTP_BUF_LEN);
        return false;
    }
    http_sink_t sink = { .buf = buf, .cap = HTTP_BUF_LEN, .len = 0 };

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/console/information", host, port);

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = on_http_event,
        .user_data     = &sink,
        .timeout_ms    = 5000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "client init failed");
        free(buf);
        return false;
    }

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GET %s failed err=%s status=%d",
                 url, esp_err_to_name(err), status);
        free(buf);
        return false;
    }

    // Null-terminate so cJSON has a clean string. sink.len < cap is
    // guaranteed by the on_data guard above.
    buf[sink.len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "json parse failed");
        return false;
    }

    cJSON *jt = cJSON_GetObjectItem(root, "totalChannels");
    if (cJSON_IsNumber(jt)) out->total = jt->valueint;

    cJSON *jts = cJSON_GetObjectItem(root, "channelTypes");
    if (cJSON_IsArray(jts)) {
        cJSON *type = NULL;
        cJSON_ArrayForEach(type, jts) {
            cJSON *jname   = cJSON_GetObjectItem(type, "name");
            cJSON *jcount  = cJSON_GetObjectItem(type, "count");
            cJSON *joffset = cJSON_GetObjectItem(type, "offset");
            if (!cJSON_IsString(jname) || !cJSON_IsNumber(jcount) ||
                !cJSON_IsNumber(joffset)) continue;
            const char *name = jname->valuestring;
            int count  = jcount->valueint;
            int offset = joffset->valueint;
            if      (strcmp(name, "Input")  == 0) { out->input_offset  = offset; out->input_count  = count; }
            else if (strcmp(name, "Aux")    == 0) { out->aux_offset    = offset; out->aux_count    = count; }
            else if (strcmp(name, "Mix")    == 0) { out->mix_offset    = offset; out->mix_count    = count; }
            else if (strcmp(name, "Matrix") == 0) { out->matrix_offset = offset; out->matrix_count = count; }
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "info: total=%d input=%d@%d aux=%d@%d mix=%d@%d matrix=%d@%d",
             out->total,
             out->input_count,  out->input_offset,
             out->aux_count,    out->aux_offset,
             out->mix_count,    out->mix_offset,
             out->matrix_count, out->matrix_offset);
    return true;
}
