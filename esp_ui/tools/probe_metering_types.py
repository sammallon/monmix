"""Probe different metering 'type' values for one channel to see which one
reports non-silence when audio is flowing. Si Expression docs aren't
explicit about what type 0/1/2/3 mean -- map them empirically."""
import argparse
import base64
import json
import struct
import sys
import time

import websocket


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.1.20.64")
    ap.add_argument("--port", default=8080, type=int)
    ap.add_argument("--ch",   default=10,   type=int)
    ap.add_argument("--dur",  default=6.0,  type=float)
    args = ap.parse_args()

    ws = websocket.create_connection(f"ws://{args.host}:{args.port}/", timeout=5)

    # Subscribe channel under types 0..3 in one batch; the broadcast
    # frame will pack 4 int16s per snapshot, in subscribe order.
    types = [0, 1, 2, 3]
    params = [{"index": args.ch, "type": t} for t in types]
    ws.send(json.dumps({
        "method": "POST",
        "path":   "/console/metering2/subscribe",
        "body":   {"binary": True, "interval": 100, "id": 1, "params": params},
    }))
    print(f"watching ch.{args.ch} types {types} for {args.dur}s")

    deadline = time.time() + args.dur
    ws.settimeout(0.6)
    per_type_max = [-200.0] * len(types)
    per_type_first = [None] * len(types)
    n = 0
    while time.time() < deadline:
        try:
            msg = ws.recv()
        except websocket.WebSocketTimeoutException:
            continue
        j = json.loads(msg)
        if not j.get("path", "").startswith("/console/metering2/"):
            continue
        b = j.get("body", {}).get("b", "")
        pad = (4 - len(b) % 4) % 4
        raw = base64.b64decode(b + "=" * pad)
        vals = [struct.unpack(">h", raw[i:i+2])[0] / 100.0 for i in range(0, len(raw), 2)]
        n += 1
        for i, v in enumerate(vals):
            if i >= len(per_type_max):
                continue
            if v > per_type_max[i]:
                per_type_max[i] = v
            if per_type_first[i] is None:
                per_type_first[i] = v

    ws.send(json.dumps({"method":"POST","path":"/console/metering/unsubscribe","body":{"id":1}}))
    ws.close()
    print(f"\n{n} frames received")
    for i, t in enumerate(types):
        print(f"  type={t}  first={per_type_first[i]}  peak={per_type_max[i]:.1f} dB")


if __name__ == "__main__":
    main()
