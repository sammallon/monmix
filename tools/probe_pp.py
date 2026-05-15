"""Starter probe for the ProPresenter REST + streaming API.

Drives the 8 open empirical questions in INVESTIGATION.md against a
live ProPresenter instance. Read-only — no triggers, no state changes.

Usage:
    python tools/probe_pp.py <host> [port]

    python tools/probe_pp.py 192.168.0.10
    python tools/probe_pp.py propresenter.local 49850

Defaults to port 49850 (ProPresenter's typical Network/API port; user
configures it in ProPresenter > Preferences > Network).

Output:
    - Stdout: human-readable progress, JSON dumps of key responses
    - probe-out/<timestamp>/*.json - one file per endpoint
    - probe-out/<timestamp>/stream.log - first N seconds of the
      chunked status feed, with per-frame timing

Iterate this script as findings come in; goal is to lock down the
wire format precisely enough that the firmware port doesn't have to
guess. Save findings back into ../INVESTIGATION.md section 4.
"""

import json
import os
import socket
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime


DEFAULT_PORT = 49850
STREAM_SECONDS = 20
STREAM_PATHS = [
    "status/slide",
    "presentation/active",
    "presentation/slide_index",
    "timer/system_time",
    "timers/current",
    "stage/message",
    "look/current",
    "status/stage_screens",
    "announcement/active",
]


def base(host, port):
    return f"http://{host}:{port}"


def get(url, timeout=5.0):
    """Simple GET. Returns (status, headers, body_bytes)."""
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read() if e.fp else b""


def get_json(url, timeout=5.0):
    """GET expecting JSON; returns parsed dict or sentinel on failure."""
    status, hdrs, body = get(url, timeout)
    if status != 200:
        return {"_status": status, "_body": body[:200].decode("utf-8", "replace")}
    try:
        return json.loads(body)
    except Exception as e:
        return {"_err": str(e), "_body": body[:200].decode("utf-8", "replace")}


def save_json(out_dir, name, data):
    path = os.path.join(out_dir, f"{name}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    print(f"  -> {path}")


def probe_endpoints(host, port, out_dir):
    b = base(host, port)
    summary = {}

    print("[Q8] Look for version-advertising endpoints")
    for path in ("/version", "/v1/version", "/v1/info", "/v1/server"):
        status, hdrs, body = get(b + path)
        print(f"  GET {path} -> {status}")
        if status == 200 and body:
            summary[f"version_probe{path}"] = body[:200].decode("utf-8", "replace")

    print("[Q7] Auth check: try /v1/status/slide unauthenticated")
    s, h, body = get(b + "/v1/status/slide")
    summary["auth_status_slide"] = s
    print(f"  GET /v1/status/slide -> {s} ({len(body)} bytes)")

    print("[Q2/Q9] /v1/status/slide payload (text content drives rendering)")
    sslide = get_json(b + "/v1/status/slide")
    save_json(out_dir, "status_slide", sslide)
    # Also dump raw bytes — text richness / encoding details may be
    # lost through json.loads roundtrip
    s, hdrs, body = get(b + "/v1/status/slide")
    with open(os.path.join(out_dir, "status_slide.raw.json"), "wb") as f:
        f.write(body)
    print(f"  raw bytes: {len(body)}; content-type={hdrs.get('Content-Type','?')}")

    for ep in (
        "/v1/presentation/active",
        "/v1/presentation/slide_index",
        "/v1/timer/system_time",
        "/v1/timers/current",
        "/v1/stage/message",
        "/v1/look/current",
        "/v1/status/stage_screens",
        "/v1/announcement/active",
        "/v1/stage/layouts",
        "/v1/stage/screens",
    ):
        name = ep.replace("/", "_").strip("_")
        save_json(out_dir, name, get_json(b + ep))

    print("[Q4] High-resolution background media access?")
    print("     (thumbnails are too low-res for primary display; checking")
    print("      for a separate higher-res image surface per slide)")
    # First: confirm thumbnail is low-res as expected (gallery-grade only).
    # Then: probe for any image endpoint that might give higher fidelity.
    layouts = get_json(b + "/v1/stage/layouts")
    if isinstance(layouts, list) and layouts:
        first = layouts[0]
        layout_id = first.get("id") or first.get("uuid")
        if layout_id:
            print(f"  Stage-layout thumbnail probe (expect low-res):")
            s, hdrs, body = get(f"{b}/v1/stage/layout/{layout_id}/thumbnail?quality=75")
            ct = hdrs.get("Content-Type", "?")
            print(f"    /v1/stage/layout/.../thumbnail -> {s}, {len(body)} bytes, ct={ct}")
            if s == 200:
                ext = "jpg" if "jpeg" in ct else ("png" if "png" in ct else "bin")
                p = os.path.join(out_dir, f"stage_layout_{layout_id}.{ext}")
                with open(p, "wb") as f:
                    f.write(body)
                summary["stage_layout_thumb"] = {"path": p, "bytes": len(body), "ct": ct}
    # Themes may expose slide background media at higher resolution; try
    # to list themes and probe the first theme slide's surface.
    themes = get_json(b + "/v1/themes")
    if isinstance(themes, list) and themes:
        t0 = themes[0]
        tid = t0.get("id") or t0.get("uuid")
        if tid:
            print(f"  Theme slide probe (theme {tid})")
            for tslide in ("0", "current"):
                p1 = f"{b}/v1/theme/{urllib.parse.quote(str(tid))}/slides/{tslide}"
                p2 = p1 + "/thumbnail?quality=100"
                s, h, body = get(p1)
                print(f"    GET {p1.split(host)[1]} -> {s}, {len(body)} bytes")
                if s == 200:
                    summary[f"theme_slide_{tslide}"] = body[:200].decode("utf-8", "replace")
                s, h, body = get(p2)
                print(f"    GET {p2.split(host)[1]} -> {s}, {len(body)} bytes, "
                      f"ct={h.get('Content-Type','?')}")

    print("[Q3] Slide thumbnail format/size sweep (gallery-grade only)")
    active = get_json(b + "/v1/presentation/active")
    pid = None
    if isinstance(active, dict):
        # PP wraps the id in various shapes across versions; try a few
        node = active.get("presentation") or active
        if isinstance(node, dict):
            ident = node.get("id") or node.get("identifier") or {}
            if isinstance(ident, dict):
                pid = ident.get("uuid") or ident.get("id")
            else:
                pid = ident
            pid = pid or node.get("uuid")
    if pid:
        for q in (10, 50, 75, 100):
            s, hdrs, body = get(
                f"{b}/v1/presentation/{urllib.parse.quote(str(pid))}/thumbnail/0?quality={q}"
            )
            ct = hdrs.get("Content-Type", "?")
            print(f"  q={q}: {s}, {len(body)} bytes, ct={ct}")
            if s == 200 and q == 75:
                ext = "jpg" if "jpeg" in ct else ("png" if "png" in ct else "bin")
                p = os.path.join(out_dir, f"thumb_slide0_q75.{ext}")
                with open(p, "wb") as f:
                    f.write(body)
    else:
        print("  no active presentation uuid found; skipping thumbnail sweep")

    return summary


def probe_stream(host, port, out_dir, seconds):
    """Q1 + Q5 + Q6: chunked /v1/status/updates."""
    print(f"[Q1/Q5/Q6] Subscribe to {len(STREAM_PATHS)} paths via /v1/status/updates")
    body = json.dumps(STREAM_PATHS).encode("utf-8")

    s = socket.create_connection((host, port), timeout=5.0)
    s.settimeout(2.0)

    req = (
        f"POST /v1/status/updates HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Content-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n"
        f"Accept: application/json\r\n"
        f"Connection: keep-alive\r\n"
        f"\r\n"
    ).encode("ascii") + body
    s.sendall(req)

    log_path = os.path.join(out_dir, "stream.log")
    print(f"  Listening for {seconds} s, logging to {log_path}")

    t0 = time.monotonic()
    deadline = t0 + seconds
    buf = b""
    frames_seen = 0
    by_url = {}
    header_done = False

    with open(log_path, "w", encoding="utf-8") as logf:
        while time.monotonic() < deadline:
            try:
                chunk = s.recv(8192)
            except socket.timeout:
                continue
            if not chunk:
                logf.write(f"\n[t={time.monotonic()-t0:.3f}] CONNECTION CLOSED\n")
                print("  ! Server closed connection")
                break
            buf += chunk

            if not header_done and b"\r\n\r\n" in buf:
                hdr, _, rest = buf.partition(b"\r\n\r\n")
                logf.write(f"[HTTP HEADER]\n{hdr.decode('utf-8','replace')}\n[/HTTP HEADER]\n\n")
                buf = rest
                header_done = True

            if not header_done:
                continue

            while b"\r\n\r\n" in buf:
                frame, _, buf = buf.partition(b"\r\n\r\n")
                frame_str = frame.decode("utf-8", "replace").strip()
                if not frame_str:
                    continue
                frames_seen += 1
                ts = time.monotonic() - t0
                try:
                    obj = json.loads(frame_str)
                    url = obj.get("url", "<no-url>")
                    by_url[url] = by_url.get(url, 0) + 1
                    logf.write(
                        f"[t={ts:.3f}] {url} :: "
                        f"{json.dumps(obj, ensure_ascii=False)[:500]}\n"
                    )
                except Exception as e:
                    logf.write(f"[t={ts:.3f}] PARSE-ERR {e}: {frame_str[:200]}\n")

    s.close()
    print(f"  Saw {frames_seen} frames in {seconds} s:")
    for url, n in sorted(by_url.items(), key=lambda kv: -kv[1]):
        print(f"    {n:4d}  {url}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = os.path.join("probe-out", ts)
    os.makedirs(out_dir, exist_ok=True)
    print(f"Probing {host}:{port} -> {out_dir}\n")

    summary = probe_endpoints(host, port, out_dir)
    print()
    probe_stream(host, port, out_dir, STREAM_SECONDS)

    save_json(out_dir, "_summary", summary)
    print(f"\nDone. Findings in {out_dir}. Update ../INVESTIGATION.md section 4.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
