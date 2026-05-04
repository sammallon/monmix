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
    APP_LEVEL_FORMAT_DB,           // dB string; floor renders "-INF dB"
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

// Backlight brightness, percent 5..100. Floor of 5% prevents a fully-dark
// mis-tap; the slider itself enforces the floor at the UI layer too.
uint8_t app_prefs_get_brightness_pct(void);
void    app_prefs_set_brightness_pct(uint8_t pct);

// Per-channel color tag (palette index 0..7). Returns -1 if no preference
// is set; callers should fall back to a default palette derived from the
// channel id. set with `index < 0` to clear.
int  app_prefs_get_channel_color(int ms_channel_id);
void app_prefs_set_channel_color(int ms_channel_id, int index);

// Subscribe to pref changes -- typically the UI registers once at init
// time and re-reads whatever it cares about on each notification.
void app_prefs_register_on_change(app_prefs_on_change_t cb, void *ctx);

// Diagnostic dump used by the `prefs-dump` console command. Prints each
// key's NVS-side and SD-side state (value + mtime) so the verification
// scripts can confirm conflict-resolution + missing-key paths landed.
void app_prefs_debug_dump(void);
