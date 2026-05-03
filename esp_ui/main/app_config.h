#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Cap on tracked channels. Si Expression 2 has 24 input channels; 24 covers
// the worst case where a musician wants every channel on a single device.
// Practical defaults are far smaller.
#define APP_CONFIG_MAX_CHANNELS 24

// 802.11 SSID is max 32 octets; password is max 64 octets (WPA2/WPA3).
// Plus one byte for the NUL terminator on each.
#define APP_CONFIG_SSID_MAX 33
#define APP_CONFIG_PASS_MAX 65
#define APP_CONFIG_HOST_MAX 64

// Load all persisted config (channel list + wifi/ms creds) from NVS, seeding
// from secrets.h compile-time defaults on first boot. Always succeeds in
// memory even if NVS itself fails (in-memory defaults).
//
// Must be called after nvs_flash_init.
void app_config_init(void);

// Channel selection.
const int *app_config_channel_ids(size_t *out_count);

// WiFi/MS credentials. Getters return pointers to internal RAM that stays
// valid until the next set_* call (or app_config_init re-seed). Setters
// persist immediately to NVS but DO NOT reapply -- callers are responsible
// for triggering reconnect or reboot. Returning false means the new value
// was rejected (too long) or NVS write failed; the in-memory value is
// unchanged in either case.
const char *app_config_wifi_ssid(void);
const char *app_config_wifi_pass(void);
const char *app_config_ms_host(void);
uint16_t    app_config_ms_port(void);

bool app_config_set_wifi_ssid(const char *ssid);
bool app_config_set_wifi_pass(const char *pass);
bool app_config_set_ms_host(const char *host);
bool app_config_set_ms_port(uint16_t port);
