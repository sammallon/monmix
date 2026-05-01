#pragma once

#include <stdbool.h>

// Phase 1: bring up the radio transport (ESP-Hosted to the on-board C6 over
// SDIO) so the SDMMC host controller is initialised. Returns immediately
// after esp_wifi_start(); does NOT wait for an SSID. Required before
// app_storage_init() because IDF v6's SDMMC host can only be init'd once
// and ESP-Hosted is the canonical owner.
void app_wifi_init_radio(void);

// Phase 2: block until the STA either acquires an IP or exhausts retries.
// Returns true on connect, false on permanent failure.
bool app_wifi_wait_connected(void);
