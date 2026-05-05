#!/usr/bin/env python3
"""Run scripted regression tests against the PC sim.

Discovers test_*.py modules in this directory, executes each one's TEST
dict, and reports pass/fail. The runner is deliberately tiny -- no
pytest, no yaml, no third-party deps -- so it works under whatever
Python is handy (Windows-side or WSL).

Each test_*.py exports a top-level dict named TEST, e.g.:

    TEST = {
        "name": "mute_toast_no_crash",
        "description": "Tap mute with MUTE EN off, expect amber toast.",
        "args": [],
        "script": "sleep 800\\ntap 298 498\\nsleep 200\\nquit\\n",
        "expect": {
            "exit_code": 0,
            "stdout_contains":     ["OK tap 298 498", "OK quit"],
            "stdout_not_contains": ["LV_ASSERT", "WS closed"],
        },
    }
"""

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT      = Path(__file__).resolve().parent.parent.parent
SIM_EXE   = ROOT / "pc_sim" / "build-windows" / "monmix_sim.exe"
ARTIFACTS = ROOT / "tests" / "_artifacts"


def discover(filter_substr: str | None) -> list[Path]:
    test_files = sorted(Path(__file__).parent.glob("test_*.py"))
    if filter_substr:
        test_files = [p for p in test_files if filter_substr in p.name]
    return test_files


def load_test(path: Path) -> dict:
    spec   = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.TEST


def run_one(test: dict) -> tuple[bool, str]:
    name      = test["name"]
    args      = list(test.get("args", []))
    script    = test["script"]
    expect    = test.get("expect", {})
    headless  = test.get("headless", True)
    timeout_s = test.get("timeout_s", 90)

    out_dir = ARTIFACTS / name
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    script_path = out_dir / "script.txt"
    script_path.write_text(script)

    cmd = [str(SIM_EXE), "--script", str(script_path)] + args
    if headless:
        cmd.append("--headless")

    start = time.time()
    proc  = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout_s, cwd=out_dir)
    elapsed = time.time() - start

    (out_dir / "stdout.txt").write_text(proc.stdout)
    (out_dir / "stderr.txt").write_text(proc.stderr)

    fails: list[str] = []

    if "exit_code" in expect and proc.returncode != expect["exit_code"]:
        fails.append(f"exit {proc.returncode} != {expect['exit_code']}")

    for s in expect.get("stdout_contains", []):
        if s not in proc.stdout:
            fails.append(f"stdout missing: {s!r}")
    for s in expect.get("stdout_not_contains", []):
        if s in proc.stdout:
            fails.append(f"stdout has forbidden: {s!r}")
    for s in expect.get("stderr_not_contains", []):
        if s in proc.stderr:
            fails.append(f"stderr has forbidden: {s!r}")

    msg = f"{elapsed:5.1f}s"
    if fails:
        msg += "  " + "; ".join(fails)
    return (not fails, msg)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("filter", nargs="?", help="substring to filter test names")
    args = ap.parse_args()

    if not SIM_EXE.exists():
        sys.stderr.write(f"sim not built; expected {SIM_EXE}\n"
                         "build via pc_sim/build.ps1 first.\n")
        return 2

    tests = discover(args.filter)
    if not tests:
        sys.stderr.write("no tests matched\n")
        return 1

    passed = 0
    failed = 0
    skipped = 0
    for path in tests:
        try:
            test = load_test(path)
        except Exception as e:
            print(f"FAIL {path.name}: load {e}")
            failed += 1
            continue

        skip_fn = test.get("skip_if")
        if skip_fn and skip_fn():
            reason = test.get("skip_reason", "")
            print(f"SKIP {test['name']:<40s}  {reason}")
            skipped += 1
            continue

        try:
            ok, msg = run_one(test)
        except subprocess.TimeoutExpired:
            ok, msg = False, f"timeout > {test.get('timeout_s', 90)}s"
        except Exception as e:
            ok, msg = False, f"runner exception: {e}"

        tag = "PASS" if ok else "FAIL"
        print(f"{tag} {test['name']:<40s}  {msg}")
        if ok: passed += 1
        else:  failed += 1

    print(f"---\n{passed} passed, {failed} failed, {skipped} skipped")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
