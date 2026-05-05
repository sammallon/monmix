// In-memory mock of app_config: a fixed channel list (so the UI mounts
// 8 strips by default) and stub credential getters. The bug we're chasing
// doesn't depend on persisted config, so nothing has to round-trip to disk.
#include "app_config.h"

#include <stdint.h>
#include <string.h>

#define DEFAULT_CHANNEL_COUNT 8
static int      s_channel_ids[APP_CONFIG_MAX_CHANNELS] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static size_t   s_channel_count = DEFAULT_CHANNEL_COUNT;
static char     s_wifi_ssid[33] = "sim-wifi";
static char     s_wifi_pass[65] = "sim-pass";
static char     s_ms_host[64]   = "127.0.0.1";
static uint16_t s_ms_port       = 9000;

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

const char *app_config_wifi_ssid(void) { return s_wifi_ssid; }
const char *app_config_wifi_pass(void) { return s_wifi_pass; }
const char *app_config_ms_host (void) { return s_ms_host;   }
uint16_t    app_config_ms_port (void) { return s_ms_port;   }

bool app_config_set_wifi_ssid(const char *s) { strncpy(s_wifi_ssid, s, 32); s_wifi_ssid[32] = 0; return true; }
bool app_config_set_wifi_pass(const char *s) { strncpy(s_wifi_pass, s, 64); s_wifi_pass[64] = 0; return true; }
bool app_config_set_ms_host  (const char *s) { strncpy(s_ms_host,   s, 63); s_ms_host[63]   = 0; return true; }
bool app_config_set_ms_port  (uint16_t   p)  { s_ms_port = p; return true; }
