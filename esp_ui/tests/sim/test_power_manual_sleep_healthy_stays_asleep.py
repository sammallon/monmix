"""M7 (new model): manual sleep while healthy stays asleep.

Requirement #4 from the user: "auto-wake when MS connection becomes
active again", but EXPLICITLY exempting the manual-sleep-healthy case.
A user who taps the power icon while everything is fine has said
"go dark", not "wake me up on the next MS twitch". Tap to wake is
the only escape from that state.

Tricky: stopping MS during sleep (the 48ea146 graceful-shutdown path)
makes ms->get_state() report non-CONNECTED, which would naively trip
the "degraded mid-sleep -> arm auto-wake" clause. The implementation
guards against that -- only externally-observable degradation (wifi
drop) arms the gate while MS is self-stopped. This test pins that
behavior down.

Setup: --power-scale 120 makes the 30 s probe interval = 250 ms scaled.
Test waits past one probe cycle (and then some) without any state
change, asserts the device stayed in SLEEP, AND uses a verify hook
to assert NO auto-wake log line appeared anywhere in the window.
"""


def _verify(stdout, _stderr):
    errors = []
    # No auto-wake should have fired between the manual sleep and quit.
    # The marker enter_awake prints when it restarts MS is also a tell.
    forbidden = [
        "auto-wake: MS connection became active",
        "wake: restarting MS client",
        "mid-sleep wifi degraded; arming auto-wake",
    ]
    for f in forbidden:
        if f in stdout:
            errors.append(
                f"saw {f!r} -- manual-sleep-healthy spuriously auto-woke")
    return errors


TEST = {
    "name": "power_manual_sleep_healthy_stays_asleep",
    "description": "Manual sleep while healthy stays asleep (no spurious wake).",
    "args": ["--power-scale", "120"],
    "script": (
        "echo healthy-baseline\n"
        "power_phase\n"
        # Manual sleep via top-bar power icon.
        "tap 894 16\n"
        "sleep 100\n"
        "power_phase\n"
        # Wait past multiple probe cycles (probe interval = 250 ms
        # scaled; 1000 ms covers ~4 probes). Nothing should wake the
        # panel during this window because auto-wake isn't armed and
        # the probe is gated on auto_wake_armed.
        "echo wait-multiple-probes\n"
        "sleep 1000\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK power_phase=AWAKE eff_to_ms=0",
            "I (app_power) entering sleep (manual=1 auto_wake_armed=0)",
            "OK power_phase=SLEEP",
            "OK echo wait-multiple-probes",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            # Critically: device must still be asleep at quit time.
            # If a spurious auto-wake fires, power_phase reports AWAKE
            # after the probe and this string appears.
            "OK power_phase=AWAKE eff_to_ms=0\nOK quit",
            # The probe itself shouldn't have fired -- it's gated on
            # auto_wake_armed.
            "sleep probe: restarting MS",
        ],
        "verify": _verify,
    },
    "hw_compatible": False,
}
