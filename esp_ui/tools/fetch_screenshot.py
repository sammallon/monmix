"""Pull a screenshot from the device and save as PNG.

Sends `screenshot` to the UART REPL, captures the base64-framed payload,
zlib-decompresses the pixel data, and renders to PNG via Pillow.

Header layout (36 bytes, little-endian):
    char     magic[8];     // "MMSCRN\0\0"
    uint32_t w, h, stride, format;
    uint32_t uncompressed_size, compressed_size, flags;

flags bit 0 = zlib-compressed payload (currently always set).

Usage:
    python tools/fetch_screenshot.py COM3 out.png

A bare filename (no directory component) is written to `tmp/screenshots/`
relative to the repo root. Pass an absolute path or a path with separators
to override that default.
"""
import os
import struct
import sys
import time
import zlib

import serial
import serial.serialutil


# Repo root is two levels up from this file (tools/ → repo/).
_REPO_ROOT       = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SHOT_DIR = os.path.join(_REPO_ROOT, "tmp", "screenshots")


def resolve_shot_path(name):
    """Map a user-supplied output name onto the screenshots dir if it's bare.

    Returns the path unchanged when the caller passed a real path (absolute
    or with a separator). Otherwise prepends the default tmp/screenshots/
    folder, creating it if needed.
    """
    if os.path.isabs(name) or os.path.dirname(name):
        return name
    os.makedirs(DEFAULT_SHOT_DIR, exist_ok=True)
    return os.path.join(DEFAULT_SHOT_DIR, name)


PORT     = sys.argv[1] if len(sys.argv) > 1 else "COM3"
OUT_PATH = resolve_shot_path(sys.argv[2] if len(sys.argv) > 2 else "screenshot.png")

import re
import base64

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


def fetch_payload(ser, cmd, idle_grace=2.0):
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    deadline = time.time() + 4.0
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(16384)
        if chunk:
            buf.extend(chunk)
            deadline = time.time() + idle_grace
        else:
            time.sleep(0.02)
    return bytes(buf)


STATS_RE = re.compile(rb"screenshot-stats: src=(\d+) comp=(\d+) "
                      rb"ratio=([\d.]+)% compress_us=(\d+)")
BAUD_BYTES_PER_SEC = 921600 / 10   # 8N1 framing — 1 start + 8 data + 1 stop


def main():
    try:
        from PIL import Image
    except ImportError:
        sys.exit("PIL/Pillow not available. `pip install Pillow`.")

    ser = open_port(PORT)
    try:
        print(">>> screenshot")
        t_send = time.perf_counter()
        raw = fetch_payload(ser, "screenshot")
        t_recv = time.perf_counter() - t_send
    finally:
        ser.close()

    m = BEGIN_RE.search(raw)
    if not m:
        sys.exit("ERROR: no BEGIN BASE64 marker — is the screenshot command flashed?")
    end_idx = raw.find(END_TAG, m.end())
    if end_idx < 0:
        sys.exit("ERROR: no END BASE64 marker — capture truncated")
    stats_m = STATS_RE.search(raw)
    src_dev = int(stats_m.group(1)) if stats_m else None
    comp_dev = int(stats_m.group(2)) if stats_m else None
    compress_us = int(stats_m.group(4)) if stats_m else None

    payload_b64 = b"".join(raw[m.end():end_idx].split())
    t_b64 = time.perf_counter()
    payload = base64.b64decode(payload_b64, validate=True)
    t_b64 = time.perf_counter() - t_b64

    if len(payload) < 36:
        sys.exit(f"ERROR: payload too small ({len(payload)} bytes)")
    magic, w, h, stride, fmt, ulen, clen, flags = struct.unpack_from(
        "<8sIIIIIII", payload, 0)
    if not magic.startswith(b"MMSCRN"):
        sys.exit(f"ERROR: bad magic {magic!r}")

    body = payload[36:]
    if len(body) != clen:
        print(f"WARN: header says {clen} compressed bytes, got {len(body)}")

    t_decomp = time.perf_counter()
    if flags & 1:
        pixels = zlib.decompress(body)
    else:
        pixels = body
    t_decomp = time.perf_counter() - t_decomp
    if len(pixels) != ulen:
        print(f"WARN: header says {ulen} uncompressed bytes, got {len(pixels)}")

    # ── Compression accounting ─────────────────────────────────────────
    # On-wire bytes when compressed: header (36) + b64 of (header + comp_len)
    # On-wire bytes if uncompressed: header (36) + b64 of (header + ulen)
    b64_size = lambda n: ((n + 2) // 3) * 4
    sent_now    = b64_size(36 + clen)
    sent_if_raw = b64_size(36 + ulen)
    t_wire_now    = sent_now    / BAUD_BYTES_PER_SEC
    t_wire_if_raw = sent_if_raw / BAUD_BYTES_PER_SEC
    overhead_dev  = (compress_us / 1e6) if compress_us is not None else 0.0
    overhead_host = t_decomp + t_b64
    saved = (t_wire_if_raw) - (t_wire_now + overhead_dev + t_decomp)

    print(f"=== {w}×{h}, stride={stride}, format=0x{fmt:02x} ===")
    print(f"    bytes:    raw={ulen}, compressed={clen} ({100.0 * clen / ulen:.1f}% — {ulen / clen:.1f}× ratio)")
    print(f"    on-wire:  base64={sent_now}, would-be uncompressed={sent_if_raw}")
    print(f"    timings:  device-compress={overhead_dev * 1000:.1f} ms, "
          f"host-decompress={t_decomp * 1000:.1f} ms, host-b64={t_b64 * 1000:.1f} ms")
    print(f"              wire@921600: actual={t_wire_now:.2f} s, uncompressed-would-be={t_wire_if_raw:.2f} s")
    print(f"              total round-trip={t_recv:.2f} s")
    print(f"    net win:  {saved:.2f} s saved vs. uncompressed transfer "
          f"(device+host overhead {(overhead_dev + t_decomp) * 1000:.1f} ms)")

    # LV_COLOR_FORMAT_RGB565 is 0x12 in LVGL 9.
    if fmt != 0x12:
        sys.exit(f"ERROR: unsupported color format 0x{fmt:02x} (expected RGB565=0x12)")

    # Pillow can't natively load RGB565 — unpack to RGB888.
    bpp = 2
    out = bytearray(w * h * 3)
    for y in range(h):
        row_off = y * stride
        for x in range(w):
            lo = pixels[row_off + x * bpp]
            hi = pixels[row_off + x * bpp + 1]
            v  = (hi << 8) | lo
            r  = (v >> 11) & 0x1F
            g  = (v >>  5) & 0x3F
            b  =  v        & 0x1F
            # 5/6-bit → 8-bit with replicate-high-bits expansion.
            o = (y * w + x) * 3
            out[o + 0] = (r << 3) | (r >> 2)
            out[o + 1] = (g << 2) | (g >> 4)
            out[o + 2] = (b << 3) | (b >> 2)

    img = Image.frombytes("RGB", (w, h), bytes(out))
    img.save(OUT_PATH)
    print(f"=== saved {OUT_PATH} ({img.size[0]}×{img.size[1]}) ===")


if __name__ == "__main__":
    main()
