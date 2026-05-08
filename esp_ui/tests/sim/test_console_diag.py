"""Cherry-pick coverage: set-bright, wifi-stats, ws-status console commands.
These were ported from the disconnect-fixes worktree (commits 1b9aef8,
1f66676). All three are firmware-only (no sim equivalent) so the test
is hardware-only -- the sim run skips itself."""

TEST = {
    "name": "console_diag_smoke",
    "description": "set-bright / wifi-stats / ws-status REPL commands respond.",
    "args": [],
    "skip_if": lambda: True,
    "skip_reason": "hardware-only: REPL commands require firmware",
    "script": "quit\n",  # never runs in sim
    "expect": {"exit_code": 0},

    "hw_compatible": True,
    "hw_expect": {
        "exit_code": 0,
        "stdout_contains": [
            # set-bright echoes the new percentage.
            "backlight = 60%",
            # wifi-stats prints state= and ssid= prefixes.
            "state=",
            "ssid=",
            # ws-status prints the MS endpoint URL.
            "endpoint=ws://",
        ],
        "stdout_not_contains": ["panic", "abort"],
    },
    # The hw_script overrides the sim script. The hw runner translates
    # `cmd:` steps into REPL writes and prints whatever serial output
    # comes back, which is where the ESP_LOG / printf lines we assert
    # on land.
    "hw_script": (
        "sleep 800\n"
        "cmd:set-bright 60\n"
        "sleep 200\n"
        "cmd:wifi-stats\n"
        "sleep 200\n"
        "cmd:ws-status\n"
        "sleep 200\n"
        "quit\n"
    ),
}
