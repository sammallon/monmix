#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_prefs.h"

// Brings up MIPI-DSI panel + GT911 touch + LVGL via esp_lvgl_port.
bool app_display_init(void);

// Re-apply the LVGL theme to the default display. Safe to call from any task
// — internally takes lvgl_port_lock. Used by the prefs-change subscriber to
// flip light/dark at runtime.
void app_display_apply_theme(app_theme_t theme);

// Drive the backlight LEDC PWM duty to the given percentage. Clamped to
// 5..100 -- a fully-dark mis-tap leaves no non-touch recovery path.
void app_display_set_backlight_pct(uint8_t pct);
