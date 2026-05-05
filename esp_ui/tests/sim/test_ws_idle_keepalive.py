"""Regression: without a periodic WS PING the WS would close every ~25s
of idle and the sim's reconnect loop would cycle "WS closed" / "WS
open" forever. Fixed in 8f0e5d8 by sending mongoose's PING every 15s.

Test: connect to live MS, sit idle 45s (longer than the 25s drop
window observed in the bug report), expect the connection to stay
open the whole time — i.e. no "WS closed" log lines.
"""

import os

MS_HOST = os.environ.get("MONMIX_MS_HOST")
MS_PORT = os.environ.get("MONMIX_MS_PORT", "8102")

TEST = {
    "name": "ws_idle_keepalive",
    "description": "WS stays open through 45s of idle (was dropping ~25s without ping).",
    "args": (
        ["--ms-host", MS_HOST, "--ms-port", MS_PORT] if MS_HOST else []
    ),
    "skip_if": lambda: not MS_HOST,
    "skip_reason": "needs MONMIX_MS_HOST pointing at a live MS",
    "timeout_s": 90,
    "script": (
        "sleep 45000\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "WS open",
            "OK quit",
        ],
        "stdout_not_contains": [
            "WS closed",
            "LV_ASSERT",
        ],
    },
}
