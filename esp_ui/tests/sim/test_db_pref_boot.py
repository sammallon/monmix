"""Regression: ms_client.level_format zero-inits to NORM at start, but
app_prefs may persist DB from a prior session. If start() doesn't seed
level_format from the live pref, the first subscribe goes out in
"norm", broadcasts land in ch.level (NORM slot), apply_pending reads
ch.level_db (because the pref says DB) -> 0 -> -inf dB on every fader.

This is a BOOT-time form of the runtime-switch bug we fixed earlier.
The test does it in two phases sharing one cwd:

  phase 1 (seed):   start fresh, set_format db, quit -> NVS now has
                     level=db.
  phase 2 (boot):   relaunch with the same cwd; ms_client must seed
                     g.level_format from app_prefs at start.

We can't observe slider positions directly from stdout, but the new
ms_real broadcast log (ms_real: lvl ch=N value=...) is format-tagged,
so phase 2 stdout containing "ms_real: lvl ... db=" is proof that
broadcasts routed in DB. If level_format wasn't seeded the broadcasts
would be norm-tagged.
"""

import os

MS_HOST = os.environ.get("MONMIX_MS_HOST")
MS_PORT = os.environ.get("MONMIX_MS_PORT", "8102")

TEST = {
    "name": "db_pref_boot",
    "description": "Booting with persisted level=db must seed ms_client.level_fmt.",
    "args": (
        ["--ms-host", MS_HOST, "--ms-port", MS_PORT] if MS_HOST else []
    ),
    "skip_if": lambda: not MS_HOST,
    "skip_reason": "needs MONMIX_MS_HOST pointing at a live MS",
    "timeout_s": 30,
    "phases": [
        {
            "name": "seed_db",
            "script": (
                "sleep 1500\n"
                "set_format db\n"
                "sleep 500\n"
                "quit\n"
            ),
            "expect": {
                "exit_code": 0,
                "stdout_contains": ["OK set_format db", "OK quit"],
            },
        },
        {
            "name": "boot_with_db",
            "script": (
                "sleep 2000\n"
                "quit\n"
            ),
            "expect": {
                "exit_code": 0,
                # Confirms (a) prefs reload at level=db, (b) the WS
                # subscribe fired in val/db (so MS broadcasted dB),
                # (c) handle_broadcast routed via app_state_set_level_db.
                "stdout_contains": [
                    "level=db",
                    "ms_real: lvl",
                    "db=",
                ],
                "stdout_not_contains": ["LV_ASSERT", "Assertion failed"],
            },
        },
    ],
}
