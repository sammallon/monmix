"""Tiny REPL helper that uses the same DTR/RTS-disabled open dance as
fetch_screenshot.py, so opening the port doesn't pulse the auto-reset.

Usage:
    python tools/repl_send.py "level-format db"
"""
import sys
import time

import serial


def open_port(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            ser = serial.Serial()
            ser.port     = port
            ser.baudrate = 921600
            ser.timeout  = 0.05
            ser.dtr      = False
            ser.rts      = False
            ser.open()
            return ser
        except Exception:
            time.sleep(0.2)
    raise RuntimeError(f"could not open {port}")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    port = sys.argv[2] if len(sys.argv) > 2 else "COM3"
    ser = open_port(port)
    time.sleep(0.3)
    drained = ser.read(8192)
    print(f"[drained {len(drained)} pre-cmd bytes]")
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    deadline = time.time() + 1.5
    out = bytearray()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            out.extend(chunk)
            deadline = time.time() + 0.3
    sys.stdout.write(out.decode("utf-8", errors="replace"))
    ser.close()


if __name__ == "__main__":
    main()
