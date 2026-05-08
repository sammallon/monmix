"""Master tile color picker: tapping the swatch opens the color
picker with title "Color: <master name>"; tapping a palette button
writes the choice through to app_prefs keyed by the master's MS
channel id (mix_offset + mix_idx = 60 in the mock).
"""

TEST = {
    "name": "config_master_tile_color",
    "description": "Tap master tile swatch + pick red; pref persists for master id.",
    "args": [],
    # Master tile [3][5] inside settings overlay (with pad 16):
    #   tile screen rect (763, 548)-(1002, 590)
    #   swatch RIGHT_MID, sz 28: rect (968, 555)-(996, 583).
    #   tap (982, 569) hits.
    # Color picker (300x340, centered on 1024x600): popup at
    # (362, 130)-(662, 470), pad 20 -> content (382, 150).
    # Button 0 (red): pos (0, 40) -> screen rect (382, 190)-(462, 270).
    # Tap (422, 230) hits.
    "script": (
        "echo before-pick\n"
        "prefs_get_color 60\n"
        "tap 1000 16\n"          # gear -> settings overlay
        "sleep 200\n"
        "tap 982 569\n"          # master swatch -> color picker
        "sleep 300\n"
        "tap 422 230\n"          # red (palette index 0)
        "sleep 200\n"
        "echo after-pick\n"
        "prefs_get_color 60\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "before-pick",
            # Default unset -> -1.
            "OK prefs_get_color id=60 color=-1",
            "after-pick",
            "OK prefs_get_color id=60 color=0",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
    "hw_compatible": False,
}
