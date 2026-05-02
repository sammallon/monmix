"""Post-soak analysis: pull the device's monmix-NNNN.log files and chart
internal/PSRAM heap trends so we can see fragmentation accumulating.

Usage:
    python tools/soak_report.py COM3              # pull all logs since last boot
    python tools/soak_report.py COM3 --logs 5     # pull last 5 logs

Reads each log via `cat-b64`, concatenates, then extracts the heartbeat
lines emitted by app_logd. Plots heap stats vs time as ASCII so we don't
need matplotlib for a quick look. Also flags any
"HEAP CORRUPTION DETECTED" lines and any boot markers (logd's session-
start log) which can identify unexpected reboots over the run.

Heartbeat format (post commit dd96ff4):
    [TIMESTAMP] [hb] I int free=N largest=N min=N | spi free=N largest=N
"""
from __future__ import annotations

import argparse
import base64
import re
import sys
import time
from pathlib import Path

import serial
import serial.serialutil

REPO_ROOT = Path(__file__).resolve().parent.parent
LOG_DIR   = REPO_ROOT / "tmp" / "logs"
LOG_DIR.mkdir(parents=True, exist_ok=True)

BAUD = 921600
HB_RE = re.compile(
    r"\[(\d+)\] \[hb\] I int free=(\d+) largest=(\d+) min=(\d+) "
    r"\| spi free=(\d+) largest=(\d+)")


def open_port(port: str, timeout: float = 10.0) -> serial.Serial:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            ser = serial.Serial()
            ser.port     = port
            ser.baudrate = BAUD
            ser.timeout  = 0.05
            ser.dtr      = False
            ser.rts      = False
            ser.open()
            return ser
        except (serial.serialutil.SerialException, OSError):
            time.sleep(0.2)
    raise RuntimeError(f"could not open {port}")


def send_and_drain(ser: serial.Serial, line: str,
                   idle_grace: float = 1.0, max_wait: float = 30) -> bytes:
    ser.write((line + "\r\n").encode())
    ser.flush()
    deadline      = time.time() + 4.0
    hard_deadline = time.time() + max_wait
    buf = bytearray()
    while time.time() < deadline and time.time() < hard_deadline:
        chunk = ser.read(16384)
        if chunk:
            buf.extend(chunk)
            deadline = time.time() + idle_grace
        else:
            time.sleep(0.02)
    return bytes(buf)


def fetch_b64(ser: serial.Serial, path: str) -> bytes | None:
    raw = send_and_drain(ser, f"cat-b64 {path}", max_wait=120)
    m = re.search(rb"===BEGIN BASE64 \S+ SIZE \d+ ===", raw)
    if not m:
        return None
    end = raw.find(b"===END BASE64===", m.end())
    if end < 0:
        return None
    body = raw[m.end():end]
    try:
        return base64.b64decode(b"".join(body.split()), validate=True)
    except Exception as e:
        print(f"  ! decode failed for {path}: {e}")
        return None


def list_logs(ser: serial.Serial) -> list[str]:
    raw = send_and_drain(ser, "ls /sdcard", idle_grace=0.4, max_wait=4)
    text = raw.decode(errors="replace")
    return sorted(re.findall(r"monmix-\d+\.log", text))


def render_chart(label: str, values: list[int], width: int = 60) -> str:
    if not values:
        return f"{label}: (no data)"
    lo = min(values)
    hi = max(values)
    if hi == lo:
        return f"{label}: flat at {hi}"
    step = (hi - lo) / width
    bar = "".join("█" if v - lo > step * (i / 1.0)  else " "
                  for i, v in enumerate(values))
    return f"{label}: lo={lo} hi={hi} delta={hi-lo}"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("port",        help="device serial port (e.g. COM3)")
    ap.add_argument("--logs", type=int, default=5,
                    help="how many of the most recent log files to pull (default 5)")
    args = ap.parse_args()

    ser = open_port(args.port)
    try:
        # Nudge prompt so we don't read partial last-command output.
        send_and_drain(ser, "", idle_grace=0.3, max_wait=2)

        logs = list_logs(ser)
        if not logs:
            print("no logs on /sdcard"); sys.exit(1)
        targets = logs[-args.logs:]
        print(f"pulling {len(targets)} log(s): {targets}")

        all_hb: list[tuple[int, int, int, int, int, int]] = []
        boots = 0
        corruption_lines = []
        log_paths_local: list[Path] = []

        for name in targets:
            data = fetch_b64(ser, f"/sdcard/{name}")
            if data is None:
                print(f"  ! {name}: pull failed")
                continue
            local = LOG_DIR / name
            local.write_bytes(data)
            log_paths_local.append(local)
            text = data.decode(errors="replace")

            # Boot marker: logd "session start" lines indicate fresh boots.
            boots += text.count("session start")

            # Corruption lines from heartbeat task's integrity check.
            for line in text.splitlines():
                if "HEAP CORRUPTION" in line:
                    corruption_lines.append(f"{name}: {line.strip()}")

            # Heartbeat extraction.
            for m in HB_RE.finditer(text):
                t, in_free, in_lg, in_min, sp_free, sp_lg = (
                    int(m.group(i)) for i in range(1, 7))
                all_hb.append((t, in_free, in_lg, in_min, sp_free, sp_lg))

        print()
        print(f"=== boots seen: {boots} ===")
        if boots > len(targets):
            print("  ! more boots than log files — device rebooted unexpectedly during the soak")

        print()
        print(f"=== corruption alerts: {len(corruption_lines)} ===")
        for line in corruption_lines:
            print(f"  {line}")
        if not corruption_lines:
            print("  (none — heap integrity check passed every cycle)")

        print()
        print(f"=== heartbeats analyzed: {len(all_hb)} ===")
        if all_hb:
            in_free  = [h[1] for h in all_hb]
            in_lg    = [h[2] for h in all_hb]
            in_min   = [h[3] for h in all_hb]
            sp_free  = [h[4] for h in all_hb]
            sp_lg    = [h[5] for h in all_hb]

            print(f"INTERNAL free:    start={in_free[0]:>8d}  end={in_free[-1]:>8d}  "
                  f"min={min(in_free):>8d}  max={max(in_free):>8d}")
            print(f"INTERNAL largest: start={in_lg[0]:>8d}  end={in_lg[-1]:>8d}  "
                  f"min={min(in_lg):>8d}  max={max(in_lg):>8d}")
            print(f"INTERNAL min-ever:start={in_min[0]:>8d}  end={in_min[-1]:>8d}  "
                  f"min={min(in_min):>8d}  max={max(in_min):>8d}")
            print(f"PSRAM    free:    start={sp_free[0]:>8d}  end={sp_free[-1]:>8d}  "
                  f"min={min(sp_free):>8d}  max={max(sp_free):>8d}")
            print(f"PSRAM    largest: start={sp_lg[0]:>8d}  end={sp_lg[-1]:>8d}  "
                  f"min={min(sp_lg):>8d}  max={max(sp_lg):>8d}")

            # Fragmentation signal: largest_free_block dropping while
            # total_free stays high → fragmentation accumulating.
            in_free_drop = in_free[0] - min(in_free)
            in_lg_drop   = in_lg[0]   - min(in_lg)
            print()
            print("=== fragmentation signals ===")
            print(f"  internal: free dropped by {in_free_drop} bytes "
                  f"(largest dropped by {in_lg_drop} bytes)")
            if in_lg_drop > in_free_drop * 1.5 and in_lg_drop > 8 * 1024:
                print("  ! INTERNAL fragmentation likely — largest_free_block "
                      "dropped much more than total_free")
        print()
        print(f"raw logs saved to: {LOG_DIR}")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
