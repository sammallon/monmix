#include "app_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "secrets.h"

static const char *TAG = "app_config";

#define NVS_NAMESPACE      "monpp"
#define NVS_KEY_WIFI_SSID  "wifi_ssid"
#define NVS_KEY_WIFI_PASS  "wifi_pass"
#define NVS_KEY_PP_HOST    "pp_host"
#define NVS_KEY_PP_PORT    "pp_port"
#define NVS_KEY_WIFI_SAVED "wifi_saved"

static char     s_wifi_ssid[APP_CONFIG_SSID_MAX];
static char     s_wifi_pass[APP_CONFIG_PASS_MAX];
static char     s_pp_host[APP_CONFIG_HOST_MAX];
static uint16_t s_pp_port;

typedef struct {
    char ssid[APP_CONFIG_SSID_MAX];
    char pass[APP_CONFIG_PASS_MAX];
} saved_net_t;

static saved_net_t s_saved[APP_CONFIG_SAVED_NETWORKS_MAX];
static size_t      s_saved_count;

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

static bool save_blob(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save blob %s failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

static void load_saved_networks(void)
{
    s_saved_count = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_saved);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_WIFI_SAVED, s_saved, &len);
    nvs_close(h);
    if (err != ESP_OK) return;
    if (len == 0 || len % sizeof(saved_net_t) != 0) {
        ESP_LOGW(TAG, "ignoring malformed saved-networks blob (%u bytes)",
                 (unsigned) len);
        memset(s_saved, 0, sizeof(s_saved));
        return;
    }
    s_saved_count = len / sizeof(saved_net_t);
    if (s_saved_count > APP_CONFIG_SAVED_NETWORKS_MAX) {
        s_saved_count = APP_CONFIG_SAVED_NETWORKS_MAX;
    }
    for (size_t i = 0; i < s_saved_count; ++i) {
        s_saved[i].ssid[sizeof(s_saved[i].ssid) - 1] = '\0';
        s_saved[i].pass[sizeof(s_saved[i].pass) - 1] = '\0';
    }
}

static bool persist_saved_networks(void)
{
    return save_blob(NVS_KEY_WIFI_SAVED, s_saved,
                     s_saved_count * sizeof(saved_net_t));
}

void app_config_init(void)
{
    load_str(NVS_KEY_WIFI_SSID, s_wifi_ssid, sizeof(s_wifi_ssid), APP_WIFI_SSID);
    load_str(NVS_KEY_WIFI_PASS, s_wifi_pass, sizeof(s_wifi_pass), APP_WIFI_PASSWORD);
    load_str(NVS_KEY_PP_HOST,   s_pp_host,   sizeof(s_pp_host),   APP_PP_HOST);
    load_u16(NVS_KEY_PP_PORT,   &s_pp_port,  APP_PP_PORT);
    load_saved_networks();

    // First-run seeding: if the saved-networks list is empty but we have a
    // current SSID, drop it in. Stops the user from having to "save once to
    // populate the list" before they can ever benefit from the feature.
    if (s_saved_count == 0 && s_wifi_ssid[0] != '\0') {
        app_config_wifi_saved_add(s_wifi_ssid, s_wifi_pass);
    }

    // Don't log SSID or password -- not strictly sensitive but no need to
    // leak to UART/SD on every boot.
    ESP_LOGI(TAG, "config loaded (pp=%s:%u, %u saved net(s))",
             s_pp_host, s_pp_port, (unsigned) s_saved_count);
}

const char *app_config_wifi_ssid(void) { return s_wifi_ssid; }
const char *app_config_wifi_pass(void) { return s_wifi_pass; }
const char *app_config_pp_host(void)   { return s_pp_host; }
uint16_t    app_config_pp_port(void)   { return s_pp_port; }

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

bool app_config_set_pp_host(const char *host)
{
    if (!host || strlen(host) >= sizeof(s_pp_host)) return false;
    if (!save_str(NVS_KEY_PP_HOST, host)) return false;
    strncpy(s_pp_host, host, sizeof(s_pp_host) - 1);
    s_pp_host[sizeof(s_pp_host) - 1] = '\0';
    return true;
}

bool app_config_set_pp_port(uint16_t port)
{
    if (port == 0) return false;
    if (!save_u16(NVS_KEY_PP_PORT, port)) return false;
    s_pp_port = port;
    return true;
}

// --- Saved networks --------------------------------------------------------

static int saved_find(const char *ssid)
{
    if (!ssid || !*ssid) return -1;
    for (size_t i = 0; i < s_saved_count; ++i) {
        if (strcmp(s_saved[i].ssid, ssid) == 0) return (int) i;
    }
    return -1;
}

bool app_config_wifi_saved_add(const char *ssid, const char *pass)
{
    if (!ssid || !pass) return false;
    if (ssid[0] == '\0') return false;
    if (strlen(ssid) >= APP_CONFIG_SSID_MAX) return false;
    if (strlen(pass) >= APP_CONFIG_PASS_MAX) return false;

    int existing = saved_find(ssid);
    saved_net_t entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.ssid, ssid, sizeof(entry.ssid) - 1);
    strncpy(entry.pass, pass, sizeof(entry.pass) - 1);

    if (existing >= 0) {
        for (int i = existing; i > 0; --i) s_saved[i] = s_saved[i - 1];
        s_saved[0] = entry;
    } else {
        size_t cap  = APP_CONFIG_SAVED_NETWORKS_MAX;
        size_t keep = s_saved_count < cap ? s_saved_count : cap - 1;
        for (int i = (int) keep; i > 0; --i) s_saved[i] = s_saved[i - 1];
        s_saved[0] = entry;
        s_saved_count = keep + 1;
    }
    return persist_saved_networks();
}

bool app_config_wifi_saved_remove(const char *ssid)
{
    int idx = saved_find(ssid);
    if (idx < 0) return false;
    for (size_t i = (size_t) idx + 1; i < s_saved_count; ++i) {
        s_saved[i - 1] = s_saved[i];
    }
    s_saved_count--;
    memset(&s_saved[s_saved_count], 0, sizeof(saved_net_t));
    return persist_saved_networks();
}

size_t app_config_wifi_saved_count(void) { return s_saved_count; }

bool app_config_wifi_saved_get(size_t index,
                               char *ssid_out, size_t ssid_size,
                               char *pass_out, size_t pass_size)
{
    if (index >= s_saved_count) return false;
    if (!ssid_out || ssid_size == 0) return false;
    if (!pass_out || pass_size == 0) return false;
    strncpy(ssid_out, s_saved[index].ssid, ssid_size - 1);
    ssid_out[ssid_size - 1] = '\0';
    strncpy(pass_out, s_saved[index].pass, pass_size - 1);
    pass_out[pass_size - 1] = '\0';
    return true;
}

bool app_config_wifi_saved_lookup(const char *ssid,
                                  char *pass_out, size_t pass_size)
{
    int idx = saved_find(ssid);
    if (idx < 0) return false;
    if (!pass_out || pass_size == 0) return false;
    strncpy(pass_out, s_saved[idx].pass, pass_size - 1);
    pass_out[pass_size - 1] = '\0';
    return true;
}
