#pragma once

// Drop the "boot-time MS-info setup completed" gate so the next CONNECTED
// state change re-runs try_apply_ms_info. Used by the MS-config save path
// when host/port changes at runtime: the strip-name cache, routability
// probe, and mix-routing state were primed against the old host (or never
// primed if MS was unreachable at boot) and need a fresh pass.
void app_ms_setup_reset(void);
