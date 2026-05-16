"""M7 (new model): a tap during the WARNING countdown dismisses it
and returns to AWAKE without going to SLEEP.

Setup: --power-scale 120 makes the 60 s degraded cap = 500 ms, with
warn fading in at ~250 ms (cap - WARN_MS scaled). Script flips to
degraded, waits past warn-entry, taps to dismiss, asserts AWAKE.
"""

TEST = {
    "name": "power_save_warn_cancel",
    "description": "Tap during WARNING dismisses back to AWAKE (no SLEEP).",
    "args": ["--power-scale", "120"],
    "script": (
        "echo flip-degraded\n"
        "power_degraded on\n"
        "sleep 50\n"
        # Drift into the warning window (warn fires at ~250 ms scaled).
        "sleep 300\n"
        "power_phase\n"
        "echo tap-to-dismiss\n"
        "tap 50 50\n"
        "sleep 100\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK power_phase=WARNING eff_to_ms=500",
            # Tap returns to AWAKE; no SLEEP.
            "OK power_phase=AWAKE eff_to_ms=500",
        ],
        "stdout_not_contains": [
            # Critical: WARN tap must not slip into SLEEP.
            "I (app_power) entering sleep",
            "OK power_phase=SLEEP",
            "LV_ASSERT",
        ],
    },
    "hw_compatible": False,
}
