"""Drag-to-rearrange in the settings overlay's channel grid.

Default 8 channels with ids 0..7; tile [0] (top-left) corresponds to
ms_id=0, tile [1] (column 1, row 0) to ms_id=1. Long-pressing tile
[0]'s name then dragging the pointer onto tile [1] should swap the
two slots in app_state -- verified via the chan_id script command
both before and after the gesture.
"""

TEST = {
    "name": "config_drag_reorder",
    "description": "Long-press + drag tile A onto tile B swaps channel order.",
    "args": [],
    # 4-col x 6-row grid inside an overlay with pad 16. List screen
    # pos: (16, 312). list pad 6 -> list content (22, 318).
    # Tile [0]: (22, 318)-(261, 360). Tile [1]: (269, 318)-(508, 360).
    # Tile content mid-y = 339. Tile [0] name label rect ~
    # (28, 331)-(215, 347); tile [1] name rect ~ (275, 331)-(462, 347).
    "script": (
        "echo before-drag\n"
        "chan_id 0\n"
        "chan_id 1\n"
        "tap 1000 16\n"           # gear -> settings overlay
        "sleep 200\n"
        "press 100 339\n"         # press on tile 0's name
        "sleep 600\n"             # > LVGL's 400 ms long-press default
        "move 372 339\n"          # drag pointer onto tile 1
        "sleep 200\n"
        "release\n"
        "sleep 500\n"             # let async rebuild settle
        "echo after-drag\n"
        "chan_id 0\n"
        "chan_id 1\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "before-drag",
            "OK chan_id idx=0 ms_id=0",
            "OK chan_id idx=1 ms_id=1",
            "after-drag",
            "OK chan_id idx=0 ms_id=1",
            "OK chan_id idx=1 ms_id=0",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
    "hw_compatible": False,
}
