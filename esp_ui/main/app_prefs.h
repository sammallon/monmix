#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Persistent local preferences. NVS is primary (one blob per key plus a
// parallel u64 mtime). SD-mirror at /sdcard/monmix-prefs.json is a backup
// that lets the user copy or edit prefs offline. On boot, per-key newest
// mtime wins; the loser side is overwritten. If neither side has a value
// the compile-time default is installed and explicitly persisted to both
// places so the next boot finds an authoritative entry.
//
// mtimes are uptime-relative monotonic counters — wall-clock isn't
// available without NTP. The implementation guarantees per-source
// monotonicity by seeding `s_mtime_floor` from the max mtime seen across
// NVS+SD at init and incrementing past it on every commit.

typedef enum {
    APP_LEVEL_FORMAT_NORM = 0,    // 0..100 raw integer (matches the slider)
    APP_LEVEL_FORMAT_DB,           // dB string; floor renders "-inf dB"
} app_level_format_t;

typedef enum {
    APP_SIGNAL_INDICATOR_NONE = 0,
    APP_SIGNAL_INDICATOR_PRESENT,   // single dot, ~5 Hz polling
    APP_SIGNAL_INDICATOR_METER,     // full bar, ~10 Hz polling
} app_signal_indicator_t;

typedef enum {
    APP_THEME_DARK = 0,             // default — low-light stage use
    APP_THEME_LIGHT,
} app_theme_t;

// Display rotation in degrees. Only 0 and 180 supported -- the panel is
// landscape and 90/270 would reflow the entire fader UI.
typedef enum {
    APP_DISPLAY_ROTATION_0   = 0,
    APP_DISPLAY_ROTATION_180 = 180,
} app_display_rotation_t;

// Notification fired whenever any preference changes. Multiple
// subscribers allowed; each callback receives its registered ctx pointer.
typedef void (*app_prefs_on_change_t)(void *ctx);

// Reconcile NVS + SD at boot. Per-key mtime comparison; loser side gets
// rewritten. Missing keys get the default and are committed. Safe to call
// before SD is mounted (NVS-only path) -- but call it AFTER app_storage
// so the SD mirror gets seeded.
void app_prefs_init(void);

app_level_format_t app_prefs_get_level_format(void);
void               app_prefs_set_level_format(app_level_format_t f);

app_signal_indicator_t app_prefs_get_signal_indicator(void);
void                   app_prefs_set_signal_indicator(app_signal_indicator_t s);

app_theme_t app_prefs_get_theme(void);
void        app_prefs_set_theme(app_theme_t t);

app_display_rotation_t app_prefs_get_display_rotation(void);
void                   app_prefs_set_display_rotation(app_display_rotation_t r);

// Backlight brightness, percent 5..100. Floor of 5% prevents a fully-dark
// mis-tap; the slider itself enforces the floor at the UI layer too.
uint8_t app_prefs_get_brightness_pct(void);
void    app_prefs_set_brightness_pct(uint8_t pct);

// Last mix bus the user picked (0-based; "Mix 1" in MS UI = index 0).
// The boot path validates this against the actual mix count from
// /console/information and falls back to 0 if the saved index is out of
// range (and persists the fallback so the change survives reboots).
uint8_t app_prefs_get_selected_mix_index(void);
void    app_prefs_set_selected_mix_index(uint8_t idx);

// Per-channel color tag (palette index 0..7). Returns -1 if no preference
// is set; callers should fall back to a default palette derived from the
// channel id. set with `index < 0` to clear.
int  app_prefs_get_channel_color(int ms_channel_id);
void app_prefs_set_channel_color(int ms_channel_id, int index);

// WiFi static IP toggle + dotted-quad strings. When the toggle is false the
// IP / netmask / gateway / DNS values are ignored and DHCP runs as usual.
// Strings are stored as IPv4 dotted form ("192.168.1.50"); empty string
// means "fall back to a sensible default" (zeros). Max in-buffer length is
// 16 (15 chars + NUL).
#define APP_PREFS_IP_STR_MAX 16

bool app_prefs_get_wifi_use_static(void);
void app_prefs_set_wifi_use_static(bool on);

// Each getter writes into the caller's buffer (must be APP_PREFS_IP_STR_MAX
// or larger) and returns the same pointer; the value is "" when the pref
// isn't set. Setters reject values longer than 15 chars.
const char *app_prefs_get_wifi_static_ip      (char *out, size_t out_len);
const char *app_prefs_get_wifi_static_netmask (char *out, size_t out_len);
const char *app_prefs_get_wifi_static_gateway (char *out, size_t out_len);
const char *app_prefs_get_wifi_static_dns     (char *out, size_t out_len);
void        app_prefs_set_wifi_static_ip      (const char *s);
void        app_prefs_set_wifi_static_netmask (const char *s);
void        app_prefs_set_wifi_static_gateway (const char *s);
void        app_prefs_set_wifi_static_dns     (const char *s);

// NTP server hostname (or IP) and POSIX TZ string. Both freeform; the
// caller is trusted (hand-typed via the on-device keyboard). Buffers
// must be at least APP_PREFS_STR_MAX. Defaults are pool.ntp.org and a
// US-Pacific TZ string -- the user is expected to override TZ at first
// run if elsewhere. setenv("TZ",...) drives localtime_r; logs format
// from monotonic uptime so they stay TZ-independent.
#define APP_PREFS_STR_MAX 64

const char *app_prefs_get_ntp_server     (char *out, size_t out_len);
void        app_prefs_set_ntp_server     (const char *s);
const char *app_prefs_get_display_tz     (char *out, size_t out_len);
void        app_prefs_set_display_tz     (const char *s);

// When true, a DHCP-supplied NTP server (option 42) takes priority and the
// user's manual ntp_server is the fallback. When false, only the manual
// server is used. Default true.
bool app_prefs_get_ntp_use_dhcp(void);
void app_prefs_set_ntp_use_dhcp(bool on);

// Subscribe to pref changes -- typically the UI registers once at init
// time and re-reads whatever it cares about on each notification.
void app_prefs_register_on_change(app_prefs_on_change_t cb, void *ctx);

// Diagnostic dump used by the `prefs-dump` console command. Prints each
// key's NVS-side and SD-side state (value + mtime) so the verification
// scripts can confirm conflict-resolution + missing-key paths landed.
void app_prefs_debug_dump(void);
