"""Top-bar sleep button forces immediate sleep without warning.

Pilot follow-up 2026-05-09: a one-tap "blank now" affordance for
when the user wants the panel dark immediately (e.g. on stage between
sets) without waiting for the inactivity timer or messing with the
config overlay. Skips the WARNING phase by design -- the user's
intent is "off NOW", not "remind me in 30s". Tapping the resulting
blank panel routes through the existing wake-menu so it's reversible.

Layout: sleep icon at TOP_RIGHT -116, size 28x28 -> screen rect
(880..908, 2..30). Tap center (894, 16). Mute-en hit pad puts a
clickable region behind it spanning 875..912 to give the icon a
generous hit zone.
"""

TEST = {
    "name": "topbar_sleep_button",
    "description": "Tapping the top-bar power icon forces SLEEP phase immediately.",
    "args": ["--power-scale", "120"],
    "script": (
        "echo before-tap\n"
        "power_phase\n"
        # Tap the sleep icon. Should land in SLEEP without going through
        # WARNING first (force_sleep skips the countdown).
        "tap 894 16\n"
        "sleep 100\n"
        "echo after-tap\n"
        "power_phase\n"
        # Sanity: a tap on the blank screen wakes via the WAKE_MENU,
        # confirming the path remains reversible.
        "tap 500 300\n"
        "sleep 100\n"
        "echo after-wake-tap\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Boot lands in AWAKE (1h scaled to 30s).
            "OK power_phase=AWAKE eff_to_ms=30000",
            # Tap fires the user-initiated sleep log line + force_sleep.
            "I (app_ui) user-initiated sleep",
            "I (app_power) entering sleep",
            # Critically: WARN is skipped. SLEEP arrives directly.
            "OK power_phase=SLEEP",
            # Tap on blank routes to wake menu, proving reversibility.
            "OK power_phase=WAKE_MENU",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            # The hit-pad layout previously routed (894, 16) to the MS
            # icon -- if it lands there we'd see the MS panel open
            # instead of sleep firing. Watch for that wrong path.
            "OK power_phase=WARNING",
        ],
    },
    # HW parity: same coords on the panel. WAKE_MENU verification is
    # sim-only because the device doesn't surface the phase via stdout.
    "hw_compatible": True,
    "hw_script": (
        "sleep 3500\n"
        "tap 894 16\n"
        "sleep 800\n"
    ),
    "hw_expect": {
        "exit_code": 0,
        "stdout_contains": [
            "user-initiated sleep",
            "entering sleep",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "panic",
            "abort",
        ],
    },
}
