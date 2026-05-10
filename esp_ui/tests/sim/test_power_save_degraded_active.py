"""M7 power save: in degraded state the timeout is relative to last
interaction, not absolute from awake-start.

Pilot bug 2026-05-09: while the user was actively typing in the WiFi
config panel trying to recover the link, the screen blanked at the
60 s degraded cap regardless of taps. With the fix, taps reset the
inactivity clock so the panel stays awake as long as the user is
interacting.

Test setup mirrors test_power_save_degraded: --power-scale 120 means
the 60 s cap becomes 500 ms. The script flips to degraded then taps
every ~150 ms for ~700 ms (well past 500 ms). Without the fix, the
phase would be SLEEP at the end of the tap stream. With the fix it
stays AWAKE because each tap resets `inactive`.
"""

TEST = {
    "name": "power_save_degraded_active",
    "description": "Tapping in degraded state keeps the screen awake (relative timeout).",
    "args": ["--power-scale", "120"],
    "script": (
        "echo flip-degraded\n"
        "power_degraded on\n"
        "sleep 50\n"
        "power_phase\n"
        "echo tap-stream-start\n"
        # Five taps spread over ~750 ms. Each `tap` injects a touch
        # event which resets LVGL's inactivity timer; the next tick
        # sees inactive=0 and stays in AWAKE. Without the relative-
        # timeout fix, elapsed = (now - awake_started) would have
        # crossed the 500 ms cap by the second or third tap.
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
        # Now stop tapping. After ~600 ms of idle (well past the
        # 500 ms cap) the panel should blank.
        "sleep 700\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Just after going degraded: cap kicks in.
            "OK power_phase=AWAKE eff_to_ms=500",
            # End of tap stream (~750 ms post-degraded). With taps
            # resetting inactive, still AWAKE.
            "OK echo tap-stream-end",
            "OK power_phase=AWAKE eff_to_ms=500",
            # After a 700 ms idle without taps, sleep fires.
            "I (app_power) entering sleep (effective_timeout=500ms)",
            "OK power_phase=SLEEP eff_to_ms=500",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
    # PC sim only -- needs --power-scale CLI flag.
    "hw_compatible": False,
}
