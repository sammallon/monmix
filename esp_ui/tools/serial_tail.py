#!/usr/bin/env python3
"""
Non-interactive serial tail for COM3@115200. Writes raw bytes to stdout as
lines, with a [serial-tail] prefix on its own status messages so they don't
get confused with device output. Used by automation that can't attach a TTY
to idf.py monitor.

Usage: python tools/serial_tail.py [PORT] [BAUD]
"""
import sys, time

try:
    import serial
except ImportError:
    print("[serial-tail] pyserial not installed", flush=True)
    sys.exit(1)

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

print(f"[serial-tail] opening {PORT}@{BAUD}", flush=True)
try:
    p = serial.Serial(PORT, BAUD, timeout=0.5)
except Exception as e:
    print(f"[serial-tail] open failed: {e}", flush=True)
    sys.exit(2)
print(f"[serial-tail] open ok; tailing", flush=True)

buf = b""
while True:
    try:
        chunk = p.read(4096)
    except Exception as e:
        print(f"[serial-tail] read error: {e}", flush=True)
        time.sleep(0.5)
        try:
            p.close()
        except Exception:
            pass
        try:
            p = serial.Serial(PORT, BAUD, timeout=0.5)
            print(f"[serial-tail] reopened {PORT}", flush=True)
        except Exception as e2:
            print(f"[serial-tail] reopen failed: {e2}", flush=True)
            time.sleep(2)
        continue
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        line, _, buf = buf.partition(b"\n")
        try:
            sys.stdout.write(line.decode("utf-8", "replace") + "\n")
        except Exception:
            sys.stdout.write(repr(line) + "\n")
        sys.stdout.flush()
