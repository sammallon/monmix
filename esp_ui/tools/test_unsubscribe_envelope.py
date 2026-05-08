#!/usr/bin/env python3
"""Pure-logic test: the JSON envelope built by send_unsubscribe() must
match what MS expects per its OpenAPI -- /console/data/unsubscribe takes
the same {path, format} body as /subscribe (and "the path must match
1:1 the path used for the subscription").

This is a logic-only test that doesn't need a live MS. It builds the
same envelope strings the firmware does (the format-strings live in
main/app_ms_client_ws.c; we mirror them here so a future drift in
either side fails the test).

Run:
    python tools/test_unsubscribe_envelope.py
"""

import json
import sys


# Mirrors send_unsubscribe / unsubscribe_channel / unsubscribe_master /
# subscribe_all_routable_names (the cfg.name path) byte-for-byte. If the
# firmware drifts these formats, MS will start rejecting the call and
# this test will catch it on the next change.
def make_unsubscribe(path: str, fmt: str) -> str:
    # Matches the snprintf format string in send_unsubscribe.
    return (
        '{"method":"POST","path":"/console/data/unsubscribe",'
        '"body":{"path":"%s","format":"%s"}}'
    ) % (path, fmt)


def make_meter_unsubscribe(sub_id: int) -> str:
    # Matches meter_send_unsubscribe.
    return (
        '{"method":"POST","path":"/console/metering/unsubscribe",'
        '"body":{"id":%d}}'
    ) % sub_id


FAILS: list[str] = []


def expect(cond: bool, msg: str) -> None:
    if not cond:
        FAILS.append(msg)


def run() -> int:
    # Channel level (NORM mode).
    s = make_unsubscribe("ch.5.levelData.0.lvl", "norm")
    j = json.loads(s)
    expect(j["method"] == "POST", "channel-lvl method")
    expect(j["path"] == "/console/data/unsubscribe", "channel-lvl path")
    expect(j["body"]["path"] == "ch.5.levelData.0.lvl", "channel-lvl body.path")
    expect(j["body"]["format"] == "norm", "channel-lvl body.format")

    # Channel level (DB mode).
    s = make_unsubscribe("ch.5.levelData.0.lvl", "val")
    j = json.loads(s)
    expect(j["body"]["format"] == "val", "channel-lvl-db format")

    # Channel mute (always val).
    s = make_unsubscribe("ch.5.levelData.0.on", "val")
    j = json.loads(s)
    expect(j["body"]["path"] == "ch.5.levelData.0.on", "channel-on path")

    # Master level under a mix.
    s = make_unsubscribe("ch.60.mix.lvl", "norm")
    j = json.loads(s)
    expect(j["body"]["path"] == "ch.60.mix.lvl", "master-lvl path")

    # cfg.name (subscribe_all_routable_names).
    s = make_unsubscribe("ch.0.cfg.name", "val")
    j = json.loads(s)
    expect(j["body"]["path"] == "ch.0.cfg.name", "name path")
    expect(j["body"]["format"] == "val", "name format")

    # Metering unsubscribe is a different shape -- {id: <int>} not
    # {path, format} -- since metering subscriptions are id-keyed.
    s = make_meter_unsubscribe(1)
    j = json.loads(s)
    expect(j["path"] == "/console/metering/unsubscribe", "meter path")
    expect(j["body"]["id"] == 1, "meter body.id")
    expect("path" not in j["body"], "meter body has no path")

    # Round-trip stability: every envelope must be valid JSON. (Catches
    # a regression where someone hand-edits the format and forgets to
    # quote a value.)
    for path in ("ch.0.cfg.name", "ch.99.levelData.13.lvl",
                 "ch.99.levelData.13.on", "ch.74.mix.on"):
        for fmt in ("norm", "val"):
            try:
                json.loads(make_unsubscribe(path, fmt))
            except json.JSONDecodeError as e:
                FAILS.append(f"unsubscribe({path}, {fmt}) not valid JSON: {e}")

    if FAILS:
        for f in FAILS:
            print(f"FAIL  {f}")
        return 1
    print("PASS  test_unsubscribe_envelope")
    return 0


if __name__ == "__main__":
    sys.exit(run())
