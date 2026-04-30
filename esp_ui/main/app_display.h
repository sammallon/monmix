#pragma once

#include <stdbool.h>

// Brings up MIPI-DSI panel + GT911 touch + LVGL via esp_lvgl_port.
//
// M1 stub: returns true without doing real init. Real implementation lands
// once the 10.1" CrowPanel is in hand and we know:
//   - which panel IC actually shipped (ILI9881C vs JD9365)
//   - the exact GPIO/DSI lane assignments from the schematic
//   - the GT911 I2C bus + interrupt pins
bool app_display_init(void);

// Wraps lv_async_call for non-LVGL tasks (WS handler) to safely poke widgets.
// Same as lv_async_call directly; provided so app code doesn't include lvgl.h
// in the WS path. (Implementation lives alongside the real display init.)
