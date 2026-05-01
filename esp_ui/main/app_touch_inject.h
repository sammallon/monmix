#pragma once

#include <stdbool.h>

// Synthetic touch injection for closed-loop UI testing from the host.
//
// Registers a virtual LVGL pointer indev alongside the real GT911 touch
// driver. LVGL polls both indevs at refresh time; whichever is "pressed"
// drives input. The console `touch` command sets state here, the read
// callback reports it, LVGL processes it as a normal touch event.
//
// Coordinates are LVGL LOGICAL pixels — same frame as `tools/fetch_screenshot.py`
// produces, so what you see in a screenshot maps 1:1 to what `touch x y`
// will hit. (The panel's 180° software rotation happens at flush time
// after hit-testing, so input doesn't need to be rotated.)
//
// Call after app_display_init() — LVGL must be initialised first.
void app_touch_inject_init(void);

// Set the virtual touch state. Safe from any task — internally takes
// lvgl_port_lock to serialize against the LVGL polling task.
void app_touch_inject_set(int x, int y, bool pressed);
