"""M7 power save: idle countdown blanks the panel, tap wakes,
wake-menu pick restores normal use. Runs at --power-scale 120 so
1 h compresses to 30 s -- the warn/sleep transitions land in seconds
and the test can finish in well under a minute.
"""

TEST = {
    "name": "power_save_basic",
    "description": "Default 1h timeout warns + blanks; tap wakes; pick restores.",
    "args": ["--power-scale", "120"],
    # Boot at AWAKE, wait 30.1 s scaled to land in SLEEP, tap to
    # wake (anywhere -- blank overlay consumes it), sleep briefly to
    # let WAKE_MENU mount, tap the "1h" button to commit + return to
    # AWAKE.
    "script": (
        "echo phase-after-boot\n"
        "sleep 100\n"
        "power_phase\n"
        "echo waiting-for-sleep\n"
        "sleep 30100\n"
        "power_phase\n"
        "echo tap-to-wake\n"
        "tap 100 100\n"
        "sleep 50\n"
        "power_phase\n"
        "echo tap-1h\n"
        "tap 356 235\n"
        "sleep 100\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        # Phase markers are emitted in order; each "OK power_phase=..."
        # pins down the state at the corresponding script step.
        "stdout_contains": [
            "OK power_phase=AWAKE eff_to_ms=30000",
            "I (app_power) entering sleep",
            # Sleep drives backlight all the way off (mock prints
            # "backlight=off"). Real device drops LEDC duty to 0.
            "[mock_display] backlight=off",
            # Sleep also gracefully closes MS so the server doesn't
            # hold zombie subscriptions overnight.
            "[mock_ms] shutdown_graceful",
            "OK power_phase=SLEEP",
            "OK power_phase=WAKE_MENU",
            "wake-menu pick: 1 h",
            # Wake restarts MS so the user has a live connection
            # by the time the new AWAKE phase begins.
            "I (app_power) wake: restarting MS client",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
            # Sleep must NOT use the 5% floor any more -- that was the
            # symptom the user noticed (panel visibly lit at night).
            "[mock_display] backlight=5%",
        ],
    },
    "hw_compatible": False,  # PC sim only -- needs power-scale CLI flag
}
