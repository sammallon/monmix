"""Wake-menu picker is modal: taps outside the duration card don't
dodge the picker.

Pilot follow-up 2026-05-09: the centered card was previously non-
modal -- taps anywhere outside the buttons leaked through to the
gear icon / sleep button / faders, letting the user enter the rest
of the UI without ever picking a duration. Wake time was then
ambiguously configured because nothing was actively chosen.

Test sequence (scale 30 -> auto-revert is 1s real, gives us room
to fire several taps inside the wake-menu phase):
1. Boot lands in WAKE_MENU.
2. Tap a sequence of NON-card coords that USED to leak through to
   the underlying UI (top-bar gear, top-bar sleep btn, far corner).
3. Phase MUST still be WAKE_MENU after each.
4. Tap the 1h duration button -> commits and goes AWAKE. Confirms
   the modal isn't blocking legitimate choices.
"""

TEST = {
    "name": "power_save_wake_menu_modal",
    "description": "Taps outside duration card don't dismiss the boot wake menu.",
    # Less aggressive scale than power-save tests (30 vs 120) so the
    # 30 s wake-menu auto-revert maps to 1 s real -- enough room for
    # the dodge-tap sequence below to fire before time runs out.
    "args": ["--power-scale", "30"],
    "script": (
        "echo at-boot\n"
        "power_phase\n"
        # Top-bar gear (right side) -- without the scrim, this would
        # have opened the settings overlay underneath the wake menu.
        "tap 1002 16\n"
        # Top-bar sleep button -- would have force-slept the device.
        "tap 894 16\n"
        # Far corner -- a tap on truly empty space; without the scrim
        # this falls through to whatever's underneath.
        "tap 50 50\n"
        "echo after-dodge-attempts\n"
        "power_phase\n"
        # Now the legitimate "1h" duration button. From build_wake_menu:
        # btn_w=140, gap=16, cols=3. Card centered at (512, 300) size
        # 540x360 -> card top-mid at screen (512, 140). Btn 0 (1h):
        #   x_in_card = -156 (col 0 in 3-col grid centered)
        #   y_in_card = 40 + 35 = 75
        # screen: (356, 215).
        "tap 356 215\n"
        "echo after-1h-pick\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Boot lands in WAKE_MENU; full default user timeout
            # (1 h scaled by 30 -> 120 000 ms).
            "at-boot",
            "OK power_phase=WAKE_MENU eff_to_ms=120000",
            # Dodge attempts are absorbed -- still WAKE_MENU after
            # all three.
            "after-dodge-attempts",
            "OK power_phase=WAKE_MENU eff_to_ms=120000",
            # Legitimate pick fires and transitions to AWAKE.
            "wake-menu pick: 1 h",
            "after-1h-pick",
            "OK power_phase=AWAKE eff_to_ms=120000",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            # Critical: NONE of the dodge taps should have triggered
            # the paths the scrim is meant to block. Each of these
            # would log if the corresponding leak path fired.
            "user-initiated sleep",      # top-bar sleep button path
            "settings overlay opened",   # gear icon path
            # If the auto-revert fired we'd be in SLEEP, indicating
            # the test ran longer than the scaled 1 s window.
            "I (app_power) entering sleep",
        ],
    },
    "hw_compatible": False,  # PC sim only -- needs --power-scale flag
}

