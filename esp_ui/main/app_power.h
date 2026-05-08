#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_ms_client.h"

// Display power management (M7).
//
// Idle-timeout blanks the panel after the user-selected duration; the
// duration is picked from a wake-time menu (1/2/4/8/12/24 h) so a
// stale "always-on" preference can't leave the panel running when the
// user steps away. A 30 s warning dialog with live countdown precedes
// blanking; any touch during the warning cancels it and resets the
// idle counter. After blank, a touch shows the wake menu; no
// selection within 30 s of wake re-blanks.
//
// Connectivity-degraded states (no WiFi, MS not connected, MS up but
// the physical console isn't attached) cap the effective timeout at
// 60 s -- if the device can't drive a mix there's no reason to keep
// the panel lit. Transitions INTO a degraded state reset inactivity
// so a transient drop doesn't blank the screen mid-mix.
//
// PC sim / scripted tests can scale all M7 times via
// app_power_set_time_scale to avoid waiting real minutes/hours per
// run. Default is 1/1 (real time); tests typically use 1/N where N
// makes 1 h fit in seconds (e.g. 1/120 -> 1 h = 30 s, warn = 0.25 s).

typedef enum {
    APP_POWER_PHASE_AWAKE = 0,    // normal use
    APP_POWER_PHASE_WARNING,       // 30 s pre-blank warning, countdown live
    APP_POWER_PHASE_SLEEP,         // blanked
    APP_POWER_PHASE_WAKE_MENU,     // touch-from-blank: pick a duration
} app_power_phase_t;

// Initialize. Builds the warning + blank + wake-menu overlays lazily
// on first phase entry; starts the periodic tick timer that drives
// phase transitions. Wires WiFi + MS state-change observers so the
// degraded-cap path fires.
void app_power_init(const ms_client_iface_t *ms);

// Apply a time scale to all power timeouts. Set numerator=1,
// denominator=N to make 1 h equal to (3600/N) seconds. Default 1/1
// is real time. Call BEFORE app_power_init for the first tick to
// observe the scale.
void app_power_set_time_scale(uint32_t numerator, uint32_t denominator);

// Force the inactivity timer to reset. Same effect as a touch.
// Useful after the wake menu commits a pick or after a degraded-state
// transition so the user keeps a full timeout from the moment they
// re-engaged.
void app_power_kick(void);

// Test/diagnostic: skip the warning, blank now.
void app_power_force_sleep(void);

// Override the user timeout (pre-scale ms). Tests use this to land a
// tight warn/sleep window inside a few seconds of script time --
// production code only ever sets this through the wake-menu pick.
void app_power_set_user_timeout_ms(uint32_t ms);

// Test introspection.
app_power_phase_t app_power_get_phase(void);
uint32_t          app_power_get_effective_timeout_ms(void);
uint32_t          app_power_get_user_timeout_ms(void);
