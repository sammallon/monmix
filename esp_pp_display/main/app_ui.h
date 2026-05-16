#pragma once

// Phase A stub: bring up a splash screen with a status line. Full stage-
// display UI (current/next slide, timer, stage message) lands in Phase C.

// Build the root UI under lvgl_port_lock. Returns once the splash is
// visible. Safe to call once after app_display_init has succeeded.
void app_ui_init(void);

// Replace the status line text. Safe to call from any task — internally
// takes lvgl_port_lock around the LVGL touch. NULL is treated as "".
void app_ui_set_status(const char *text);
