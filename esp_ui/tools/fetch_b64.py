"""Pull a base64-framed payload from the device's UART REPL.

Sends one of:
    cat-b64 <path>       # any file, e.g. /sdcard/coredump-0007.elf
    coredump-b64         # raw flash coredump partition (when SD failed)

extracts the bytes between the `===BEGIN BASE64 ...===` /
`===END BASE64===` markers, base64-decodes, and writes to a local file.

Usage:
    python tools/fetch_b64.py COM3 /sdcard/coredump-0007.elf  out.elf
    python tools/fetch_b64.py COM3 coredump                   out.elf

A bare local-out filename (no directory component) is written to
`tmp/logs/` relative to the repo root. Pass an absolute path or a path
with separators to override that default.
"""
import base64
import os
import re
import sys
import time

import serial
import serial.serialutil

if len(sys.argv) < 4:
    sys.exit("usage: fetch_b64.py <port> <remote-path-or-'coredump'> <local-out>")


# Repo root is two levels up from this file (tools/ → repo/).
_REPO_ROOT      = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_LOG_DIR = os.path.join(_REPO_ROOT, "tmp", "logs")


def resolve_local_out(name):
    """Map a bare filename onto tmp/logs/, leaving real paths untouched."""
    if os.path.isabs(name) or os.path.dirname(name):
        return name
    os.makedirs(DEFAULT_LOG_DIR, exist_ok=True)
    return os.path.join(DEFAULT_LOG_DIR, name)


PORT       = sys.argv[1]
REMOTE     = sys.argv[2]
LOCAL_OUT  = resolve_local_out(sys.argv[3])
REPLY_WAIT = float(sys.argv[4]) if len(sys.argv) > 4 else 8.0

BEGIN_RE = re.compile(rb"===BEGIN BASE64 (\S+) SIZE (\d+) ===")
END_TAG  = b"===END BASE64==="


def open_port(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            ser = serial.Serial()
            ser.port     = port
            ser.baudrate = 921600   # match CONFIG_ESP_CONSOLE_UART_BAUDRATE
            ser.timeout  = 0.05
            ser.dtr      = False
            ser.rts      = False
            ser.open()
            return ser
        except (serial.serialutil.SerialException, OSError):
            time.sleep(0.2)
    raise RuntimeError(f"could not open {port}")


def main():
    cmd_str = "coredump-b64" if REMOTE == "coredump" else f"cat-b64 {REMOTE}"
    ser = open_port(PORT)
    try:
        print(f">>> sending: {cmd_str}", flush=True)
        ser.write((cmd_str + "\r\n").encode())
        ser.flush()

        deadline = time.time() + REPLY_WAIT
        buf      = bytearray()
        while time.time() < deadline:
            chunk = ser.read(8192)
            if chunk:
                buf.extend(chunk)
                # idle-extend: keep reading until the device goes quiet
                deadline = time.time() + 1.5
            else:
                time.sleep(0.05)

        m = BEGIN_RE.search(buf)
        if not m:
            print("ERROR: no BEGIN marker found", file=sys.stderr)
            print("--- captured ---", file=sys.stderr)
            print(buf.decode("utf-8", errors="replace"), file=sys.stderr)
            sys.exit(1)
        size = int(m.group(2))
        path = m.group(1).decode()
        print(f"=== marker: {path} ({size} bytes) ===")

        end_idx = buf.find(END_TAG, m.end())
        if end_idx < 0:
            print("ERROR: no END marker found", file=sys.stderr)
            sys.exit(1)

        payload = b"".join(buf[m.end():end_idx].split())
        try:
            raw = base64.b64decode(payload, validate=True)
        except Exception as e:
            print(f"ERROR: base64 decode failed: {e}", file=sys.stderr)
            print(f"first 200 chars of payload: {payload[:200]!r}", file=sys.stderr)
            sys.exit(1)

        if len(raw) != size:
            print(f"WARN: decoded {len(raw)} bytes, expected {size}",
                  file=sys.stderr)

        with open(LOCAL_OUT, "wb") as f:
            f.write(raw)
        print(f"=== saved {len(raw)} bytes to {LOCAL_OUT} ===")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
