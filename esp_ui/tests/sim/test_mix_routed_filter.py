"""Routed-mix filter: only mixes in /console/mixTargets appear in picker.

Regressed when c15e337 substituted cesanta/mongoose for esp_websocket_client;
ws_is_mix_routed/ws_fetch_mix_routing landed as TODO stubs returning true
for all indices. Restored via the original 42449bc approach: blocking REST
GET /console/mixTargets, parse targets[].id, populate a bool mask.

Live-MS only — the mock has no real MS to query. Skipped without
MONMIX_MS_HOST. Asserts the picker's "N/M routed" log line from
build_mix_picker_popup matches the count reported by ms_real's
mixTargets fetch.

The test is intentionally agnostic about the exact count of routed
mixes — whoever runs the test against their own MS instance gets
whichever subset their profile exposes. It only checks that the two
counts (picker vs mixTargets fetch) agree and that the picker count
is strictly less than mix_count (otherwise the filter is wholly
inactive — that's the pre-fix state).
"""
import os
import re

# Mix indicator: aligned TOP_RIGHT -154 with min_width 90, height 28.
# Right edge ~= 1024 - 154 = 870; left ~= 870 - 90 = 780; vert ~= 16.
MIX_IND_X = 825
MIX_IND_Y = 16


def _verify(stdout, _stderr):
    # "ms_real: mix routing N/M in profile"
    m1 = re.search(r"ms_real: mix routing (\d+)/(\d+) in profile", stdout)
    # "mix picker: N/M routed"
    m2 = re.search(r"mix picker: (\d+)/(\d+) routed", stdout)
    errors = []
    if not m1:
        errors.append("missing 'ms_real: mix routing N/M in profile' line "
                      "(mixTargets fetch never ran or failed)")
    if not m2:
        errors.append("missing 'mix picker: N/M routed' line "
                      "(build_mix_picker_popup never ran; "
                      "did the mix-indicator tap miss?)")
    if m1 and m2:
        fetch_routed, fetch_total = int(m1.group(1)), int(m1.group(2))
        pick_routed,  pick_total  = int(m2.group(1)), int(m2.group(2))
        if pick_routed != fetch_routed:
            errors.append(
                f"picker showed {pick_routed} mixes but mixTargets reported "
                f"{fetch_routed} routed; ws_is_mix_routed not honoring mask")
        if pick_total != fetch_total:
            errors.append(
                f"mix_count mismatch: picker={pick_total} fetch={fetch_total}")
        if fetch_routed >= fetch_total:
            errors.append(
                f"every mix routed ({fetch_routed}/{fetch_total}); test "
                "needs an MS profile that filters at least one mix to be "
                "meaningful")
    return errors


TEST = {
    "name": "mix_routed_filter",
    "description": "Mix picker filters out un-routed mixes per /console/mixTargets.",
    "args": ["--ms-host", os.environ.get("MONMIX_MS_HOST", ""),
             "--ms-port", os.environ.get("MONMIX_MS_PORT", "8080")],
    "skip_if": lambda: not os.environ.get("MONMIX_MS_HOST"),
    "skip_reason": "live-MS required (set MONMIX_MS_HOST)",
    "timeout_s": 30,
    "script": (
        # Wait for WS connect + /console/information + /console/mixTargets
        # to land. Live MS at 8080 typically responds in <200 ms; 4 s is
        # generous.
        "sleep 4000\n"
        # Dismiss the boot wake-menu (a duration row near panel center).
        f"tap 512 280\n"
        "sleep 400\n"
        # Tap the mix indicator to open the picker.
        f"tap {MIX_IND_X} {MIX_IND_Y}\n"
        "sleep 600\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "ms_real: mix routing",
            "in profile",
            "mix picker:",
            "routed",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "panic",
            "abort",
            "mixTargets fetch failed",
        ],
        "verify": _verify,
    },
}
