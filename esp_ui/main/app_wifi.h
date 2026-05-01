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
