"""M7 power save: the awake clock is absolute from the wake/menu pick.
In-AWAKE touches DO NOT reset the timer (vs the prior behaviour that
inferred the timer from lv_display_get_inactive_time and got reset
by every tap, leaving an actively-used panel awake forever).

Window: timeout 5 s, warn fires at 2 s, sleep at 5 s. The repro taps
4 times within the first 1.5 s (all firmly inside the AWAKE phase),
then idles past the original 5 s deadline. Under the new design
SLEEP fires at ~5 s regardless of the taps; under the old design
the last tap's inactive_time reset would push sleep out by another
5 s.

Taps stop BEFORE warn_start (2 s) on purpose -- a tap during warning
is a different interaction (opens the wake menu, see
power_save_warn_cancel) and would muddy the assertion here.
"""

TEST = {
    "name": "power_save_no_idle_reset",
    "description": "Touches during AWAKE don't reset the absolute-from-wake timer.",
    "args": ["--power-scale", "10"],
    "script": (
        "power_set_user_timeout_ms 50000\n"   # 5 s post-scale
        "power_kick\n"
        "echo tap-stream-start\n"
        # 4 taps inside the first 1.5 s (all in AWAKE, before warn).
        # Each tap consumes ~120 ms of pump time on top of the sleep.
        "tap 50 50\n"
        "sleep 300\n"
        "tap 60 60\n"
        "sleep 300\n"
        "tap 70 70\n"
        "sleep 300\n"
        "tap 80 80\n"
        # Wait past the 5 s absolute deadline. Total elapsed ~ 4*120 +
        # 3*300 + 4500 = 5880 ms -- comfortably past 5 s. Under the
        # absolute-from-wake design we land in SLEEP.
        "sleep 4500\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "I (app_power) entering sleep (effective_timeout=5000ms)",
            "OK power_phase=SLEEP eff_to_ms=5000",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
        ],
    },
    "hw_compatible": False,
    "timeout_s": 30,
}
