"""Top-bar sleep button forces immediate SLEEP without warning.

Pilot follow-up from 2026-05-09: a one-tap "blank now" affordance
for when the user wants the panel dark immediately (e.g. on stage
between sets) without waiting for the inactivity timer. Skips the
WARNING phase by design.

Under the new connectivity-driven sleep model, tapping the blank
overlay wakes directly to AWAKE (no wake menu).

Layout: sleep icon at TOP_RIGHT -116, size 28x28 -> screen rect
(880..908, 2..30). Tap center (894, 16).
"""

TEST = {
    "name": "topbar_sleep_button",
    "description": "Top-bar power icon: AWAKE -> SLEEP -> AWAKE (tap to wake).",
    "args": ["--power-scale", "120"],
    "script": (
        "echo before-tap\n"
        "power_phase\n"
        # Manual sleep button.
        "tap 894 16\n"
        "sleep 100\n"
        "echo after-tap\n"
        "power_phase\n"
        # Tap on blank overlay should wake.
        "tap 500 300\n"
        "sleep 100\n"
        "echo after-wake-tap\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Healthy boot -> AWAKE with no timeout.
            "OK power_phase=AWAKE eff_to_ms=0",
            # Manual sleep skips WARN and goes straight to SLEEP.
            "I (app_ui) user-initiated sleep",
            "I (app_power) entering sleep (manual=1",
            "OK power_phase=SLEEP",
            # Tap on blank goes directly to AWAKE (no wake menu).
            "OK power_phase=AWAKE eff_to_ms=0",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            # Manual sleep MUST skip the WARNING phase.
            "OK power_phase=WARNING",
            # No wake menu in the new model.
            "WAKE_MENU",
        ],
    },
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
