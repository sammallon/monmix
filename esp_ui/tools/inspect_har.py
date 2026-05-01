"""One-off HAR inspector for Mixing Station traffic discovery.

Pulls out:
  - unique HTTP request shapes (method + URL path + body excerpt)
  - all WebSocket frames (direction, opcode, payload excerpt)

Usage: python tools/inspect_har.py <path-to.har>
"""
import json
import sys
from collections import OrderedDict


def short(s, n=400):
    if s is None:
        return ""
    s = s if isinstance(s, str) else str(s)
    return s if len(s) <= n else s[:n] + f"...<+{len(s)-n}>"


def main(path):
    with open(path, "r", encoding="utf-8") as f:
        har = json.load(f)

    entries = har["log"]["entries"]
    print(f"# {len(entries)} entries\n")

    http_seen = OrderedDict()
    ws_entries = []
    for e in entries:
        rtype = e.get("_resourceType") or e.get("_webSocketMessages") and "websocket"
        if e.get("_webSocketMessages"):
            ws_entries.append(e)
            continue
        req = e["request"]
        url = req["url"]
        method = req["method"]
        body = (req.get("postData") or {}).get("text")
        key = (method, url.split("?")[0])
        if key not in http_seen:
            http_seen[key] = {
                "first_url": url,
                "body": body,
                "status": e["response"]["status"],
                "resp_excerpt": short((e["response"].get("content") or {}).get("text"), 300),
            }

    print("## HTTP unique shapes")
    for (method, path_only), info in http_seen.items():
        print(f"\n{method} {path_only}")
        print(f"  full url:   {info['first_url']}")
        print(f"  status:     {info['status']}")
        if info["body"]:
            print(f"  req body:   {short(info['body'], 300)}")
        if info["resp_excerpt"]:
            print(f"  resp body:  {info['resp_excerpt']}")

    print("\n## WebSocket sessions")
    for e in ws_entries:
        url = e["request"]["url"]
        msgs = e.get("_webSocketMessages", [])
        print(f"\nWS {url}  ({len(msgs)} frames)")
        for i, m in enumerate(msgs):
            direction = "->" if m.get("type") == "send" else "<-"
            data = m.get("data", "")
            print(f"  [{i:04d}] {direction} {short(data, 500)}")


if __name__ == "__main__":
    main(sys.argv[1])
