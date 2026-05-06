"""Boot the sim against a live MS instance using the OSC backend. Verifies
the UDP open, REST-fetched mix layout, and the heartbeat/prime path bring
the UI up to a CONNECTED state without crashing.

Skipped unless MONMIX_MS_HOST is set (so CI without a live MS doesn't try
to connect).
"""
import os

TEST = {
    "name": "osc_live_smoke",
    "description": "Sim with --protocol osc connects to a live MS, primes, exits clean.",
    "args": [
        "--protocol", "osc",
        "--ms-host",  os.environ.get("MONMIX_MS_HOST", "127.0.0.1"),
        "--ms-port",  os.environ.get("MONMIX_MS_PORT", "8102"),
        "--osc-port", os.environ.get("MONMIX_MS_OSC_PORT", "3000"),
    ],
    # Boot is ~2-3 s for connect + REST. Then a few seconds to let prime
    # walk through ~30 paths at 25 ms each, plus settle. Quit cleanly.
    "script": (
        "sleep 6000\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "UI shell mounted",
            "faders mounted",
            "ms_real_osc: UDP open",
            "ms_real_osc: mix offset=",
            "OK quit",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
            "ms_real_osc: UDP error",
        ],
    },
    "skip_if":     lambda: not os.environ.get("MONMIX_MS_HOST"),
    "skip_reason": "MONMIX_MS_HOST not set; skipping live-MS test",
    "timeout_s":   30,
}
