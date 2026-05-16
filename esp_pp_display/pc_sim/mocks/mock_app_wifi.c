// Mock app_wifi. The PC sim doesn't have WiFi; we pretend to be associated
// so app_ui's wifi-status icon shows green. Functions called by app_ui:
//   app_wifi_get_state, app_wifi_register_on_change.
// Other app_wifi functions are stubbed to satisfy the linker if accidentally
// pulled in via headers; they should not be reached from the UI path.
#include "app_wifi.h"

#include <stdio.h>
#include <string.h>

#define MAX_OBS 4

typedef struct { app_wifi_on_change_t cb; void *ctx; } obs_t;
static obs_t s_obs[MAX_OBS];
static size_t s_obs_count;
static app_wifi_state_t s_state = APP_WIFI_STATE_CONNECTED;

void app_wifi_init_radio(void) {}
bool app_wifi_wait_connected(void) { return true; }
void app_wifi_apply_ip_config(void) {}
bool app_wifi_reconfigure(void) { return true; }

app_wifi_state_t app_wifi_get_state(void) { return s_state; }
const char *app_wifi_get_ssid(void) { return "MockNet"; }

void app_wifi_format_ip(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return;
    snprintf(buf, buflen, "192.168.1.100");
}

const char *app_wifi_get_security_str(void) { return "WPA2-PSK"; }

void app_wifi_register_on_change(app_wifi_on_change_t cb, void *ctx)
{
    if (!cb) return;
    if (s_obs_count < MAX_OBS) {
        s_obs[s_obs_count].cb  = cb;
        s_obs[s_obs_count].ctx = ctx;
        s_obs_count++;
    }
}

// Test helper -- called from pc_main to flip WiFi state and exercise the
// header-icon path.
void mock_wifi_set_state(app_wifi_state_t s)
{
    if (s == s_state) return;
    s_state = s;
    for (size_t i = 0; i < s_obs_count; ++i) s_obs[i].cb(s_obs[i].ctx);
}

app_wifi_scan_result_t app_wifi_scan_start(app_wifi_scan_done_t done_cb, void *ctx) {
    (void) done_cb; (void) ctx; return APP_WIFI_SCAN_FAILED;
}
size_t app_wifi_scan_results(char (*dst)[33], size_t max_count) {
    (void) dst; (void) max_count; return 0;
}
bool app_wifi_force_reassociate(uint32_t timeout_ms) {
    (void) timeout_ms; return true;
}
