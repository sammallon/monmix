"""Probe MS metering: open WS, subscribe to channel 10 (Keys L), watch
broadcasts for 5 s. Tells us whether the wire-level subscribe shape
works against the live console — separating "firmware bug" from
"protocol misunderstanding"."""
import argparse
import base64
import json
import struct
import sys
import time

try:
    import websocket
except ImportError:
    print("pip install websocket-client", file=sys.stderr)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.1.20.64")
    ap.add_argument("--port", default=8080, type=int)
    ap.add_argument("--ch",   default=10,   type=int, help="channel to watch (10=Keys L)")
    ap.add_argument("--dur",  default=5.0,  type=float)
    args = ap.parse_args()

    url = f"ws://{args.host}:{args.port}/"
    print(f"connecting {url}")
    ws = websocket.create_connection(url, timeout=5)

    sub = {
        "method": "POST",
        "path":   "/console/metering2/subscribe",
        "body": {
            "binary":   True,
            "interval": 100,
            "id":       1,
            "params":   [{"index": args.ch, "type": 0}],
        },
    }
    ws.send(json.dumps(sub))
    print(f"subscribed ch.{args.ch} type=0 interval=100ms")

    deadline = time.time() + args.dur
    n_meter  = 0
    n_other  = 0
    samples  = []
    ws.settimeout(0.5)
    while time.time() < deadline:
        try:
            msg = ws.recv()
        except websocket.WebSocketTimeoutException:
            continue
        try:
            j = json.loads(msg)
        except Exception:
            n_other += 1
            continue
        path = j.get("path", "")
        if path.startswith("/console/metering2/"):
            n_meter += 1
            b64 = j.get("body", {}).get("b", "")
            if b64:
                pad = (4 - len(b64) % 4) % 4
                raw = base64.b64decode(b64 + ("=" * pad))
                # Big-endian int16, scale=100 -> dB
                vals = [struct.unpack(">h", raw[i:i+2])[0] / 100.0
                        for i in range(0, len(raw), 2)]
                samples.append(vals)
        else:
            n_other += 1

    # Unsubscribe so MS doesn't keep blasting at us
    ws.send(json.dumps({"method":"POST","path":"/console/metering/unsubscribe","body":{"id":1}}))
    ws.close()

    print(f"\n{n_meter} metering broadcasts, {n_other} other")
    if samples:
        flat = [v for arr in samples for v in arr]
        print(f"db range: {min(flat):.1f} .. {max(flat):.1f}")
        print(f"first 8 samples: {samples[:8]}")
    else:
        print("no metering broadcasts received")
        sys.exit(1)


if __name__ == "__main__":
    main()
