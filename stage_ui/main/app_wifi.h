// WiFi iface — slimmed from esp_ui/main/app_wifi.h. Same public surface
// shape so the WiFi config panel can port across products with minimal
// edits. Sim mock reports CONNECTED and fakes scan results; hardware
// round replaces with the real ESP-Hosted-backed implementation.

#ifndef APP_WIFI_H
#define APP_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    APP_WIFI_STATE_BOOT = 0,
    APP_WIFI_STATE_CONNECTING,
    APP_WIFI_STATE_CONNECTED,
    APP_WIFI_STATE_FAILED,
} app_wifi_state_t;

typedef void (*app_wifi_on_change_t)(void *ctx);

void app_wifi_init_radio(void);
bool app_wifi_wait_connected(void);

// Live reconfigure: pick up the latest SSID/password from app_prefs and
// trigger a clean disconnect+reconnect WITHOUT a reboot. Mirrors the
// pattern esp_ui uses post-pilot.
bool app_wifi_reconfigure(void);

app_wifi_state_t app_wifi_get_state(void);
const char      *app_wifi_get_ssid(void);
void             app_wifi_format_ip(char *buf, size_t buflen);
const char      *app_wifi_get_security_str(void);

void app_wifi_register_on_change(app_wifi_on_change_t cb, void *ctx);

#define APP_WIFI_SCAN_MAX_RESULTS 24

typedef void (*app_wifi_scan_done_t)(void *ctx);

typedef enum {
    APP_WIFI_SCAN_STARTED,
    APP_WIFI_SCAN_ALREADY_RUNNING,
    APP_WIFI_SCAN_FAILED,
} app_wifi_scan_result_t;

app_wifi_scan_result_t app_wifi_scan_start(app_wifi_scan_done_t done_cb, void *ctx);
size_t                 app_wifi_scan_results(char (*dst)[33], size_t max_count);

#endif // APP_WIFI_H
