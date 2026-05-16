"""M7 (new model): in degraded state with continued user activity, the
60 s timeout is relative to last touch -- taps reset the clock and
the panel stays awake. Matches the pilot fix from 2026-05-09 where
the user was actively typing in the WiFi config and the screen
blanked under them at the 60 s mark.

Setup: --power-scale 120 makes the 60 s cap = 500 ms. The script
flips to degraded, then taps every 150 ms for ~750 ms (well past
the 500 ms cap). Without the relative-timeout behavior the screen
would have blanked partway through; with it, AWAKE persists.

After the tap stream stops, the script idles past the cap to verify
SLEEP does eventually fire (so we're confirming relativity, not just
blanket no-sleep).
"""

TEST = {
    "name": "power_save_degraded_active",
    "description": "Taps in degraded state keep AWAKE (relative cap).",
    "args": ["--power-scale", "120"],
    "script": (
        "echo flip-degraded\n"
        "power_degraded on\n"
        "sleep 50\n"
        "power_phase\n"
        "echo tap-stream-start\n"
        "tap 100 100\n"
        "sleep 150\n"
        "tap 110 110\n"
        "sleep 150\n"
        "tap 120 120\n"
        "sleep 150\n"
        "tap 130 130\n"
        "sleep 150\n"
        "tap 140 140\n"
        "sleep 150\n"
        "echo tap-stream-end\n"
        "power_phase\n"
        # No more taps -> sleep window expires (~600 ms past last tap).
        "sleep 700\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK power_phase=AWAKE eff_to_ms=500",
            "OK echo tap-stream-end",
            # Still AWAKE at end-of-tap-stream even though >500 ms has
            # passed since degraded entry.
            "OK power_phase=AWAKE eff_to_ms=500",
            # 700 ms past the last tap -- sleep eventually fires.
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
