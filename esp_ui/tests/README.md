# monmix tests

Three execution lanes. Each test lives in exactly one (the one with the
narrowest possible setup), but the same _kind_ of property can be
checked in multiple lanes when sim/hardware parity is what's interesting.

```
tests/
├── sim/      scripted runs against pc_sim/build-windows/monmix_sim.exe
├── unit/     native C exe linking only the pure-logic modules
└── hw/       same .test.py files as sim/, but driven against real tablet
              over UART REPL via tools/run_steps.py + tools/fetch_screenshot.py
```

## sim — scripted UI tests against the PC sim

Each test is a Python file declaring a `TEST` dict:

```python
TEST = {
    "name": "mute_toast_no_crash",
    "description": "Tap mute with MUTE EN off, expect amber toast.",
    "args": [],                    # extra CLI args, e.g. ["--ms-host", "..."]
    "script": "sleep 800\ntap 298 498\nsleep 200\nquit\n",
    "expect": {
        "exit_code": 0,
        "stdout_contains":     ["OK tap 298 498", "OK quit"],
        "stdout_not_contains": ["LV_ASSERT", "Assertion failed", "WS closed"],
    },
}
```

Run: `python tests/sim/run.py [test_name_pattern]`. Without an argument
it runs every test_*.py in `tests/sim/`. Pass a substring to filter.

The runner writes per-test artifacts to `tests/_artifacts/<name>/`:
stdout, stderr, the running script, and any screenshots the script
produced. Failures print the diff that triggered them.

Default mode is `--headless` so the runner doesn't spam SDL windows
across the desktop. Add `"headless": False` in the test dict if a
test needs to be watched.

## unit — pure-C tests with no UI / network / FS

`tests/unit/` builds a small native exe linking just the
self-contained modules (`app_state.c`, the dB-taper math from
`app_ms_client.h`, etc.). Anything that wants ESP-IDF or LVGL stays
out of this lane.

Run: `bash tests/unit/run.sh` (Linux/WSL) or `pwsh tests/unit/run.ps1`
(Windows). Built with the same MSVC / gcc as the sim.

## hw — same scripts, against the live tablet

Convention: a sim test that should also pass on hardware sets
`"hw": True` in its dict. `tests/hw/run.py` replays the scripts via
`tools/run_steps.py`'s REPL adapter (which already speaks `touch X Y
tap`, `screenshot`, etc.), captures the on-device frame via
`tools/fetch_screenshot.py`, and applies the same expectations.

Hardware tests are not in CI; they're for parity verification when a
sim regression test catches something that ought to also be true on
the device. Run by hand with the tablet on the bench.
