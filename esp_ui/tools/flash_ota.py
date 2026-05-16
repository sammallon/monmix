#!/usr/bin/env python3
"""Network-OTA flasher for monmix.

Workflow:
  1. Spawn a one-shot HTTP server on this machine that serves the
     supplied .bin file at a unique path.
  2. Connect to <device-ip>:4242 (the netconsole listener).
  3. Authenticate using APP_NET_TOKEN (read from the device-side
     `secrets.h` -- pass via --token or the MONMIX_NET_TOKEN env var).
  4. Send `ota http://<my-ip>:<port>/<filename>` and stream progress
     until OTA_FINISH or OTA_ERROR.

Usage:
    python tools/flash_ota.py --host <device-ip> [--port 4242]
        [--token <token>] [--bin build/esp_ui.bin]

If --bin is omitted, the most recent build/esp_ui.bin under the repo's
esp_ui directory is used.
"""

import argparse
import http.server
import os
import socket
import socketserver
import sys
import threading
import time
from pathlib import Path

DEFAULT_BIN = Path(__file__).resolve().parent.parent / "build" / "esp_ui.bin"
NETCON_PORT = 4242
# Fixed HTTP port for the firmware-serving listener. Pinned (rather than
# ephemeral) so router firewall rules can be scoped to this one port instead
# of having to allow any high TCP port. Override with --http-port if it
# collides with something else on the host machine.
HTTP_BIND_PORT = 18080


def detect_local_ip(target_host: str) -> str:
    """Return the local IP that would route to target_host.

    Trick: open a UDP socket "connected" to the target; getsockname
    returns the local address the kernel chose for that route. No
    packets actually go out (UDP connect just sets the destination).
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((target_host, 9))
        return s.getsockname()[0]
    finally:
        s.close()


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler that doesn't log to stderr."""
    def log_message(self, fmt, *args):
        pass


def start_http_server(bin_path: Path, http_port: int) -> tuple[socketserver.TCPServer, int, str]:
    """Serve bin_path's directory; return (server, port, basename).

    The device fetches `http://<our_ip>:<port>/<basename>`.
    """
    dirname = bin_path.parent
    basename = bin_path.name
    os.chdir(dirname)  # SimpleHTTPRequestHandler serves from cwd
    server = socketserver.TCPServer(("0.0.0.0", http_port), QuietHandler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, port, basename


def netcon_session(host: str, port: int, token: str, ota_url: str) -> int:
    """Connect to device netconsole, auth, trigger OTA, stream output.

    Returns 0 on OTA_FINISH ok, non-zero on error / timeout.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(15.0)
    s.connect((host, port))

    def send_line(line: str):
        s.sendall((line + "\n").encode())

    # Recv loop accumulator: TCP doesn't preserve message boundaries,
    # so split on '\n' explicitly.
    buf = b""

    def recv_line(timeout: float = 5.0) -> str | None:
        nonlocal buf
        s.settimeout(timeout)
        while b"\n" not in buf:
            try:
                chunk = s.recv(1024)
            except socket.timeout:
                return None
            if not chunk:
                return None
            buf += chunk
        line, _, buf = buf.partition(b"\n")
        return line.decode(errors="replace").rstrip("\r")

    # Drain greeting.
    while True:
        line = recv_line(2.0)
        if line is None:
            break
        print(f"[device] {line}")
        if line.startswith("auth:"):
            break

    send_line(f"auth {token}")
    line = recv_line(5.0)
    print(f"[device] {line}")
    if not line or not line.startswith("OK"):
        print("[ERR] auth failed", file=sys.stderr)
        return 2

    print(f"[host] sending: ota {ota_url}")
    send_line(f"ota {ota_url}")

    # Stream until OTA_FINISH / OTA_ERROR or timeout.
    deadline = time.monotonic() + 300.0  # 5 min upper bound
    rc = 1
    while time.monotonic() < deadline:
        line = recv_line(30.0)
        if line is None:
            print("[ERR] no progress in 30 s; aborting", file=sys.stderr)
            rc = 3
            break
        print(f"[device] {line}")
        if line.startswith("OTA_FINISH"):
            rc = 0 if "ok" in line else 4
            break
        if line.startswith("OTA_ERROR"):
            rc = 5
            break
    s.close()
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True,
                    help="Device IP address (the CrowPanel running monmix)")
    ap.add_argument("--port", type=int, default=NETCON_PORT,
                    help=f"Netconsole port (default {NETCON_PORT})")
    ap.add_argument("--token",
                    default=os.environ.get("MONMIX_NET_TOKEN"),
                    help="APP_NET_TOKEN from secrets.h (or env MONMIX_NET_TOKEN)")
    ap.add_argument("--bin", default=str(DEFAULT_BIN),
                    help=f"Firmware .bin path (default: {DEFAULT_BIN})")
    ap.add_argument("--http-port", type=int, default=HTTP_BIND_PORT,
                    help=f"TCP port to bind the firmware-serving HTTP server on "
                         f"(default {HTTP_BIND_PORT}; routers expect this port "
                         f"for cross-subnet OTA fetches)")
    args = ap.parse_args()

    if not args.token:
        print("ERROR: --token or MONMIX_NET_TOKEN required", file=sys.stderr)
        return 2

    bin_path = Path(args.bin).resolve()
    if not bin_path.is_file():
        print(f"ERROR: firmware not found: {bin_path}", file=sys.stderr)
        return 2

    print(f"[host] firmware: {bin_path} ({bin_path.stat().st_size:,} bytes)")
    server, http_port, basename = start_http_server(bin_path, args.http_port)
    local_ip = detect_local_ip(args.host)
    url = f"http://{local_ip}:{http_port}/{basename}"
    print(f"[host] serving at {url}")

    try:
        rc = netcon_session(args.host, args.port, args.token, url)
    finally:
        server.shutdown()
        server.server_close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
