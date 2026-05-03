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

// Tell the UI how many mix buses are available (from /console/information's
// Mix channelType). The mix selector popup populates with this many buttons.
// Pass 0 to hide the selector entirely. Default before this is called: 0.
void app_ui_set_mix_count(int count);

// Tell the UI the input-channel range available on the connected console
// (Input channelType from /console/information). The channel picker uses
// this to render a row per available input. Pass 0 count to disable the
// picker. Default before this is called: 0/0.
void app_ui_set_input_range(int offset, int count);

// User-facing maximum number of channels that can be tracked at once.
// Currently 16 (per #33: typical monitor mix size + LVGL build-time bound
// at boot). Storage cap APP_CONFIG_MAX_CHANNELS is higher; this is the
// editable-UI cap.
#define APP_UI_MAX_TRACKED_CHANNELS 16

// Update the status line at the top of the screen. Safe to call from any
// task; uses lv_async_call internally. Calls made before app_ui_init runs
// are no-ops.
void app_ui_set_status(const char *text);
