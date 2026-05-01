#pragma once

#include <stdbool.h>

// Local-only preferences persisted to `/sdcard/monmix-prefs.json`. These
// are the user's UI choices (level readout format, channel colors, signal
// indicator mode, ...) that should NOT live in NVS or come from Mixing
// Station — they belong on the SD card so they survive NVS erase /
// reflash and can be copied off.
//
// Falls back to compile-time defaults when SD didn't mount or the file is
// missing/malformed. Init may be called before SD bring-up; in that case
// it's a no-op and the file gets written on the first set_*() call after
// the SD is up.

typedef enum {
    APP_LEVEL_FORMAT_NORM = 0,   // 0..100 raw integer (matches the slider)
    APP_LEVEL_FORMAT_DB,          // dB string; "-∞ dB" at the channel's min
} app_level_format_t;

typedef enum {
    APP_SIGNAL_INDICATOR_NONE = 0,
    APP_SIGNAL_INDICATOR_PRESENT,   // single dot, ~5 Hz polling
    APP_SIGNAL_INDICATOR_METER,     // full bar, ~10 Hz polling
} app_signal_indicator_t;

// Notification fired whenever any preference changes. Multiple
// subscribers allowed; each callback receives its registered ctx pointer.
typedef void (*app_prefs_on_change_t)(void *ctx);

// Load (or create with defaults) `/sdcard/monmix-prefs.json`. Safe to
// call when SD is unmounted — values stay at defaults until the next
// init that succeeds.
void app_prefs_init(void);

app_level_format_t app_prefs_get_level_format(void);
void               app_prefs_set_level_format(app_level_format_t f);

app_signal_indicator_t app_prefs_get_signal_indicator(void);
void                   app_prefs_set_signal_indicator(app_signal_indicator_t s);

// Per-channel color tag (palette index 0..7). Returns -1 if no preference
// is set; callers should fall back to a default palette derived from the
// channel id. set with `index < 0` to clear.
int  app_prefs_get_channel_color(int ms_channel_id);
void app_prefs_set_channel_color(int ms_channel_id, int index);

// Subscribe to pref changes — typically the UI registers once at init
// time and re-reads whatever it cares about on each notification.
void app_prefs_register_on_change(app_prefs_on_change_t cb, void *ctx);
