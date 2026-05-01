"""Run a scripted sequence of touch/console/screenshot steps in one go.

Step syntax (as positional args):
    tap:X,Y              synthetic LVGL tap at (X, Y)
    cmd:CMD ARGS         arbitrary console command (e.g. cmd:level-format db)
    shot:NAME            take a screenshot, save as NAME.png
    wait:MS              sleep for MS milliseconds (let animations settle)

Example — open settings, switch to dB, capture before/after, close:

    python tools/run_steps.py COM3 \
        shot:01-pre-settings \
        tap:1002,16 shot:02-settings-open \
        cmd:level-format\\ db shot:03-db-mode \
        tap:990,28 shot:04-closed

Screenshots are tagged with the supplied NAME (no extension) and use the
same RGB565+deflate+base64 pipeline as `fetch_screenshot.py`, just inlined
here so a single port-open serves the whole sequence.

A bare NAME (no directory component) is written to `tmp/screenshots/`
relative to the repo root. Pass an absolute path or a path with separators
in NAME to override that default.
"""
import base64
import os
import re
import struct
import sys
import time
import zlib

import serial
import serial.serialutil

if len(sys.argv) < 3:
    sys.exit("usage: run_steps.py <port> <step1> [step2 ...]")

PORT  = sys.argv[1]
STEPS = sys.argv[2:]


# Repo root is two levels up from this file (tools/ → repo/).
_REPO_ROOT       = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SHOT_DIR = os.path.join(_REPO_ROOT, "tmp", "screenshots")


def resolve_shot_path(name):
    """Map a `shot:NAME` value onto tmp/screenshots/NAME.png if it's bare.

    Returns an absolute path. If NAME has a directory component or is
    already absolute, it's used as-is (with `.png` appended).
    """
    out = name + ".png"
    if os.path.isabs(out) or os.path.dirname(out):
        return out
    os.makedirs(DEFAULT_SHOT_DIR, exist_ok=True)
    return os.path.join(DEFAULT_SHOT_DIR, out)

BAUD = 921600

BEGIN_RE = re.compile(rb"===BEGIN BASE64 (\S+) SIZE (\d+) ===")
END_TAG  = b"===END BASE64==="


def open_port(port, timeout=10):
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


def send_and_drain(ser, line, idle_grace=1.5, max_wait=30):
    ser.write((line + "\r\n").encode())
    ser.flush()
    deadline = time.time() + 4.0
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


def screenshot(ser, name):
    raw = send_and_drain(ser, "screenshot", idle_grace=1.5, max_wait=30)
    m = BEGIN_RE.search(raw)
    if not m:
        print(f"  ! {name}: no BEGIN marker")
        return
    end_idx = raw.find(END_TAG, m.end())
    if end_idx < 0:
        print(f"  ! {name}: no END marker")
        return
    payload = base64.b64decode(b"".join(raw[m.end():end_idx].split()),
                               validate=True)
    if len(payload) < 36:
        print(f"  ! {name}: payload too small")
        return
    magic, w, h, stride, fmt, ulen, clen, flags = struct.unpack_from(
        "<8sIIIIIII", payload, 0)
    if not magic.startswith(b"MMSCRN"):
        print(f"  ! {name}: bad magic")
        return
    body   = payload[36:]
    pixels = zlib.decompress(body) if (flags & 1) else body

    try:
        from PIL import Image
    except ImportError:
        sys.exit("PIL/Pillow not available. `pip install Pillow`.")

    out = bytearray(w * h * 3)
    for y in range(h):
        row_off = y * stride
        for x in range(w):
            lo = pixels[row_off + x * 2]
            hi = pixels[row_off + x * 2 + 1]
            v  = (hi << 8) | lo
            r  = (v >> 11) & 0x1F
            g  = (v >>  5) & 0x3F
            b  =  v        & 0x1F
            o = (y * w + x) * 3
            out[o + 0] = (r << 3) | (r >> 2)
            out[o + 1] = (g << 2) | (g >> 4)
            out[o + 2] = (b << 3) | (b >> 2)
    img = Image.frombytes("RGB", (w, h), bytes(out))
    out_path = resolve_shot_path(name)
    img.save(out_path)
    print(f"  → saved {out_path} ({w}×{h}, {clen} comp)")


def main():
    ser = open_port(PORT)
    try:
        for i, step in enumerate(STEPS, start=1):
            print(f"[{i}/{len(STEPS)}] {step}")
            if step.startswith("tap:"):
                xy = step[4:].split(",")
                if len(xy) != 2:
                    print(f"  ! bad tap step: {step!r}")
                    continue
                send_and_drain(ser, f"touch {xy[0].strip()} {xy[1].strip()} tap",
                               idle_grace=0.4, max_wait=4)
            elif step.startswith("shot:"):
                screenshot(ser, step[5:])
            elif step.startswith("cmd:"):
                send_and_drain(ser, step[4:], idle_grace=0.4, max_wait=8)
            elif step.startswith("wait:"):
                ms = int(step[5:])
                time.sleep(ms / 1000.0)
            else:
                print(f"  ! unknown step: {step!r}")
        print("=== done ===")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
