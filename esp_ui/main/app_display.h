#pragma once

#include <stdbool.h>

#include "app_prefs.h"

// Brings up MIPI-DSI panel + GT911 touch + LVGL via esp_lvgl_port.
bool app_display_init(void);

// Re-apply the LVGL theme to the default display. Safe to call from any task
// — internally takes lvgl_port_lock. Used by the prefs-change subscriber to
// flip light/dark at runtime.
void app_display_apply_theme(app_theme_t theme);
