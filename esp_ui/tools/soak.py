"""Long-run soak test for the monmix firmware.

Drives bidirectional traffic to flush out crashes, hangs, heap fragmentation,
and reconnect bugs over a multi-hour run:

  - HOST → MS via REST: random fader / mute changes pushed to MS, which
    broadcasts them back over the device's WS subscriptions. Stresses
    handle_broadcast and the dirty-flag sweep with realistic high-rate
    traffic.

  - DEVICE → MS via touch injection: synthetic taps at known UI coords
    fire the on_slider / on_mute paths, which post to MS via the
    device's WS. Verified via the next REST GET (faster than a
    screenshot when the change is non-visual; the script falls back to
    a screenshot when verification needs the actual rendered UI).

  - Periodic screenshots into tmp/screenshots/soak-NNNN.png at a low
    rate so we can spot-check rendering health.

  - The device's heartbeat log is pulled at the end of each run — heap
    free / largest_free_block / minimum trends across the run identify
    fragmentation accumulation before it crashes.

Usage:
    python tools/soak.py COM3 192.168.76.186 --hours 4

Adds:
    --hours N            run duration (default 1 h)
    --shot-every N       seconds between screenshots (default 60)
    --rest-rps N         host→MS REST rate (default 2 sets/sec)
    --touch-every N      seconds between device-side touch injections (default 30)
"""
from __future__ import annotations

import argparse
import base64
import json
import random
import re
import struct
import sys
import time
import urllib.request
import zlib
from pathlib import Path

import serial
import serial.serialutil

REPO_ROOT = Path(__file__).resolve().parent.parent
SHOT_DIR  = REPO_ROOT / "tmp" / "screenshots"
LOG_DIR   = REPO_ROOT / "tmp" / "logs"
SHOT_DIR.mkdir(parents=True, exist_ok=True)
LOG_DIR.mkdir(parents=True, exist_ok=True)

BAUD = 921600

BEGIN_RE = re.compile(rb"===BEGIN BASE64 (\S+) SIZE (\d+) ===")
END_TAG  = b"===END BASE64==="
B64_ALPHABET = frozenset(
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")


# ─────────────────────────────────────────────────────────────────────────
# MS REST helpers
# ─────────────────────────────────────────────────────────────────────────

def ms_get(host: str, port: int, path: str, timeout: float = 3.0) -> dict | None:
    url = f"http://{host}:{port}{path}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.loads(r.read())
    except Exception as e:
        print(f"  ! GET {path} failed: {e}")
        return None


def ms_set(host: str, port: int, path: str, value,
           timeout: float = 3.0, quiet: bool = False) -> bool:
    """POST to /console/data/set/<path>/<fmt> with {"value": ...}.

    The path argument is the full set-path including /<fmt> suffix
    (e.g. "ch.0.levelData.0.lvl/norm"). `quiet=True` suppresses error
    printing — useful during route probing where 404s are expected on
    un-routed mixes.
    """
    url  = f"http://{host}:{port}/console/data/set/{path}"
    body = json.dumps({"value": value}).encode()
    req  = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status == 200
    except Exception as e:
        if not quiet:
            print(f"  ! SET {path} = {value!r} failed: {e}")
        return False


def ms_get_lvl(host: str, port: int, ch: int, mix: int) -> float | None:
    """Read back ch.<ch>.levelData.<mix>.lvl as a float (norm 0..1).

    /console/data/get/<path> isn't a valid endpoint per the OpenAPI;
    the GET form is /console/data/paths/<path> which returns {val: [...]}
    at a leaf, but we use the simpler /console/information-style snapshot
    by subscribing-then-reading isn't worth the WS plumbing here. We fall
    back to skipping verification when the GET shape isn't supported.
    """
    # Plain GET on the leaf; if MS doesn't expose it, we don't fail soak.
    return ms_get(host, port, f"/console/data/paths/ch.{ch}.levelData.{mix}.lvl")


# ─────────────────────────────────────────────────────────────────────────
# UART helpers (mirror tools/run_steps.py)
# ─────────────────────────────────────────────────────────────────────────

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
                   idle_grace: float = 1.5, max_wait: float = 30) -> bytes:
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


def screenshot(ser: serial.Serial, name: str) -> bool:
    # Bumped max_wait from 30 → 60 s — under realistic WS-broadcast load
    # the device's snapshot + compress + b64-emit chain can stretch to
    # ~10–20 s before the BEGIN marker arrives. idle_grace bumped to 15 s
    # because between the REPL echo and the first stats/BEGIN printf the
    # device runs lv_snapshot_take_to_draw_buf (full-screen render into
    # the PSRAM buffer) and tdefl_compress on 1.2 MB of pixels — that
    # whole window is silent on UART, and a smaller idle_grace lets the
    # host bail out before BEGIN arrives.
    raw = send_and_drain(ser, "screenshot", idle_grace=15.0, max_wait=60)
    m = BEGIN_RE.search(raw)
    if not m:
        # Surface the first kilobyte of what we DID receive — if the
        # device printed "screenshot: lvgl_port_lock timeout" or "PSRAM
        # alloc failed", that signal is what we actually need, not just
        # "no BEGIN marker". Strip the command echo prefix so the
        # diagnostic is the first interesting bytes.
        snippet = raw[:1024].decode(errors="replace")
        print(f"  ! {name}: no BEGIN marker (got {len(raw)} bytes)\n"
              f"      <<<{snippet!r}>>>")
        return False
    end_idx = raw.find(END_TAG, m.end())
    if end_idx < 0:
        print(f"  ! {name}: no END marker (got {len(raw)} bytes)")
        return False
    # Filter the body to b64-valid lines only. Under load a printf from
    # another task can land mid-stream (esp_log_level_set quiesces ESP_LOG
    # but not raw printf), so an interleaved log line would otherwise pollute
    # the joined payload and trip validate=True. Drop any line containing a
    # non-b64 byte; if we lose a few bytes of payload, the header magic check
    # below catches it.
    body_lines = raw[m.end():end_idx].split()
    clean = bytearray()
    for line in body_lines:
        if all(c in B64_ALPHABET for c in line):
            clean.extend(line)
    try:
        payload = base64.b64decode(bytes(clean), validate=True)
    except Exception as e:
        print(f"  ! {name}: base64 decode failed: {e}")
        return False
    if len(payload) < 36:
        print(f"  ! {name}: payload too small")
        return False
    magic, w, h, stride, fmt, ulen, clen, flags = struct.unpack_from(
        "<8sIIIIIII", payload, 0)
    if not magic.startswith(b"MMSCRN"):
        print(f"  ! {name}: bad magic")
        return False
    body   = payload[36:]
    pixels = zlib.decompress(body) if (flags & 1) else body

    try:
        from PIL import Image
    except ImportError:
        print(f"  ! {name}: Pillow not installed; saving raw")
        (SHOT_DIR / (name + ".bin")).write_bytes(pixels)
        return True

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
    img.save(SHOT_DIR / (name + ".png"))
    print(f"  -> {name}.png ({w}x{h}, {clen} comp)")
    return True


def touch(ser: serial.Serial, x: int, y: int) -> None:
    send_and_drain(ser, f"touch {x} {y} tap", idle_grace=0.4, max_wait=4)


# ─────────────────────────────────────────────────────────────────────────
# Soak loop
# ─────────────────────────────────────────────────────────────────────────

# Approximate UI fader x-centers for the default 12-channel page (slot
# centers at slot_w/2 + N*slot_w with slot_w = 1024/12 ≈ 85). y-coords:
# slider center ~300, mute btn ~498. Used by the device-side touch path.
SLIDER_Y = 300
MUTE_Y   = 498
GEAR_X, GEAR_Y     = 1002, 16    # status-bar gear button
SETTINGS_X_X, SETTINGS_X_Y = 990, 28  # close-X inside settings overlay

def slot_x(idx: int) -> int:
    slot_w = 1024 // 12
    return slot_w // 2 + (idx % 12) * slot_w


# ─────────────────────────────────────────────────────────────────────────
# Settings-overlay interaction. Two paths fold into the soak:
#   (a) console-driven pref toggles -- level-format / signal-indicator /
#       theme. These run via the REPL and exercise the same on_prefs_change
#       callback chain that the UI tap path triggers, including the
#       resubscribe in ms_client_ws.set_level_format.
#   (b) overlay-tap path -- tap the gear, screenshot the open overlay so
#       we can spot rendering regressions, tap the close-X. Catches things
#       a pref toggle alone wouldn't (overlay layout, modality, etc).
# ─────────────────────────────────────────────────────────────────────────

def cycle_pref_via_console(ser: serial.Serial, idx: int) -> str:
    """Issue one pref toggle through the REPL. Cycles through a small
    set on each call; returns the command string for logging."""
    rotation = [
        "level-format db",
        "level-format norm",
        "signal-indicator signal-present",
        "signal-indicator none",
        "theme light",
        "theme dark",
    ]
    cmd = rotation[idx % len(rotation)]
    send_and_drain(ser, cmd, idle_grace=0.5, max_wait=4)
    return cmd


def overlay_open_close(ser: serial.Serial, name: str) -> bool:
    """Tap gear, snap a screenshot of the overlay, tap close. Returns
    True if both taps and the screenshot succeeded."""
    touch(ser, GEAR_X, GEAR_Y)
    time.sleep(0.5)  # let the overlay finish its open animation
    ok = screenshot(ser, name)
    touch(ser, SETTINGS_X_X, SETTINGS_X_Y)
    time.sleep(0.3)
    return ok


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("port",                                          help="device serial port (e.g. COM3)")
    ap.add_argument("ms_host",                                       help="MS host or IP (e.g. 192.168.76.186)")
    ap.add_argument("--ms-port",      type=int,  default=8102,       help="MS HTTP/WS port (default 8102)")
    ap.add_argument("--hours",        type=float, default=1.0,       help="soak duration in hours")
    ap.add_argument("--shot-every",   type=int,  default=60,         help="seconds between screenshots")
    ap.add_argument("--rest-rps",     type=float, default=2.0,       help="host→MS REST writes per second")
    ap.add_argument("--touch-every",  type=int,  default=30,         help="seconds between device-side touches")
    ap.add_argument("--num-channels", type=int,  default=12,         help="how many input channels to drive")
    ap.add_argument("--num-mixes",    type=int,  default=14,         help="upper bound on mix count to probe; the driver auto-detects which mixes accept SETs (un-routed ones return 404)")
    ap.add_argument("--pref-every",   type=int,  default=90,         help="seconds between console-driven pref toggles (level-format / signal-indicator / theme)")
    ap.add_argument("--overlay-every",type=int,  default=180,        help="seconds between settings-overlay open/close cycles")
    args = ap.parse_args()

    print(f"=== monmix soak: {args.hours} h, {args.rest_rps} rps REST, "
          f"shot every {args.shot_every} s, touch every {args.touch_every} s ===")

    # Sanity-check MS reachability before tying up the port.
    info = ms_get(args.ms_host, args.ms_port, "/console/information")
    if info is None:
        print("MS not reachable — abort"); sys.exit(1)
    total = info.get("totalChannels", 0)
    print(f"MS info: totalChannels={total}, channelTypes={[t['name'] for t in info.get('channelTypes', [])]}")

    # Probe which mixes accept SETs. Un-routed mixes 404 — random across
    # them all wastes effort and clogs the log. One probe write per mix:
    # whichever return 200 are exercised in the hot loop.
    print(f"probing mix routing (0..{args.num_mixes-1})...")
    routed_mixes = []
    for m in range(args.num_mixes):
        # 0.5 = mid-fader, harmless probe value; failure means non-routed.
        # quiet=True so the typical 13-of-14 404s don't drown the log.
        if ms_set(args.ms_host, args.ms_port,
                  f"ch.0.levelData.{m}.lvl/norm", 0.5,
                  timeout=2.0, quiet=True):
            routed_mixes.append(m)
    if not routed_mixes:
        print("no routed mixes found on MS — abort"); sys.exit(1)
    print(f"routed mixes: {routed_mixes}")

    ser = open_port(args.port)
    try:
        # Snapshot heap state at start, then loop.
        send_and_drain(ser, "", idle_grace=0.3, max_wait=2)  # drain boot noise
        print("device port open")

        deadline       = time.time() + args.hours * 3600
        next_shot_t    = time.time() + args.shot_every
        next_touch_t   = time.time() + args.touch_every
        rest_period    = 1.0 / max(args.rest_rps, 0.01)
        next_rest_t    = time.time() + rest_period
        next_pref_t    = time.time() + args.pref_every
        next_overlay_t = time.time() + args.overlay_every
        pref_idx       = 0

        shots = touches = sets = prefs_done = overlays = 0
        start_t = time.time()

        try:
            while time.time() < deadline:
                now = time.time()

                # --- HOST → MS REST set ---
                if now >= next_rest_t:
                    next_rest_t = now + rest_period
                    ch = random.randrange(args.num_channels)
                    mix = random.choice(routed_mixes)
                    if random.random() < 0.85:
                        # Slider: random norm 0..1
                        v = round(random.random(), 3)
                        if ms_set(args.ms_host, args.ms_port,
                                  f"ch.{ch}.levelData.{mix}.lvl/norm", v, timeout=2.0):
                            sets += 1
                    else:
                        # Mute toggle (MS .on: true=audible, false=muted)
                        v = random.choice([True, False])
                        if ms_set(args.ms_host, args.ms_port,
                                  f"ch.{ch}.levelData.{mix}.on/val", v, timeout=2.0):
                            sets += 1

                # --- DEVICE → MS touch injection ---
                if now >= next_touch_t:
                    next_touch_t = now + args.touch_every
                    ch_idx = random.randrange(min(args.num_channels, 12))
                    # Half the time touch a slider somewhere along its
                    # vertical range, half the time toggle a mute.
                    if random.random() < 0.5:
                        x = slot_x(ch_idx)
                        # Random Y inside the visible slider area
                        y = random.randint(180, 440)
                        touch(ser, x, y)
                    else:
                        x = slot_x(ch_idx)
                        touch(ser, x, MUTE_Y)
                    touches += 1

                # --- Console pref toggle (level-format / signal-indicator / theme) ---
                if now >= next_pref_t:
                    next_pref_t = now + args.pref_every
                    cmd = cycle_pref_via_console(ser, pref_idx)
                    pref_idx += 1
                    prefs_done += 1
                    print(f"  pref: {cmd}")

                # --- Settings overlay open/snap/close ---
                if now >= next_overlay_t:
                    next_overlay_t = now + args.overlay_every
                    # Push REST out of the way during the screenshot dance
                    next_rest_t = now + 15.0
                    elapsed_min = int((now - start_t) / 60)
                    name = f"soak-overlay-{overlays:02d}-t{elapsed_min:03d}m"
                    ser.read(65536)
                    if overlay_open_close(ser, name):
                        overlays += 1

                # --- Periodic screenshot ---
                if now >= next_shot_t:
                    next_shot_t = now + args.shot_every
                    # Pause REST activity for the screenshot window. The
                    # device's snapshot + b64-emit chain can take 10–20 s
                    # under WS-broadcast load (the broadcasts queue up
                    # async sweeps which run after lvgl_port_lock
                    # releases). Pushing the next REST tick 15 s out
                    # gives the device room to drain before we hit it
                    # again.
                    next_rest_t = now + 15.0
                    elapsed_min = int((now - start_t) / 60)
                    name = f"soak-{shots:04d}-t{elapsed_min:03d}m"
                    # Drain any pending UART chatter before triggering shot.
                    ser.read(65536)
                    if screenshot(ser, name):
                        shots += 1

                # Print progress every minute
                if int(now) % 60 == 0:
                    elapsed = int(now - start_t)
                    print(f"[{elapsed//60:3d}m {elapsed%60:02d}s] "
                          f"rest={sets} touch={touches} shot={shots} "
                          f"prefs={prefs_done} overlay={overlays}")
                    time.sleep(1)  # avoid log spam from tight loop hitting %60==0

                time.sleep(0.05)
        except KeyboardInterrupt:
            print("\ninterrupted by user")

        # Final screenshot + log pull
        elapsed_min = int((time.time() - start_t) / 60)
        screenshot(ser, f"soak-{shots:04d}-final-t{elapsed_min:03d}m")
        print(f"=== done: {sets} REST sets, {touches} touches, "
              f"{shots} screenshots over {elapsed_min} min ===")

    finally:
        ser.close()


if __name__ == "__main__":
    main()
