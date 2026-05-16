"""M7 (new model): mid-sleep WiFi degradation arms auto-wake, so
when WiFi (and MS via cascade) comes back, the device wakes.

This is the OTHER side of the manual-sleep-healthy gate: the user
manually slept while everything was fine, but later WiFi went away
(rig got moved, AP lost power, etc.). When WiFi comes back, the
panel should light up automatically -- that's the user-facing
auto-wake-on-MS-reconnect feature in full effect.

Implementation: tick_cb watches for wifi-down-while-asleep and arms
the gate. The probe then restarts MS each cycle until it succeeds;
the resulting degraded->healthy edge wakes us.

Setup: --power-scale 120 makes 60 s = 500 ms, probe = 250 ms.
"""


def _verify(stdout, _stderr):
    errors = []
    # The arm log line must appear AFTER manual sleep, BEFORE auto-wake.
    arm_idx  = stdout.find("mid-sleep wifi degraded; arming auto-wake")
    wake_idx = stdout.find("auto-wake: MS connection became active")
    sleep_idx = stdout.find("entering sleep (manual=1 auto_wake_armed=0)")
    if sleep_idx < 0:
        errors.append("expected manual sleep entry log not found")
    if arm_idx < 0:
        errors.append("expected mid-sleep wifi arm log not found")
    elif arm_idx < sleep_idx:
        errors.append("arm log appeared before manual sleep entry (timing wrong)")
    if wake_idx < 0:
        errors.append("expected auto-wake log not found")
    elif wake_idx < arm_idx:
        errors.append("auto-wake log appeared before arm log (timing wrong)")
    return errors


TEST = {
    "name": "power_mid_sleep_wifi_arm_wakes",
    "description": "Manual sleep healthy + wifi drop mid-sleep arms wake; wifi up wakes.",
    "args": ["--power-scale", "120"],
    "script": (
        "echo healthy-baseline\n"
        "power_phase\n"
        # Manual sleep healthy.
        "tap 894 16\n"
        "sleep 100\n"
        "power_phase\n"
        # WiFi goes away mid-sleep. The arm clause should fire on the
        # next tick. No probe yet since wifi-down also gates the probe.
        "echo wifi-down\n"
        "power_degraded on\n"
        "sleep 100\n"
        # WiFi recovers. Probe should fire on next probe-interval edge
        # (~250 ms scaled), restart MS, edge detector wakes the panel.
        "echo wifi-up\n"
        "power_degraded off\n"
        "sleep 500\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "I (app_power) entering sleep (manual=1 auto_wake_armed=0)",
            "OK power_phase=SLEEP",
            "I (app_power) mid-sleep wifi degraded; arming auto-wake",
            "I (app_power) auto-wake: MS connection became active",
            "OK power_phase=AWAKE eff_to_ms=0",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
        "verify": _verify,
    },
    "hw_compatible": False,
}
