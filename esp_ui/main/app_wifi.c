#include "app_wifi.h"
#include "app_config.h"
#include "app_logd.h"
#include "app_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "app_wifi";

#define BIT_CONNECTED (1 << 0)
#define BIT_FAILED    (1 << 1)
#define MAX_RETRIES         20
#define RETRY_BACKOFF_MS    1000

static EventGroupHandle_t s_evt;
static int                s_retry;
static app_wifi_state_t   s_state = APP_WIFI_STATE_BOOT;
static esp_ip4_addr_t     s_ip;          // latest assigned IP, zero when none

#define MAX_SUBSCRIBERS 4
static struct {
    app_wifi_on_change_t cb;
    void                *ctx;
} s_subscribers[MAX_SUBSCRIBERS];
static size_t s_subscriber_count;

static void notify(void)
{
    for (size_t i = 0; i < s_subscriber_count; ++i) {
        if (s_subscribers[i].cb) s_subscribers[i].cb(s_subscribers[i].ctx);
    }
}

static void set_state(app_wifi_state_t s)
{
    if (s_state == s) return;
    s_state = s;
    notify();
}

static void on_scan_done(void);

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        set_state(APP_WIFI_STATE_CONNECTING);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        on_scan_done();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        s_ip.addr = 0;
        if (s_retry < MAX_RETRIES) {
            ++s_retry;
            ESP_LOGW(TAG, "disconnect (reason=%d), retry %d/%d in %d ms",
                     e ? e->reason : -1, s_retry, MAX_RETRIES, RETRY_BACKOFF_MS);
            APP_LOGD_W("app_wifi", "disconnect reason=%d retry=%d/%d",
                       e ? e->reason : -1, s_retry, MAX_RETRIES);
            set_state(APP_WIFI_STATE_CONNECTING);
            vTaskDelay(pdMS_TO_TICKS(RETRY_BACKOFF_MS));
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "wifi connect exhausted retries (last reason=%d)",
                     e ? e->reason : -1);
            APP_LOGD_E("app_wifi", "retries exhausted, last reason=%d",
                       e ? e->reason : -1);
            set_state(APP_WIFI_STATE_FAILED);
            xEventGroupSetBits(s_evt, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&evt->ip_info.ip));
        APP_LOGD_I("app_wifi", "got ip " IPSTR, IP2STR(&evt->ip_info.ip));
        s_ip = evt->ip_info.ip;
        s_retry = 0;
        set_state(APP_WIFI_STATE_CONNECTED);
        xEventGroupSetBits(s_evt, BIT_CONNECTED);
    }
}

void app_wifi_init_radio(void)
{
    s_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // esp_wifi_init() is what causes ESP-Hosted to bring up its SDIO bus to
    // the C6 — and as a side effect, sdmmc_host_init() runs once for the
    // shared host controller. After this returns, slot 0 (the SD card) can
    // be mounted with dummy host.init/deinit (see app_storage.c).
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_event, NULL, NULL));

    wifi_config_t cfg = {0};
    // strncpy(..., sizeof(dst)) trips -Wstringop-truncation with a runtime
    // source. App config getters already enforce length bounds (33 / 65 with
    // NUL), so a memcpy of strlen + the zero-init above is exact and safe.
    const char *ssid = app_config_wifi_ssid();
    const char *pass = app_config_wifi_pass();
    memcpy(cfg.sta.ssid,     ssid, strlen(ssid));
    memcpy(cfg.sta.password, pass, strlen(pass));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool app_wifi_wait_connected(void)
{
    EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_CONNECTED | BIT_FAILED,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & BIT_CONNECTED) != 0;
}

app_wifi_state_t app_wifi_get_state(void)
{
    return s_state;
}

const char *app_wifi_get_ssid(void)
{
    return app_config_wifi_ssid();
}

void app_wifi_format_ip(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return;
    snprintf(buf, buflen, IPSTR, IP2STR(&s_ip));
}

void app_wifi_register_on_change(app_wifi_on_change_t cb, void *ctx)
{
    if (!cb || s_subscriber_count >= MAX_SUBSCRIBERS) return;
    s_subscribers[s_subscriber_count].cb  = cb;
    s_subscribers[s_subscriber_count].ctx = ctx;
    s_subscriber_count++;
}

// SSID scan ------------------------------------------------------------------
//
// esp_wifi_scan_start fires WIFI_EVENT_SCAN_DONE on completion. We collect
// records into a static buffer there (event-task context) so the UI can
// pull them later without races. Hidden APs (empty SSID) are dropped: the
// user will type those manually if they need them.

static volatile bool          s_scan_in_progress;
static app_wifi_scan_done_t   s_scan_done_cb;
static void                  *s_scan_done_ctx;
static char                   s_scan_results[APP_WIFI_SCAN_MAX_RESULTS][33];
static size_t                 s_scan_result_count;

static void on_scan_done(void)
{
    uint16_t num = APP_WIFI_SCAN_MAX_RESULTS;
    static wifi_ap_record_t recs[APP_WIFI_SCAN_MAX_RESULTS];
    if (esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) num = 0;

    s_scan_result_count = 0;
    for (uint16_t i = 0; i < num && s_scan_result_count < APP_WIFI_SCAN_MAX_RESULTS; ++i) {
        if (recs[i].ssid[0] == '\0') continue;  // hidden — skip
        // Dedup: same SSID can appear multiple times (multiple APs in same
        // ESS). Keep the first (strongest, since records come back sorted
        // by RSSI descending).
        bool dup = false;
        for (size_t j = 0; j < s_scan_result_count; ++j) {
            if (strcmp(s_scan_results[j], (const char *) recs[i].ssid) == 0) {
                dup = true; break;
            }
        }
        if (dup) continue;
        strncpy(s_scan_results[s_scan_result_count], (const char *) recs[i].ssid,
                sizeof(s_scan_results[0]) - 1);
        s_scan_results[s_scan_result_count][sizeof(s_scan_results[0]) - 1] = '\0';
        s_scan_result_count++;
    }
    ESP_LOGI(TAG, "scan done: %u SSIDs", (unsigned) s_scan_result_count);
    s_scan_in_progress = false;
    if (s_scan_done_cb) s_scan_done_cb(s_scan_done_ctx);
}

bool app_wifi_scan_start(app_wifi_scan_done_t done_cb, void *ctx)
{
    if (s_scan_in_progress) return false;
    s_scan_done_cb  = done_cb;
    s_scan_done_ctx = ctx;
    s_scan_in_progress = true;

    wifi_scan_config_t cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(err));
        s_scan_in_progress = false;
        return false;
    }
    return true;
}

size_t app_wifi_scan_results(char (*dst)[33], size_t max_count)
{
    if (!dst || max_count == 0) return 0;
    size_t n = s_scan_result_count;
    if (n > max_count) n = max_count;
    for (size_t i = 0; i < n; ++i) {
        memcpy(dst[i], s_scan_results[i], sizeof(s_scan_results[0]));
    }
    return n;
}
