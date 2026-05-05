// Mock app_wifi: pretend we're permanently CONNECTED so the wifi badge
// in the status bar is steady. No scan results — the Settings overlay's
// SSID list will just show "(scanning…)".
#include "app_wifi.h"

#include <string.h>
#include <stdio.h>

#define MAX_OBS 4
static struct { app_wifi_on_change_t cb; void *ctx; } s_obs[MAX_OBS];
static size_t s_obs_n;

void app_wifi_init_radio(void) {}
bool app_wifi_wait_connected(void) { return true; }
void app_wifi_apply_ip_config(void) {}
bool app_wifi_reconfigure(void) { return true; }

app_wifi_state_t app_wifi_get_state(void) { return APP_WIFI_STATE_CONNECTED; }
const char      *app_wifi_get_ssid(void)  { return "sim-wifi"; }

void app_wifi_format_ip(char *buf, size_t buflen) {
    if (buf && buflen > 0) {
        snprintf(buf, buflen, "127.0.0.1");
    }
}

void app_wifi_register_on_change(app_wifi_on_change_t cb, void *ctx) {
    if (s_obs_n < MAX_OBS) { s_obs[s_obs_n].cb = cb; s_obs[s_obs_n].ctx = ctx; s_obs_n++; }
}

app_wifi_scan_result_t app_wifi_scan_start(app_wifi_scan_done_t done_cb, void *ctx) {
    (void)done_cb; (void)ctx;
    return APP_WIFI_SCAN_FAILED;
}

size_t app_wifi_scan_results(char (*dst)[33], size_t max_count) {
    (void)dst; (void)max_count;
    return 0;
}
