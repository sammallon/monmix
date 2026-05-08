"""M7 power save: tapping during the warning opens the wake menu so
the user picks a fresh duration (absolute from that point). Picking
1h restarts AWAKE without ever entering SLEEP.

Uses --power-scale 10 + power_set_user_timeout_ms to land a tight
5 s post-scale window. Warn fires at 2 s; the script taps at 2.5 s
to land in the warn window, then taps the 1h button on the wake
menu to commit a new duration.
"""

TEST = {
    "name": "power_save_warn_cancel",
    "description": "Tap during warn opens wake menu; pick restarts AWAKE absolute.",
    # Scale 1/10: 30 s warn -> 3 s. Set user timeout to 50 000 ms
    # pre-scale -> 5 000 ms post-scale. Warn from 2 000 ms to 5 000 ms.
    "args": ["--power-scale", "10"],
    "script": (
        "power_set_user_timeout_ms 50000\n"
        "power_kick\n"
        "echo wait-for-warn\n"
        "sleep 2500\n"
        "power_phase\n"
        "echo tap-warning\n"
        "tap 50 50\n"
        "sleep 100\n"
        "power_phase\n"
        # Pick "1h" from the wake menu to commit a fresh duration.
        # Button positions match build_wake_menu: cols=3, rows=2, btn
        # 140x70, gap 16, panel centered at (512, 300) size 540x280.
        # 1h button center ~ (356, 235).
        "echo pick-1h\n"
        "tap 356 235\n"
        "sleep 200\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK power_phase=WARNING eff_to_ms=5000",
            # After warn-tap we're in WAKE_MENU, not AWAKE. The user
            # is being asked to pick a fresh duration.
            "OK power_phase=WAKE_MENU eff_to_ms=5000",
            "wake-menu pick: 1 h",
            # Picking a duration restarts AWAKE with that timeout.
            # 1h pre-scale = 3600000 ms; post-scale 1/10 = 360000 ms.
            "OK power_phase=AWAKE eff_to_ms=360000",
        ],
        "stdout_not_contains": [
            # Must NOT enter SLEEP -- the menu pick saves the panel.
            "I (app_power) entering sleep",
            "LV_ASSERT",
        ],
    },
    "hw_compatible": False,
    "timeout_s": 30,
}
