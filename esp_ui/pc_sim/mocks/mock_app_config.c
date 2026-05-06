// In-memory mock of app_config: a fixed channel list (so the UI mounts
// 8 strips by default) and stub credential getters. The bug we're chasing
// doesn't depend on persisted config, so nothing has to round-trip to disk.
#include "app_config.h"

#include <stdint.h>
#include <string.h>

#define DEFAULT_CHANNEL_COUNT 8
static int      s_channel_ids[APP_CONFIG_MAX_CHANNELS] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static size_t   s_channel_count = DEFAULT_CHANNEL_COUNT;
static char              s_wifi_ssid[33] = "sim-wifi";
static char              s_wifi_pass[65] = "sim-pass";
static char              s_ms_host[64]   = "127.0.0.1";
static uint16_t          s_ms_port       = 9000;
static app_ms_protocol_t s_ms_proto      = APP_MS_PROTOCOL_WS;
static uint16_t          s_ms_osc_port   = 3000;

void app_config_init(void) {}

const int *app_config_channel_ids(size_t *out_count) {
    if (out_count) *out_count = s_channel_count;
    return s_channel_ids;
}

bool app_config_set_channel_ids(const int *ids, size_t count) {
    if (count == 0 || count > APP_CONFIG_MAX_CHANNELS) return false;
    for (size_t i = 0; i < count; ++i) s_channel_ids[i] = ids[i];
    s_channel_count = count;
    return true;
}

const char        *app_config_wifi_ssid (void) { return s_wifi_ssid; }
const char        *app_config_wifi_pass (void) { return s_wifi_pass; }
const char        *app_config_ms_host   (void) { return s_ms_host;   }
uint16_t           app_config_ms_port   (void) { return s_ms_port;   }
app_ms_protocol_t  app_config_ms_protocol(void){ return s_ms_proto;  }
uint16_t           app_config_ms_osc_port(void){ return s_ms_osc_port; }

bool app_config_set_wifi_ssid(const char *s) { strncpy(s_wifi_ssid, s, 32); s_wifi_ssid[32] = 0; return true; }
bool app_config_set_wifi_pass(const char *s) { strncpy(s_wifi_pass, s, 64); s_wifi_pass[64] = 0; return true; }
bool app_config_set_ms_host  (const char *s) { strncpy(s_ms_host,   s, 63); s_ms_host[63]   = 0; return true; }
bool app_config_set_ms_port  (uint16_t   p)  { s_ms_port = p; return true; }
bool app_config_set_ms_protocol(app_ms_protocol_t p) { s_ms_proto = p; return true; }
bool app_config_set_ms_osc_port(uint16_t p)  { s_ms_osc_port = p; return true; }

// Sim-only seed: pc_main uses these to honor --protocol / --osc-port CLI
// flags before app_ui_init reads them.
void mock_app_config_seed_protocol(app_ms_protocol_t p) { s_ms_proto = p; }
void mock_app_config_seed_osc_port(uint16_t p)          { s_ms_osc_port = p; }

// In-memory mirror of the saved-networks ring -- enough for sim tests of
// the wcfg picker UI to exercise the saved/scanned merge path without
// faking NVS.
typedef struct { char ssid[APP_CONFIG_SSID_MAX]; char pass[APP_CONFIG_PASS_MAX]; } saved_net_t;
static saved_net_t s_saved[APP_CONFIG_SAVED_NETWORKS_MAX];
static size_t      s_saved_count;

static int saved_find(const char *ssid) {
    if (!ssid || !*ssid) return -1;
    for (size_t i = 0; i < s_saved_count; ++i) {
        if (strcmp(s_saved[i].ssid, ssid) == 0) return (int) i;
    }
    return -1;
}

bool app_config_wifi_saved_add(const char *ssid, const char *pass) {
    if (!ssid || !pass || !*ssid) return false;
    if (strlen(ssid) >= APP_CONFIG_SSID_MAX) return false;
    if (strlen(pass) >= APP_CONFIG_PASS_MAX) return false;
    int existing = saved_find(ssid);
    saved_net_t e = {0};
    strncpy(e.ssid, ssid, sizeof(e.ssid) - 1);
    strncpy(e.pass, pass, sizeof(e.pass) - 1);
    if (existing >= 0) {
        for (int i = existing; i > 0; --i) s_saved[i] = s_saved[i - 1];
        s_saved[0] = e;
    } else {
        size_t cap = APP_CONFIG_SAVED_NETWORKS_MAX;
        size_t keep = s_saved_count < cap ? s_saved_count : cap - 1;
        for (int i = (int) keep; i > 0; --i) s_saved[i] = s_saved[i - 1];
        s_saved[0] = e;
        s_saved_count = keep + 1;
    }
    return true;
}

bool app_config_wifi_saved_remove(const char *ssid) {
    int idx = saved_find(ssid);
    if (idx < 0) return false;
    for (size_t i = (size_t) idx + 1; i < s_saved_count; ++i) s_saved[i - 1] = s_saved[i];
    s_saved_count--;
    memset(&s_saved[s_saved_count], 0, sizeof(saved_net_t));
    return true;
}

size_t app_config_wifi_saved_count(void) { return s_saved_count; }

bool app_config_wifi_saved_get(size_t index,
                               char *ssid_out, size_t ssid_size,
                               char *pass_out, size_t pass_size) {
    if (index >= s_saved_count) return false;
    if (!ssid_out || !pass_out || ssid_size == 0 || pass_size == 0) return false;
    strncpy(ssid_out, s_saved[index].ssid, ssid_size - 1); ssid_out[ssid_size - 1] = 0;
    strncpy(pass_out, s_saved[index].pass, pass_size - 1); pass_out[pass_size - 1] = 0;
    return true;
}

bool app_config_wifi_saved_lookup(const char *ssid, char *pass_out, size_t pass_size) {
    int idx = saved_find(ssid);
    if (idx < 0 || !pass_out || pass_size == 0) return false;
    strncpy(pass_out, s_saved[idx].pass, pass_size - 1); pass_out[pass_size - 1] = 0;
    return true;
}
