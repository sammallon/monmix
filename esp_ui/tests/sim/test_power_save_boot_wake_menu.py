"""Powerup lands in the wake menu so the user explicitly picks a duration.

Pilot follow-up 2026-05-09: silently defaulting to 1 h on every
powerup surprised the tester. Now boot drops straight into the wake
menu; the user picks a duration before the panel goes "live" into
AWAKE. No persistence -- every powerup re-prompts.

Two phases verified:
1. At-boot phase is WAKE_MENU (not AWAKE).
2. Walking away (no pick within WAKE_MENU_TIMEOUT_MS) auto-reverts
   to SLEEP, which means a powered-on-and-left device doesn't sit
   lit forever burning battery.
"""

TEST = {
    "name": "power_save_boot_wake_menu",
    "description": "Boot enters WAKE_MENU; auto-reverts to SLEEP if no pick.",
    # Scale 1/120: 1 h -> 30 s, 30 s warn -> 0.25 s, 30 s wake-menu
    # auto-revert -> 0.25 s. Test fits in well under a second of
    # script time.
    "args": ["--power-scale", "120"],
    "script": (
        "echo at-boot\n"
        "power_phase\n"
        # Auto-revert window is ~250 ms scaled. Wait past it.
        "sleep 400\n"
        "echo after-no-pick\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Boot lands in the picker, not directly in AWAKE.
            "at-boot",
            "OK power_phase=WAKE_MENU",
            # No pick -> auto-revert to SLEEP.
            "I (app_power) entering sleep",
            "after-no-pick",
            "OK power_phase=SLEEP",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            # The pre-fix behavior: silent AWAKE default with the
            # scaled 30 s effective timeout. If we see this, init
            # didn't transition into the wake menu.
            "OK power_phase=AWAKE",
        ],
    },
    # PC sim only -- needs --power-scale CLI flag.
    "hw_compatible": False,
}
