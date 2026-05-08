#!/usr/bin/env python3
"""Run a sim-lane test against real hardware to validate sim/hw parity.

Reuses tests/sim/test_*.py — anything with "hw_compatible": True in
its TEST dict is eligible. Translates the sim's script grammar (tap X
Y / sleep MS / screenshot PATH / quit) to the on-device REPL grammar
that tools/run_steps.py speaks (tap:X,Y / wait:MS / shot:NAME), then
invokes run_steps.py against the tablet's serial port.

Setup:
    1. Tablet connected, drivers happy, COM3 (or whatever) free.
    2. python -m pip install pyserial   (if not already).
    3. MONMIX_HW_PORT=COM3 python tests/hw/run.py [filter]

Each test's stdout from run_steps.py is captured to
tests/_artifacts/<name>/hw_stdout.txt; on-device screenshots land in
tmp/screenshots/<name>__NN.png.

Parity expectations work the same way as sim — stdout_contains /
stdout_not_contains. Some sim-only signals (e.g. "[mock_ms]") obviously
won't be there, so write expectations that hold on both sides. If a
test should ONLY be checked on hardware, name a separate dict
"hw_expect" alongside "expect".

This runner is a starting point: feature gaps include translating
"set_format" -> "cmd:level-format db" (works), persisting prefs
between runs (the tablet's NVS is real), and screenshot diffing
between sim and hw frames. Wire those up as you build out the test
catalogue.
"""

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent.parent
SIM_TESTS  = ROOT / "tests" / "sim"
RUN_STEPS  = ROOT / "tools" / "run_steps.py"
ARTIFACTS  = ROOT / "tests" / "_artifacts"


def discover(filter_substr):
    files = sorted(SIM_TESTS.glob("test_*.py"))
    if filter_substr:
        files = [p for p in files if filter_substr in p.name]
    return files


def load_test(path):
    spec   = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.TEST


def translate(script, test_name):
    """Sim-script grammar -> run_steps.py argv list.

    Drops sim-only ops (echo, the script's terminal `quit`); maps
    `screenshot` paths to bare names so run_steps.py drops them under
    tmp/screenshots/.
    """
    steps = []
    shot_idx = 0
    # Track last (x, y) so a parameter-less `release` can map to an up at
    # the last move/press position, mirroring pc_sim's g_last_x/g_last_y.
    last_x, last_y = None, None
    for raw in script.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"): continue
        parts = line.split()
        op    = parts[0]
        if op == "sleep":
            steps.append(f"wait:{parts[1]}")
        elif op == "tap":
            steps.append(f"tap:{parts[1]},{parts[2]}")
            last_x, last_y = parts[1], parts[2]
        elif op == "press":
            # press X Y -> initial touch down. The device's
            # app_touch_inject_set holds pressed=true until an `up`.
            steps.append(f"down:{parts[1]},{parts[2]}")
            last_x, last_y = parts[1], parts[2]
        elif op == "move":
            # move X Y while pressed -> just another `down` at the new
            # position. app_touch_inject's read_cb sees an updated
            # (x, y) with pressed=true, which LVGL fires as PRESSING.
            steps.append(f"down:{parts[1]},{parts[2]}")
            last_x, last_y = parts[1], parts[2]
        elif op == "release":
            if last_x is None:
                print(f"  warning: release with no prior position, skipping")
                continue
            # release maps to touch X Y up at the last known position,
            # matching pc_sim's release-at-last-cursor semantics.
            steps.append(f"up:{last_x},{last_y}")
        elif op == "screenshot":
            steps.append(f"shot:{test_name}__{shot_idx:02d}")
            shot_idx += 1
        elif op == "set_format":
            steps.append(f"cmd:level-format {parts[1]}")
        elif op == "set_mix":
            steps.append(f"cmd:set-mix {parts[1]}")
        elif op == "mcfg_apply":
            # mcfg_apply HOST PORT -> mcfg-apply REPL command on device.
            # The REPL takes the LVGL lock and calls app_ui_mcfg_apply,
            # the same hook the sim drives.
            steps.append(f"cmd:mcfg-apply {parts[1]} {parts[2]}")
        elif op == "type":
            # type TEXT... -> `type` REPL command (writes to focused
            # textarea). Pass through everything after the keyword.
            text = line[len("type"):].strip()
            steps.append(f"cmd:type {text}")
        elif op == "chpick_save":
            # chpick_save id1,id2,... -> chpick-save REPL command. Same
            # comma-separated id list grammar; the device cmd calls
            # app_ui_chpick_apply just like pc_sim does.
            arg = parts[1] if len(parts) > 1 else ""
            steps.append(f"cmd:chpick-save {arg}" if arg else "cmd:chpick-save")
        elif op == "dump_tiles":
            # dump_tiles -- requires the settings overlay to already be
            # open on hw, same as the sim hook. Test scripts must `tap`
            # the gear + `sleep` first.
            steps.append("cmd:dump-tiles")
        elif op == "chan_id":
            # chan_id <slot> -> chan-id REPL command. Returns "OK chan_id
            # idx=N ms_id=M" (same line shape as the sim script command).
            steps.append(f"cmd:chan-id {parts[1]}")
        elif op == "master_state":
            # master_state get -> master-state REPL command. The sim
            # script takes a "get" arg; the hw command takes none.
            steps.append("cmd:master-state")
        elif op == "prefs_get_color":
            steps.append(f"cmd:prefs-get-color {parts[1]}")
        elif op.startswith("cmd:"):
            # Already in hw form (e.g. from a test's hw_script). Pass through
            # verbatim so hw-only tests can target REPL commands that don't
            # have a sim-side translation.
            steps.append(line)
        elif op in ("quit", "echo"):
            # quit: REPL session naturally ends when run_steps.py finishes.
            # echo: no on-device equivalent, drop.
            pass
        else:
            print(f"  warning: unknown op {op!r} in script")
    return steps


def run_one(test, port):
    name      = test["name"]
    timeout_s = test.get("timeout_s", 90)

    out_dir = ARTIFACTS / name
    out_dir.mkdir(parents=True, exist_ok=True)

    # `hw_script`, if present, overrides `script` for the hw lane. Lets a
    # hw-only test (e.g. one that exercises REPL commands with no sim
    # equivalent) define a clean script without the sim path having to
    # carry filler ops.
    steps = translate(test.get("hw_script", test["script"]), name)
    if not steps:
        return False, "translation produced no steps"

    cmd = [sys.executable, str(RUN_STEPS), port] + steps
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout_s)
    except subprocess.TimeoutExpired:
        return False, f"timeout > {timeout_s}s"

    (out_dir / "hw_stdout.txt").write_text(proc.stdout)
    (out_dir / "hw_stderr.txt").write_text(proc.stderr)

    expect = test.get("hw_expect", test.get("expect", {}))
    fails  = []
    if "exit_code" in expect and proc.returncode != expect["exit_code"]:
        fails.append(f"exit {proc.returncode} != {expect['exit_code']}")
    for s in expect.get("stdout_contains", []):
        if s not in proc.stdout:
            fails.append(f"stdout missing: {s!r}")
    for s in expect.get("stdout_not_contains", []):
        if s in proc.stdout:
            fails.append(f"stdout has forbidden: {s!r}")

    return (not fails), ("; ".join(fails) if fails else "ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("filter", nargs="?")
    ap.add_argument("--list", action="store_true",
                    help="discover hw_compatible tests and print the translated "
                         "REPL grammar without touching the serial port")
    args = ap.parse_args()

    tests = discover(args.filter)
    if not tests:
        sys.stderr.write("no matching tests\n")
        return 1

    if args.list:
        # Discovery + dry-run: lets a sandbox confirm a test will be picked up
        # without any serial I/O. Only --list omits the MONMIX_HW_PORT check.
        for path in tests:
            try:
                test = load_test(path)
            except Exception as e:
                print(f"LOAD-ERR {path.name}: {e}")
                continue
            tag = "HW" if test.get("hw_compatible") else "skip"
            print(f"{tag} {test['name']:<40s}  ({path.name})")
            if not test.get("hw_compatible"): continue
            steps = translate(test.get("hw_script", test["script"]), test["name"])
            for s in steps:
                print(f"     {s}")
        return 0

    port = os.environ.get("MONMIX_HW_PORT")
    if not port:
        sys.stderr.write("set MONMIX_HW_PORT=<COMx> before running\n")
        return 2

    passed = failed = skipped = 0
    for path in tests:
        try:
            test = load_test(path)
        except Exception as e:
            print(f"FAIL {path.name}: load {e}")
            failed += 1; continue

        if not test.get("hw_compatible"):
            print(f"SKIP {test['name']:<40s}  not hw_compatible")
            skipped += 1; continue

        ok, msg = run_one(test, port)
        tag = "PASS" if ok else "FAIL"
        print(f"{tag} {test['name']:<40s}  {msg}")
        if ok: passed += 1
        else:  failed += 1

    print(f"---\n{passed} passed, {failed} failed, {skipped} skipped")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
