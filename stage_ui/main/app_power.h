// Display power management — adapted from esp_ui/main/app_power.{c,h}.
// Same M7-style state machine, with PP connectivity replacing MS as the
// degraded-state trigger.
//
// Phases:
//   AWAKE   — normal use. Configured timeout from app_prefs.
//   WARNING — 30 s pre-blank dialog with countdown. Tap to keep awake.
//   SLEEP   — panel blanked. Tap to wake.
//
// Connectivity-degraded states (WiFi != CONNECTED, OR PP client !=
// CONNECTED) cap the effective timeout at 60 s. A device that can't
// reach ProPresenter has no reason to keep the panel lit on a stage.
//
// Differences from esp_ui:
//   - No WAKE_MENU boot picker. A stage display device doesn't need a
//     "pick how long to stay awake" picker on every power-on; the
//     configured sleep timeout (Settings -> General) is enough. If the
//     user later wants the picker back, the AWAKE phase + the iface
//     here are the right places to splice it in.

#ifndef APP_POWER_H
#define APP_POWER_H

#include <stdbool.h>
#include <stdint.h>

#include "app_pp_client.h"

typedef enum {
    APP_POWER_PHASE_AWAKE = 0,
    APP_POWER_PHASE_WARNING,
    APP_POWER_PHASE_SLEEP,
} app_power_phase_t;

// Initialise. Builds the warning + blank overlays lazily on first
// phase entry; starts the tick timer; wires WiFi + PP state-change
// observers so the degraded-cap path fires on link drop.
void app_power_init(const pp_client_iface_t *pp);

// Force the inactivity clock to restart from now. Equivalent of a touch.
// Used by overlays after a config change so the user gets a full new
// timeout from the moment they re-engaged.
void app_power_kick(void);

// Force sleep right now. Diagnostic / test helper.
void app_power_force_sleep(void);

// Time scale (numerator/denominator). Default 1/1 = real time. Setting
// 1/N makes 1 h take 3600/N seconds, useful for scripted tests.
void app_power_set_time_scale(uint32_t numerator, uint32_t denominator);

// Introspection.
app_power_phase_t app_power_get_phase(void);
uint32_t          app_power_get_effective_timeout_ms(void);

#endif // APP_POWER_H
