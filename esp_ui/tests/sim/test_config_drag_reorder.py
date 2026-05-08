"""Drag-to-rearrange in the settings overlay's channel grid.

Default 8 channels with ids 0..7; under the column-major fill that
landed in fix-settings-grid, tile [0] is at (col=0, row=0) and
tile [1] is directly below at (col=0, row=1). Long-pressing tile
[0]'s name then dragging down onto tile [1] should swap the two
app_state slots -- verified via the chan_id script command before
and after.
"""

TEST = {
    "name": "config_drag_reorder",
    "description": "Long-press + drag tile A onto tile B swaps channel order.",
    "args": [],
    # 4-col x 6-row grid (column-major) inside overlay pad 16. List screen
    # pos: (16, 312); list pad 6 -> list content (22, 318).
    # tile_w=239, tile_h=42, col_gap=8, row_gap=4.
    # Tile [0] (col=0, row=0): screen (22, 318)-(261, 360); inner mid-y 339.
    # Tile [1] (col=0, row=1): screen (22, 364)-(261, 406); inner mid-y 385.
    # With swatch-on-left, name label is at LEFT_MID with offset
    # (swatch_sz+8, 0) = (36, 0); name x_left = inner_x + 36 = 64.
    # Press at x=100 (inside name rect 64..251) hits tile [0]'s name.
    # Move to (100, 385) puts the pointer over tile [1]'s bounds.
    "script": (
        "echo before-drag\n"
        "chan_id 0\n"
        "chan_id 1\n"
        "tap 1002 16\n"           # gear -> settings overlay
        "sleep 200\n"
        "press 100 339\n"         # press on tile [0]'s name (col 0, row 0)
        "sleep 600\n"             # > LVGL's 400 ms long-press default
        "move 100 385\n"          # drag down onto tile [1] (col 0, row 1)
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
