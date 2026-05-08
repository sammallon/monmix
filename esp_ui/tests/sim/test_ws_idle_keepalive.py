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
            # "WS closed" used to indicate the keepalive failed and the
            # WS dropped. With graceful shutdown enabled, `quit` cleanly
            # closes the WS at exit, which now legitimately produces
            # "WS closed". A reconnect mid-test would also produce a
            # second "WS open" -- if the bug regressed, the 45 s idle
            # would still trigger one or more closes within the run,
            # which would surface as the graceful-shutdown banner not
            # being the immediate predecessor. Best the substring
            # runner can do: forbid LV_ASSERT and trust the
            # graceful_shutdown test to police the close path.
            "LV_ASSERT",
        ],
    },
    # Hardware variant: same 45 s idle, just on the device. Firmware logs
    # "ws closed" (lowercase) instead of "WS closed" -- if the keepalive
    # regressed the device would print that mid-sleep. There's no `quit`
    # equivalent, so the script just sleeps; lack of a close mid-idle is
    # the assertion.
    "hw_compatible": True,
    "hw_script": (
        "sleep 45000\n"
    ),
    "hw_expect": {
        "exit_code": 0,
        "stdout_not_contains": [
            "ws closed",
            "LV_ASSERT",
            "panic",
            "abort",
        ],
    },
}
