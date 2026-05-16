#pragma once

#include <stdbool.h>

// Networked OTA. Pulls a firmware binary from `url` (http only, no TLS
// in v1 -- LAN is trusted). Returns true if the OTA started; the
// progress + result are reported asynchronously via
// app_netconsole_send_line() so a connected client can observe.
//
// Concurrency: only one OTA at a time. A second call while an OTA is
// in flight returns false.
//
// On success, the new firmware is written to the inactive ota_<n>
// partition, the bootloader's boot-target is flipped, and the device
// reboots automatically. The next boot runs the new image in
// PENDING_VERIFY state until app_ota_mark_running_valid() confirms it
// (see below).
bool app_ota_start(const char *url);

// Bootloader rollback support: every fresh OTA boots in
// PENDING_VERIFY state. If we reboot before the running image is
// marked valid, the bootloader reverts to the previous ota_<n> slot.
// Call this once the new firmware has demonstrated basic health
// (WiFi associates, LVGL UI mounted). app_netconsole_init schedules
// a one-shot timer that runs this after a settling window.
//
// Idempotent + safe to call on non-OTA boots: just returns ESP_OK.
void app_ota_mark_running_valid(void);

// True while an OTA is downloading or finalising. UI can use this to
// suppress sleep, hide unrelated overlays, etc.
bool app_ota_in_progress(void);
