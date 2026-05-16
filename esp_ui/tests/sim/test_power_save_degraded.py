"""M7 (new model): in degraded state, the device sleeps after 60 s of
no activity. With --power-scale 120 that's a 500 ms window.

Healthy state (boot default in the mock) → no sleep ever. The script
keeps healthy for a settling window, asserts AWAKE persists, then
flips to degraded and waits past the cap. SLEEP must fire.
"""

TEST = {
    "name": "power_save_degraded",
    "description": "Degraded for >60 s scaled drives the panel to SLEEP.",
    "args": ["--power-scale", "120"],
    "script": (
        # Boot lands directly in AWAKE under the connectivity-driven
        # model. Healthy = eff_to_ms = 0 (no timeout); degraded = 500.
        "echo healthy-baseline\n"
        "power_phase\n"
        "echo flip-degraded\n"
        "power_degraded on\n"
        "sleep 50\n"
        "power_phase\n"
        # Wait past the 500 ms scaled cap. Sleep should fire mid-wait.
        "echo wait-past-cap\n"
        "sleep 700\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK power_phase=AWAKE eff_to_ms=0",
            "OK power_phase=AWAKE eff_to_ms=500",
            "I (app_power) entering sleep",
            "OK power_phase=SLEEP eff_to_ms=500",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
    "hw_compatible": False,
}
