"""High-rate MS REST hammer. Doesn't open COM3 -- pairs with serial_tail to
capture device-side UART during storm reproduction.

Usage:
    python tools/hammer_ms_rest.py <ms_host> [duration_sec] [rps]
"""
import json
import random
import sys
import time
import urllib.request


def ms_set(host: str, port: int, path: str, value, timeout: float = 1.5) -> bool:
    url = f"http://{host}:{port}/console/data/set/{path}"
    body = json.dumps({"value": value}).encode()
    req = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"}, method="POST"
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout):
            return True
    except Exception:
        return False


def main() -> None:
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    host = sys.argv[1]
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 600.0
    rps = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
    period = 1.0 / max(rps, 0.01)

    deadline = time.time() + duration
    sets = 0
    fails = 0
    next_t = time.time()
    next_log_t = time.time() + 30
    print(f"hammering {host}:8102 for {duration:.0f} s at {rps:.1f} rps", flush=True)
    while time.time() < deadline:
        now = time.time()
        if now >= next_t:
            next_t = now + period
            ch = random.randrange(12)
            mix = random.randrange(14)
            v = round(random.random(), 3)
            if ms_set(host, 8102, f"ch.{ch}.levelData.{mix}.lvl/norm", v):
                sets += 1
            else:
                fails += 1
        if now >= next_log_t:
            next_log_t = now + 30
            elapsed = int(now - (deadline - duration))
            print(f"[{elapsed:4d}s] sets={sets} fails={fails}", flush=True)
        time.sleep(0.005)
    print(f"done. sets={sets} fails={fails}", flush=True)


if __name__ == "__main__":
    main()
