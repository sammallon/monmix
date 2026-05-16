"""M7 (new model): manual sleep while healthy stays asleep.

Auto-wake-on-MS-reconnect arms only if the device was degraded at
sleep entry, OR becomes degraded during sleep. A manual sleep
button-press while everything is healthy must NOT spuriously wake
the panel just because the MS restart probe sees MS active --
that would defeat the manual sleep affordance.

Setup: --power-scale 120 keeps the test fast. Tap the top-bar power
icon while healthy, then wait long enough for the probe interval
to pass (1+ scaled probe cycle = 300+ ms). Assert SLEEP persists.

Then verify that MS *going* degraded during sleep arms the gate so
recovery wakes: flip MS off, then back on. Final state should be
AWAKE -- demonstrates that the gate works in both directions
(initial-armed at entry vs armed-mid-sleep).
"""

TEST = {
    "name": "power_manual_sleep_healthy_stays_asleep",
    "description": "Manual sleep healthy -> stays asleep until tap; later degradation arms wake.",
    "args": ["--power-scale", "120"],
    "script": (
        "echo healthy-baseline\n"
        "power_phase\n"
        # Manual sleep (top-bar button).
        "tap 894 16\n"
        "sleep 100\n"
        "power_phase\n"
        # Wait past one probe cycle (250 ms scaled) without a state
        # change. SLEEP must persist -- no spurious wake.
        "echo wait-one-probe\n"
        "sleep 400\n"
        "power_phase\n"
        # Now flip MS off mid-sleep. This arms auto-wake under the
        # mid-sleep-degraded clause.
        "echo arm-via-mid-sleep-degrade\n"
        "power_ms_state off\n"
        "sleep 100\n"
        # MS recovers. Edge detector should fire wake.
        "echo recover-ms\n"
        "power_ms_state on\n"
        "sleep 400\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Boot healthy.
            "OK power_phase=AWAKE eff_to_ms=0",
            # Manual sleep marker (auto_wake_armed=0 because healthy).
            "I (app_power) entering sleep (manual=1 auto_wake_armed=0)",
            # Still SLEEP one probe cycle later.
            "OK echo wait-one-probe",
            "OK power_phase=SLEEP",
            # Auto-wake fires AFTER mid-sleep degradation + recovery.
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
