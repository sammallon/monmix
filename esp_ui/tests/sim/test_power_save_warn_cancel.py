"""M7 power save: warning dialog cancellation. Tap during the warn
phase resets the idle counter and returns to AWAKE without entering
SLEEP.

Uses power_set_user_timeout_ms to land a tight 5 s post-scale window
so the warn phase is observable without racing pump_for slop. With
scale 1/10 the 30 s warn becomes 3 s -- comfortably above the
script's tap latency (80 ms hold + 40 ms post-pump = 120 ms) so the
cancel reliably lands inside the warn window.
"""

TEST = {
    "name": "power_save_warn_cancel",
    "description": "Tap during warn cancels and returns to AWAKE.",
    # Scale 1/10: 30 s warn -> 3 s. Set user timeout to 50 000 ms
    # pre-scale -> 5 000 ms post-scale. Warn from 2 000 ms to 5 000 ms.
    "args": ["--power-scale", "10"],
    "script": (
        "power_set_user_timeout_ms 50000\n"
        "power_kick\n"
        "echo wait-for-warn\n"
        "sleep 2500\n"
        "power_phase\n"
        "echo cancel-tap\n"
        "tap 50 50\n"
        "sleep 200\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK power_phase=WARNING eff_to_ms=5000",
            "OK power_phase=AWAKE eff_to_ms=5000",
        ],
        "stdout_not_contains": [
            # Must NOT enter SLEEP -- the cancel saves the panel.
            "I (app_power) entering sleep",
            "LV_ASSERT",
        ],
    },
    "hw_compatible": False,
    "timeout_s": 30,
}
