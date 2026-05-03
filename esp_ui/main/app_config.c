#include "app_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "secrets.h"

static const char *TAG = "app_config";

#define NVS_NAMESPACE "monmix"
#define NVS_KEY_CHAN     "chan_ids"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_MS_HOST   "ms_host"
#define NVS_KEY_MS_PORT   "ms_port"

// 12 channels covers a typical stage spread (lead vox + 4 mics + drums + DIs)
// across two pages of 6. Easy to override by erasing NVS or by editing this
// list and reflashing — the M2 firmware ships without an on-device editor.
static const int s_default_ids[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
#define DEFAULT_COUNT (sizeof(s_default_ids) / sizeof(s_default_ids[0]))

static int    s_ids[APP_CONFIG_MAX_CHANNELS];
static size_t s_count;

static char     s_wifi_ssid[APP_CONFIG_SSID_MAX];
static char     s_wifi_pass[APP_CONFIG_PASS_MAX];
static char     s_ms_host[APP_CONFIG_HOST_MAX];
static uint16_t s_ms_port;

static void use_defaults(void)
{
    s_count = DEFAULT_COUNT;
    memcpy(s_ids, s_default_ids, sizeof(s_default_ids));
}

static bool load_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return false;

    size_t blob_len = sizeof(s_ids);
    err = nvs_get_blob(h, NVS_KEY_CHAN, s_ids, &blob_len);
    nvs_close(h);

    if (err != ESP_OK) return false;
    if (blob_len == 0 || blob_len % sizeof(int) != 0) {
        ESP_LOGW(TAG, "ignoring malformed NVS blob (%u bytes)", (unsigned)blob_len);
        return false;
    }
    s_count = blob_len / sizeof(int);
    if (s_count > APP_CONFIG_MAX_CHANNELS) s_count = APP_CONFIG_MAX_CHANNELS;
    return true;
}

static void seed_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(h, NVS_KEY_CHAN, s_ids, s_count * sizeof(int));
    if (err == ESP_OK) nvs_commit(h);
    else ESP_LOGW(TAG, "seed nvs failed: %s", esp_err_to_name(err));
    nvs_close(h);
}

// Generic str/u16 NVS load with secrets.h fallback. The fallback is what
// makes secrets.h still "the source of truth on first boot, factory reset,
// or NVS corruption" -- after that, NVS wins.
static void load_str(const char *key, char *dst, size_t dst_size,
                     const char *fallback)
{
    nvs_handle_t h;
    bool got = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = dst_size;
        if (nvs_get_str(h, key, dst, &len) == ESP_OK) {
            got = true;
        }
        nvs_close(h);
    }
    if (!got) {
        strncpy(dst, fallback, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

static void load_u16(const char *key, uint16_t *dst, uint16_t fallback)
{
    nvs_handle_t h;
    bool got = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u16(h, key, dst) == ESP_OK) got = true;
        nvs_close(h);
    }
    if (!got) *dst = fallback;
}

static bool save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save %s failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool save_u16(const char *key, uint16_t val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u16(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

void app_config_init(void)
{
    if (load_from_nvs()) {
        ESP_LOGI(TAG, "loaded %u channels from NVS", (unsigned)s_count);
    } else {
        use_defaults();
        seed_nvs();
        ESP_LOGI(TAG, "seeded NVS with %u default channels", (unsigned)s_count);
    }

    load_str(NVS_KEY_WIFI_SSID, s_wifi_ssid, sizeof(s_wifi_ssid), APP_WIFI_SSID);
    load_str(NVS_KEY_WIFI_PASS, s_wifi_pass, sizeof(s_wifi_pass), APP_WIFI_PASSWORD);
    load_str(NVS_KEY_MS_HOST,   s_ms_host,   sizeof(s_ms_host),   APP_MS_HOST);
    load_u16(NVS_KEY_MS_PORT,   &s_ms_port,  APP_MS_PORT);
    // Don't log the password. Don't log the SSID either -- not strictly
    // sensitive but unnecessary to leak to UART/SD on every boot.
    ESP_LOGI(TAG, "wifi+ms config loaded (ms=%s:%u)", s_ms_host, s_ms_port);
}

const int *app_config_channel_ids(size_t *out_count)
{
    if (out_count) *out_count = s_count;
    return s_ids;
}

const char *app_config_wifi_ssid(void) { return s_wifi_ssid; }
const char *app_config_wifi_pass(void) { return s_wifi_pass; }
const char *app_config_ms_host(void)   { return s_ms_host; }
uint16_t    app_config_ms_port(void)   { return s_ms_port; }

bool app_config_set_wifi_ssid(const char *ssid)
{
    if (!ssid || strlen(ssid) >= sizeof(s_wifi_ssid)) return false;
    if (!save_str(NVS_KEY_WIFI_SSID, ssid)) return false;
    strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);
    s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';
    return true;
}

bool app_config_set_wifi_pass(const char *pass)
{
    if (!pass || strlen(pass) >= sizeof(s_wifi_pass)) return false;
    if (!save_str(NVS_KEY_WIFI_PASS, pass)) return false;
    strncpy(s_wifi_pass, pass, sizeof(s_wifi_pass) - 1);
    s_wifi_pass[sizeof(s_wifi_pass) - 1] = '\0';
    return true;
}

bool app_config_set_ms_host(const char *host)
{
    if (!host || strlen(host) >= sizeof(s_ms_host)) return false;
    if (!save_str(NVS_KEY_MS_HOST, host)) return false;
    strncpy(s_ms_host, host, sizeof(s_ms_host) - 1);
    s_ms_host[sizeof(s_ms_host) - 1] = '\0';
    return true;
}

bool app_config_set_ms_port(uint16_t port)
{
    if (port == 0) return false;
    if (!save_u16(NVS_KEY_MS_PORT, port)) return false;
    s_ms_port = port;
    return true;
}
