"""M7 (new model): auto-wake when MS connection becomes active again.

User requirement (2026-05-15): "I want the device to automatically
wake if the MS connection becomes active again." Verifies that:

  1. Boot healthy → AWAKE.
  2. Flip MS to degraded (via mock_app_ms_set_state DISCONNECTED)
     for >60 s scaled → AWAKE → WARNING → SLEEP. Auto-wake arms
     because sleep entry happened while degraded.
  3. Flip MS back to CONNECTED. The auto-wake probe is throttled
     by MS_RESTART_PROBE_MS so we wait for it. Once MS gets back to
     CONNECTED + console_attached, degraded → healthy edge fires
     and the device wakes back to AWAKE.

Setup: --power-scale 120 makes 60 s cap = 500 ms and the 30 s probe
interval = 250 ms scaled, so the probe runs at most once before the
recovery is detected. Script verifies the AWAKE -> SLEEP -> AWAKE
cycle and the explicit auto-wake log line.
"""

TEST = {
    "name": "power_wake_on_ms_reconnect",
    "description": "MS reconnect after degraded sleep auto-wakes the device.",
    "args": ["--power-scale", "120"],
    "script": (
        "echo healthy-baseline\n"
        "power_phase\n"
        # Force degraded state by disconnecting MS.
        "echo go-degraded\n"
        "power_ms_state off\n"
        "sleep 50\n"
        "power_phase\n"
        # Wait past the 500 ms scaled cap so we reach SLEEP.
        "echo wait-for-sleep\n"
        "sleep 700\n"
        "power_phase\n"
        # MS comes back. enter_sleep stopped the MS client, so the
        # iface's get_state will be DISCONNECTED until ms->start runs
        # again. The mock's start() honours mock_app_ms_set_state so
        # toggling state-overridden+CONNECTED here just sets the
        # post-start state; the tick_cb probe then restarts MS.
        "echo recover-ms\n"
        "power_ms_state on\n"
        # Allow time for the probe to fire and the edge to be
        # detected. Probe interval is 30 s nominal = 250 ms scaled,
        # so we wait at least one probe cycle plus a couple ticks.
        "sleep 500\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Boot is healthy.
            "OK power_phase=AWAKE eff_to_ms=0",
            # Goes degraded after the MS flip.
            "OK power_phase=AWAKE eff_to_ms=500",
            # Reaches SLEEP after the cap window.
            "I (app_power) entering sleep",
            "OK power_phase=SLEEP",
            # Probe restarts MS, edge detector wakes us.
            "I (app_power) auto-wake: MS connection became active",
            # Back to healthy AWAKE.
            "OK power_phase=AWAKE eff_to_ms=0",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
    "hw_compatible": False,
}
