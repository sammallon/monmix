"""Regression: the sim's exit paths (script `quit`, atexit, SDL_QUIT,
SIGINT/SIGTERM) all flush MS gracefully -- queue /console/data/unsubscribe
for every active subscription, send WS CLOSE, then tear down the worker.

Without this, MS sees the WS die from a TCP RST and carries the prior
subscription state across the connection, accumulating leaked entries
every flash/reboot cycle. The test framework hammers MS many times per
run so the leak compounds quickly without the fix.

Test: connect to live MS, let boot subscribe the default channel set,
then `quit`. Assert the shutdown path ran (graceful-shutdown log line)
AND that the WS got a clean close (we send CLOSE, MS replies, "WS closed"
appears -- distinct from a TCP RST which the worker logs as a connection
error). On the MS side a fresh sim relaunch lands on a clean /app/state
because the prior subscriptions were unsubscribed -- but MS doesn't
expose subscriber inventory via REST, so we can only verify the
client-side log evidence.
"""

import os

MS_HOST = os.environ.get("MONMIX_MS_HOST")
MS_PORT = os.environ.get("MONMIX_MS_PORT", "8102")

TEST = {
    "name": "graceful_shutdown",
    "description": "Sim quit path informs MS via unsubscribes + WS CLOSE.",
    "args": (
        ["--ms-host", MS_HOST, "--ms-port", MS_PORT] if MS_HOST else []
    ),
    "skip_if": lambda: not MS_HOST,
    "skip_reason": "needs MONMIX_MS_HOST pointing at a live MS",
    "timeout_s": 30,
    "script": (
        # Boot + WS connect + subscribe.
        "sleep 4000\n"
        # Trigger graceful shutdown via the script `quit` command. The
        # sim's quit handler runs shutdown_ms_once before printing OK.
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "ms_real: WS open",
            # The graceful-shutdown banner from m_shutdown_graceful.
            # Its presence proves the exit path ran the unsubscribe
            # sequence rather than just exiting on the worker thread.
            "ms_real: graceful shutdown -- queuing unsubscribes",
            "ms_real: graceful shutdown done",
            # WS CLOSE round-trip: we send CLOSE, MS acks, the worker
            # sees MG_EV_CLOSE and logs "WS closed". Distinguishes a
            # clean close from a TCP RST (which would surface as
            # "connection error" instead).
            "ms_real: WS closed",
            "OK quit",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            # Connection-error path = TCP RST, the bug we're avoiding.
            "ms_real: connection error",
        ],
    },
    # Hardware: not directly applicable -- the firmware-side equivalent
    # is the `pre-flash` REPL command (which prints READY-TO-FLASH after
    # running shutdown_graceful). That's covered by the REPL handshake
    # the user wires into their flash workflow separately.
    "hw_compatible": False,
}
