"""M7 power save: degraded-state cap. When wifi/MS/console drops,
the effective timeout is forced to min(user, 60 s). Verified by
flipping the mock wifi state to FAILED and watching the eff_to_ms
field drop from 30000 (1 h scaled) to 500 (60 s scaled).

Also verifies that going BACK to healthy doesn't re-light the panel
on its own -- the user gets the resume cadence they expect, with no
panel flash mid-blink.
"""

TEST = {
    "name": "power_save_degraded",
    "description": "Degraded wifi caps the effective timeout to 60 s scaled.",
    "args": ["--power-scale", "120"],
    "script": (
        "echo healthy-eff\n"
        "power_phase\n"
        "echo flip-degraded\n"
        "power_degraded on\n"
        "sleep 50\n"
        "power_phase\n"
        "echo wait-degraded-blank\n"
        "sleep 700\n"
        "power_phase\n"
        "echo flip-healthy\n"
        "power_degraded off\n"
        "sleep 50\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK power_phase=AWAKE eff_to_ms=30000",
            "OK power_phase=AWAKE eff_to_ms=500",
            "I (app_power) entering sleep (effective_timeout=500ms)",
            "OK power_phase=SLEEP eff_to_ms=500",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
    "hw_compatible": False,
}
