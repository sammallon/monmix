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

// Tell the UI the total channel count on the connected console (any
// channel type, since the picker treats them uniformly -- the user just
// wants to pick which 16 of the N strips they care about). The picker
// renders one row per channel id 0..count-1.
void app_ui_set_channel_total(int count);

// User-facing maximum number of channels that can be tracked at once.
// 22 = 2 pages * 11 input strips per page (the master strip is pinned
// outside the tileview and counts separately, so 11 inputs + master =
// 12 visible at once). Storage cap APP_CONFIG_MAX_CHANNELS is higher;
// this is the editable-UI cap.
#define APP_UI_MAX_TRACKED_CHANNELS 22

// Maximum number of channel rows the picker can render. The picker shows
// every channel id reported by the connected console (any type), so this
// has to cover the largest reasonable totalChannels value. Si Expression
// reports 80; we allow some headroom for other consoles.
#define APP_UI_MAX_PICKER_ROWS 128

// Update the status line at the top of the screen. Safe to call from any
// task; uses lv_async_call internally. Calls made before app_ui_init runs
// are no-ops.
void app_ui_set_status(const char *text);

// P5: force the mix-indicator visible regardless of the WS-connected /
// mix-list-ready gate. Pass false to release the override and revert to
// the gated path. Used by the `mix-show` console command for diagnosis.
void app_ui_force_mix_show(bool force);

// Test hook: apply a new tracked-channel selection live (the same path
// the channel-picker Save flow takes after persisting to NVS). Used by
// pc_sim's chpick_save script command so a regression can exercise the
// stop+rebuild+restart lifecycle without driving the picker UI.
// Persists to NVS, reseeds app_state, rebuilds faders, restarts the MS
// worker. Caller must hold lvgl_port_lock.
void app_ui_chpick_apply(const int *ids, size_t count);

// Test hook: print one "settings_tile i=<i> x=<x> y=<y> name_x=<nx> swatch_x=<sx>"
// line per channel tile in the settings overlay's grid, for asserting
// column-major ordering and swatch-on-left placement. The settings
// overlay must be open when called. Caller must hold lvgl_port_lock.
void app_ui_settings_dump_tiles(void);
