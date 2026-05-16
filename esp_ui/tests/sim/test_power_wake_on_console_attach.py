"""M7 (new model): auto-wake when console_attached flips false->true.

Specific case: MS is reachable (WS connected) the whole time but the
physical Si Expression console is powered off, so MS reports
console_attached=false. The device is therefore in the degraded gate
and falls asleep. When the console is powered back on, MS flips
console_attached=true; the device must auto-wake.

Mirrors test_power_wake_on_ms_reconnect but drives the recovery via
the console_attached flag instead of the MS WS state. Same auto-wake
edge detector covers both.
"""

TEST = {
    "name": "power_wake_on_console_attach",
    "description": "console_attached false->true after degraded sleep auto-wakes.",
    "args": ["--power-scale", "120"],
    "script": (
        "echo healthy-baseline\n"
        "power_phase\n"
        # MS WS stays connected; only console_attached flips off.
        "echo console-off\n"
        "power_ms_console off\n"
        "sleep 50\n"
        "power_phase\n"
        # Wait past the cap to reach SLEEP.
        "sleep 700\n"
        "power_phase\n"
        # Power-on the (simulated) console.
        "echo console-on\n"
        "power_ms_console on\n"
        "sleep 400\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK power_phase=AWAKE eff_to_ms=0",
            "OK power_phase=AWAKE eff_to_ms=500",
            "I (app_power) entering sleep",
            "OK power_phase=SLEEP",
            "I (app_power) auto-wake: MS connection became active",
            "OK power_phase=AWAKE eff_to_ms=0",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
    "hw_compatible": False,
}
