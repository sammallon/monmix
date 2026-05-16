"""M7 (new model): healthy state keeps the panel awake indefinitely.

Under the connectivity-driven sleep model there is no user-set
timeout. As long as wifi + MS WS + console_attached are all good,
the panel never sleeps regardless of touch inactivity. Old tests
that asserted "sleep fires after N seconds of idle" no longer apply.

Setup: --power-scale 600 makes a nominal 60 s into 100 ms. The
script waits past several scaled "hours" without any touch and
asserts AWAKE persists. effective_timeout_ms reads 0 in healthy
state (sentinel meaning "no timeout").
"""

TEST = {
    "name": "power_healthy_never_sleeps",
    "description": "Healthy connectivity = panel stays awake forever (no touch).",
    "args": ["--power-scale", "600"],
    "script": (
        "echo healthy-baseline\n"
        "power_phase\n"
        "sleep 200\n"
        "power_phase\n"
        # Long idle window (well past any plausible old user timeout
        # under the same scale). Asserts no degradation drift.
        "sleep 2000\n"
        "power_phase\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Sentinel: 0 means "no timeout" (healthy).
            "OK power_phase=AWAKE eff_to_ms=0",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
            # Never enters WARNING or SLEEP without a touch or a
            # connectivity flip.
            "OK power_phase=WARNING",
            "OK power_phase=SLEEP",
            "I (app_power) entering sleep",
        ],
    },
    "hw_compatible": False,
}
