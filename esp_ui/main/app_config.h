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

// Persist a new channel selection. Returns false if count is 0 or
// exceeds APP_CONFIG_MAX_CHANNELS, or if the NVS write fails. The
// in-memory list is updated on success; callers reboot to rebuild
// the fader UI against the new set.
bool app_config_set_channel_ids(const int *ids, size_t count);

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

// Saved-networks ring. Holds up to APP_CONFIG_SAVED_NETWORKS_MAX (SSID,
// password) pairs in NVS so a venue change doesn't force the user to retype
// the password they used last time. Most-recently-saved first; adding an
// SSID that's already present moves it to the front and updates its
// password. Eviction at the tail when full.
//
// The list is rewritten as a single blob on each mutation; small N + low
// write frequency keeps NVS wear-cost negligible.
#define APP_CONFIG_SAVED_NETWORKS_MAX 8

// Append-or-promote: if `ssid` already exists, its password is updated and
// it moves to index 0; otherwise a new entry is prepended and the oldest is
// evicted. Empty SSID is rejected. Returns false on validation or NVS error.
bool app_config_wifi_saved_add(const char *ssid, const char *pass);

// Remove the entry whose SSID matches; no-op if not found.
bool app_config_wifi_saved_remove(const char *ssid);

// Number of saved entries currently held.
size_t app_config_wifi_saved_count(void);

// Read entry at `index`. ssid_out / pass_out must be at least
// APP_CONFIG_SSID_MAX / APP_CONFIG_PASS_MAX bytes. Returns false on bad
// index or null buffer.
bool app_config_wifi_saved_get(size_t index,
                               char *ssid_out, size_t ssid_size,
                               char *pass_out, size_t pass_size);

// Find the password saved for `ssid`. Returns false if `ssid` not in list
// or pass_out too small. pass_out is NUL-terminated on success.
bool app_config_wifi_saved_lookup(const char *ssid,
                                  char *pass_out, size_t pass_size);
