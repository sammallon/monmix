// Slim app_prefs for stage_ui.
//
// Same shape as esp_ui/main/app_prefs.h: typed getters/setters per key,
// subscriber-notify on change. Storage backing in this skeleton is
// in-memory only; the hardware-port round adds the NVS-primary +
// SD-mirror persistence that esp_ui uses.

#ifndef APP_PREFS_H
#define APP_PREFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    APP_THEME_DARK = 0,     // default — low-light stage use
    APP_THEME_LIGHT,
} app_theme_t;

// Only 0 and 180 supported. Per the USB-C ergonomics note, 90/270 is
// indefinitely deferred (would put the connector on top/bottom of the
// device).
typedef enum {
    APP_DISPLAY_ROTATION_0   = 0,
    APP_DISPLAY_ROTATION_180 = 180,
} app_display_rotation_t;

#define APP_PREFS_STR_MAX 64

typedef void (*app_prefs_on_change_t)(void *ctx);

void app_prefs_init(void);
void app_prefs_register_on_change(app_prefs_on_change_t cb, void *ctx);

// Theme.
app_theme_t app_prefs_get_theme(void);
void        app_prefs_set_theme(app_theme_t t);

// Rotation (0 or 180).
app_display_rotation_t app_prefs_get_display_rotation(void);
void                   app_prefs_set_display_rotation(app_display_rotation_t r);

// Backlight 5..100. Floor of 5% so a misadjusted slider can't render
// the panel fully black.
uint8_t app_prefs_get_brightness_pct(void);
void    app_prefs_set_brightness_pct(uint8_t pct);

// Idle sleep timeout (seconds). UI exposes 15s..30min. Degraded-state
// cap (60s) lives in app_power.c, not here.
uint32_t app_prefs_get_sleep_timeout_sec(void);
void     app_prefs_set_sleep_timeout_sec(uint32_t s);

// WiFi credentials. Buffers must be at least APP_PREFS_STR_MAX.
const char *app_prefs_get_wifi_ssid    (char *out, size_t out_len);
void        app_prefs_set_wifi_ssid    (const char *s);
const char *app_prefs_get_wifi_password(char *out, size_t out_len);
void        app_prefs_set_wifi_password(const char *s);

// ProPresenter target.
const char *app_prefs_get_pp_host(char *out, size_t out_len);
void        app_prefs_set_pp_host(const char *s);
uint16_t    app_prefs_get_pp_port(void);
void        app_prefs_set_pp_port(uint16_t p);

#endif // APP_PREFS_H
