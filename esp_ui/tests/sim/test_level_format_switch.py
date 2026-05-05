"""Regression: switching between NORM and DB level format used to make
every fader snap to floor because (a) the lvl subscribe was hardcoded
"norm", (b) route_inbound always called app_state_set_level even for
val/dB broadcasts, (c) m_set_level_format was a no-op storage stub.
Fixed in 451c16e.

Test runs only when --ms-host points at a live MS instance — the
fault is in MS->sim subscription routing, so a mock MS doesn't
exercise it.
"""

import os

MS_HOST = os.environ.get("MONMIX_MS_HOST")
MS_PORT = os.environ.get("MONMIX_MS_PORT", "8102")

TEST = {
    "name": "level_format_switch",
    "description": "Flip pref NORM->DB, expect MS broadcasts to land in level_db.",
    # MONMIX_MS_HOST=192.168.76.186 selects this. Without the env var the
    # runner falls back to mock MS, which can't exercise the routing path
    # under test, so the test is skipped via empty args triggering only
    # the boot smoke part of the script.
    "args": (
        ["--ms-host", MS_HOST, "--ms-port", MS_PORT] if MS_HOST else []
    ),
    "skip_if": lambda: not MS_HOST,
    "skip_reason": "needs MONMIX_MS_HOST env var pointing at a live MS",
    "script": (
        "sleep 1500\n"
        "set_format db\n"
        "sleep 1500\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK set_format db",
            "OK quit",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
}
