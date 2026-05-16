#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 802.11 SSID is max 32 octets; password is max 64 octets (WPA2/WPA3).
// Plus NUL terminator.
#define APP_CONFIG_SSID_MAX 33
#define APP_CONFIG_PASS_MAX 65

// ProPresenter host buffer. Accommodates an IPv4 dotted quad ("xxx.xxx.xxx.xxx")
// or a moderately long DNS name. Match esp_ui's MS_HOST sizing for consistency.
#define APP_CONFIG_HOST_MAX 64

// Load all persisted config (wifi/PP creds + saved networks) from NVS,
// seeding from secrets.h compile-time defaults on first boot. Always
// succeeds in memory even if NVS itself fails (in-memory defaults).
//
// Must be called after nvs_flash_init.
void app_config_init(void);

// WiFi creds. Getters return pointers to internal RAM that stays valid
// until the next set_* call (or app_config_init re-seed). Setters
// persist immediately to NVS but DO NOT reapply -- callers are responsible
// for triggering reconnect or reboot.
const char *app_config_wifi_ssid(void);
const char *app_config_wifi_pass(void);
bool        app_config_set_wifi_ssid(const char *ssid);
bool        app_config_set_wifi_pass(const char *pass);

// ProPresenter host + port. Port reaches the PP TCP/socket API
// (default 63306, configurable in PP Settings > Network).
const char *app_config_pp_host(void);
uint16_t    app_config_pp_port(void);
bool        app_config_set_pp_host(const char *host);
bool        app_config_set_pp_port(uint16_t port);

// Saved-networks ring. Up to N (SSID, password) pairs so a venue change
// doesn't force the user to retype. Most-recently-saved first; re-adding
// an existing SSID moves it to the front and updates its password.
// Eviction at the tail when full.
#define APP_CONFIG_SAVED_NETWORKS_MAX 8

bool   app_config_wifi_saved_add(const char *ssid, const char *pass);
bool   app_config_wifi_saved_remove(const char *ssid);
size_t app_config_wifi_saved_count(void);
bool   app_config_wifi_saved_get(size_t index,
                                 char *ssid_out, size_t ssid_size,
                                 char *pass_out, size_t pass_size);
bool   app_config_wifi_saved_lookup(const char *ssid,
                                    char *pass_out, size_t pass_size);
