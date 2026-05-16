#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_ms_client.h"

// Display power management (M7) -- connectivity-driven model.
//
// Sleep is no longer time-driven. The user does not pick a duration.
// Behavior:
//
//   * Healthy (WiFi associated AND MS WS connected AND MS reports the
//     physical console attached): the panel stays on indefinitely.
//     Activity counter is kept pinned at zero each tick so warning and
//     sleep paths never fire.
//
//   * Degraded (any of those three conditions false): 60 s from last
//     touch -- 30 s of normal AWAKE plus a 30 s WARN countdown that
//     any touch dismisses back to AWAKE. After the 60 s window, the
//     panel sleeps. Rationale: the device can't drive a mix if it
//     can't reach MS, and the mixer is shut off at end of service, so
//     this naturally drives sleep when the rig powers down.
//
//   * Manual sleep (top-bar power-icon tap): immediate transition to
//     SLEEP regardless of connectivity. Backlight off, MS gracefully
//     shut down. Wakes on touch; also wakes on "MS connection became
//     active again" if the device had been degraded (either at sleep
//     entry or at any point during sleep) -- this matches the user's
//     UX expectation that the panel relights when the rig comes back.
//     A manual sleep that happens while everything is healthy and
//     stays healthy does NOT spuriously auto-wake (the user said "go
//     dark", not "wake when MS twitches"). The same rule covers the
//     auto-sleep path automatically since auto-sleep only fires while
//     degraded.
//
//   * Wake mechanisms while in SLEEP:
//       - Touch on the blank overlay -> AWAKE.
//       - Degraded -> healthy edge (MS reachable AND console attached
//         AND WiFi up) -> AWAKE, only when auto-wake is armed (see
//         above).
//
//   * Auto-wake probe: when armed AND MS was stopped at sleep entry,
//     periodically attempt ms->start() so the iface can re-establish
//     a WS. The MS client's own connect-retry loop takes over once
//     started; when it transitions to CONNECTED + console_attached,
//     degraded_state goes false and the edge detector wakes us. The
//     probe is throttled to once per MS_RESTART_PROBE_MS so we're not
//     re-spawning the worker every tick.
//
// PC sim / scripted tests can scale all timeouts via
// app_power_set_time_scale to avoid waiting real minutes per run.
// Default is 1/1 (real time); tests typically use 1/N where N makes
// the 60 s degraded budget fit in seconds (e.g. 1/30 -> 60 s = 2 s,
// warn = 1 s).

typedef enum {
    APP_POWER_PHASE_AWAKE = 0,    // normal use; healthy never leaves this
    APP_POWER_PHASE_WARNING,       // degraded countdown to sleep, touch -> AWAKE
    APP_POWER_PHASE_SLEEP,         // blanked; touch or MS recovery -> AWAKE
} app_power_phase_t;

// Initialize. Builds the warning + blank overlays lazily; starts the
// periodic tick timer. Wires WiFi + MS state-change observers (kept for
// future event-driven wake; tick_cb is the actual driver today). Boot
// lands directly in AWAKE -- no user-prompted duration menu.
void app_power_init(const ms_client_iface_t *ms);

// Apply a time scale to all power timeouts. Set numerator=1,
// denominator=N to make 60 s equal to (60/N) s. Default 1/1 is real
// time. Call BEFORE app_power_init for the first tick to observe the
// scale.
void app_power_set_time_scale(uint32_t numerator, uint32_t denominator);

// Force a touch-equivalent activity reset. Tests use it to keep AWAKE
// across a degraded window without faking pixel taps.
void app_power_kick(void);

// Manual sleep: immediate transition to SLEEP regardless of phase or
// connectivity. Wired to the top-bar power-icon button. Auto-wake on
// MS reconnect activates only if the device was degraded at sleep
// entry, or becomes degraded during sleep -- otherwise touch is the
// only way back.
void app_power_force_sleep(void);

// Test introspection.
app_power_phase_t app_power_get_phase(void);
uint32_t          app_power_get_effective_timeout_ms(void);
