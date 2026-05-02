#pragma once

#include "app_ms_client.h"

// Build the status bar (icons, clock, MUTE EN, settings gear) and the
// loading spinner. Does NOT build the fader strips — those are deferred
// until the channel list is known. Wire your channel discovery (MS info
// fetch, NVS, fallback default) and call app_ui_present_channels once
// it's ready.
void app_ui_init(const ms_client_iface_t *ms);

// Build (or rebuild) the fader strips from the current app_state contents.
// Called from a non-LVGL task; takes lvgl_port_lock internally. Safe to
// call multiple times — on subsequent calls, the previous tileview is
// destroyed first.
void app_ui_present_channels(void);

// Update the status line at the top of the screen. Safe to call from any
// task; uses lv_async_call internally. Calls made before app_ui_init runs
// are no-ops.
void app_ui_set_status(const char *text);
