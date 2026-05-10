"""Drag-to-reorder must NOT open the rename or color picker popups.

Pilot bug 2026-05-09: lifting the finger after a long-press + drag
fired LV_EVENT_CLICKED on the source name label, opening the
rename (scribble-strip) popup every time. The color picker is
defensively guarded by the same flag in case PRESS_LOCK lets a
swatch CLICKED through during a drag.

This test asserts the absence of "rename: open" and "picker: open"
log lines after a long-press + drag + release. Existing tests
(test_config_master_tile_rename.py) verify normal taps still open
the rename popup, so the suppression isn't accidentally too broad.
"""

TEST = {
    "name": "config_drag_no_rename",
    "description": "Drag gesture suppresses rename + color picker popups.",
    "args": [],
    "script": (
        "tap 1002 16\n"           # gear -> settings overlay
        "sleep 200\n"
        "echo drag-start\n"
        # Long-press + drag + release. Whether LVGL fires CLICKED
        # on release depends on drag distance vs scroll threshold;
        # the suppression flag handles both cases.
        "press 100 336\n"
        "sleep 600\n"             # > 400 ms long-press threshold
        "move 100 376\n"
        "sleep 200\n"
        "release\n"
        "sleep 300\n"
        "echo drag-end\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "drag-start",
            "drag-end",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            # The bug pre-fix: dragging fires LV_EVENT_CLICKED on
            # the source name label, which calls rename_open. Now
            # suppressed by s_reorder_was_active flag check.
            "rename: open",
            # Defensive: PRESS_LOCK normally keeps the swatch from
            # receiving CLICKED during a name-label drag, but the
            # same flag guards on_swatch_clicked too.
            "picker: open",
        ],
    },
    "hw_compatible": True,
    "hw_script": (
        "sleep 3500\n"
        "tap 1002 16\n"
        "sleep 1500\n"
        "press 100 336\n"
        "sleep 600\n"
        "move 100 376\n"
        "sleep 200\n"
        "release\n"
        "sleep 800\n"
    ),
    "hw_expect": {
        "exit_code": 0,
        "stdout_not_contains": [
            "LV_ASSERT",
            "panic",
            "abort",
            "rename: open",
            "picker: open",
        ],
    },
}

