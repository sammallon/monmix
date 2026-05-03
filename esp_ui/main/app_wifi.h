#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    APP_WIFI_STATE_BOOT = 0,    // before init
    APP_WIFI_STATE_CONNECTING,  // STA started, no IP yet
    APP_WIFI_STATE_CONNECTED,   // got IP
    APP_WIFI_STATE_FAILED,      // exhausted retries
} app_wifi_state_t;

typedef void (*app_wifi_on_change_t)(void *ctx);

// Phase 1: bring up the radio transport (ESP-Hosted to the on-board C6 over
// SDIO) so the SDMMC host controller is initialised. Returns immediately
// after esp_wifi_start(); does NOT wait for an SSID. Required before
// app_storage_init() because IDF v6's SDMMC host can only be init'd once
// and ESP-Hosted is the canonical owner.
void app_wifi_init_radio(void);

// Phase 2: block until the STA either acquires an IP or exhausts retries.
// Returns true on connect, false on permanent failure.
bool app_wifi_wait_connected(void);

// Live status — safe to call from any task. The IP buffer must hold at
// least 16 bytes ("xxx.xxx.xxx.xxx\0"); written even when state is not
// CONNECTED (in which case it's set to "0.0.0.0").
app_wifi_state_t app_wifi_get_state(void);
const char      *app_wifi_get_ssid(void);
void             app_wifi_format_ip(char *buf, size_t buflen);

// Subscribe to state changes (transitions between the enum values above).
// Each callback fires from the WiFi/IP event task — keep it short, defer
// LVGL work via lv_async_call.
void app_wifi_register_on_change(app_wifi_on_change_t cb, void *ctx);

// SSID scan support for the Network settings UI. The scan runs through the
// C6 (ESP-Hosted) and briefly disrupts the active connection; the watchdog
// + auto-reconnect path puts it back together within seconds.
//
// Result format: each entry is a NUL-terminated SSID. Hidden APs (empty
// SSID) are skipped from the result list since the user picks them by
// typing the SSID anyway. The list pointer remains valid until the next
// scan_start.
typedef void (*app_wifi_scan_done_t)(void *ctx);

#define APP_WIFI_SCAN_MAX_RESULTS 24

// Trigger an asynchronous scan. The done_cb fires from the WiFi event task
// when results are ready; call app_wifi_scan_results to read them.
// Returns false if a scan is already in progress.
bool app_wifi_scan_start(app_wifi_scan_done_t done_cb, void *ctx);

// Read the latest scan results. Returns the number of SSIDs filled in;
// caller passes a `dst` of at least max_count strings each at least
// 33 bytes. Safe to call from any task. Returns 0 if no scan completed.
size_t app_wifi_scan_results(char (*dst)[33], size_t max_count);
