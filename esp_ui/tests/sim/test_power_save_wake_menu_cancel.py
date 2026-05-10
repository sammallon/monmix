"""Wake menu has a Sleep cancel button that returns immediately to SLEEP.

Pilot follow-up 2026-05-09: when the panel woke accidentally (touched
in a bag, brushed during teardown) the user wants a one-tap return
to dark, not the 30 s auto-revert wait or having to pick a duration
they don't actually want.

Reuses the M7 power-scale plumbing so the test fits in seconds.
Drives the path: AWAKE -> force_sleep -> SLEEP -> tap-to-wake ->
WAKE_MENU -> tap Sleep -> SLEEP.

Wake menu geometry (build_wake_menu in app_power.c):
- panel 540x360 centered on a 1024x600 screen -> rect (242..782, 120..480)
- pad 20 -> inner top at y=140
- 6 duration buttons in 3 cols x 2 rows: y=40 + (n+cols-1)/cols * (btn_h+gap)
  = 40 + 2 * 86 = 212 inside; the cancel sits at y=212 inside.
- cancel button 460 x 50 centered horizontally -> x=512, y_inside=212+25=237
  -> screen y=140+237=377
"""

TEST = {
    "name": "power_save_wake_menu_cancel",
    "description": "Tapping the Sleep cancel in the wake menu returns to SLEEP.",
    "args": ["--power-scale", "120"],
    "script": (
        # Force into SLEEP via the top-bar power button (test geometry
        # already verified in test_topbar_sleep_button).
        "tap 894 16\n"
        "sleep 100\n"
        "echo after-force-sleep\n"
        "power_phase\n"
        # Tap anywhere on the blank panel -> WAKE_MENU.
        "tap 500 300\n"
        "sleep 100\n"
        "echo after-wake-tap\n"
        "power_phase\n"
        # Tap the Sleep cancel button at the bottom of the wake menu.
        "tap 512 377\n"
        "sleep 100\n"
        "echo after-cancel\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK power_phase=SLEEP",
            "OK power_phase=WAKE_MENU",
            "I (app_power) wake-menu cancel: returning to sleep",
            "I (app_power) entering sleep",
            # Final phase is SLEEP, not AWAKE -- the cancel didn't pick
            # a new duration.
            "after-cancel",
            "OK power_phase=SLEEP",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            # If the tap hit a duration button instead, we'd see one
            # of these. Watching for both common possibilities (the
            # 1h button is at the top-left of the grid; 24h at the
            # bottom-right).
            "wake-menu pick:",
        ],
    },
    # PC sim only -- needs --power-scale CLI flag.
    "hw_compatible": False,
}
