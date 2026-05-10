"""Drag-to-reorder uses INSERT semantics, not pairwise swap.

Pilot bug 2026-05-09: dragging a channel across multiple tiles
swapped the dragged channel with each tile in turn, mangling the
non-dragged channels' relative order. With the fix, the dragged
channel slots in at the target position; intermediate channels
shift by 1 in the opposite direction; non-traversed channels keep
their slots.

Default 8 channels with ids 0..7 in a 4-col x 6-row column-major
grid. Tile [0]..[5] occupy column 0 rows 0..5. Drag tile [0] all
the way down to tile [4]:
- swap semantics (BUG): each adjacent swap leaves the drag at [4]
  but the path is messy. The naive far-target swap would put [0]
  at [4] and [4] at [0] (i.e. order [4,1,2,3,0,5,6,7]).
- insert semantics (FIX): dragged item ends at [4]; items previously
  at [1..4] shift up to [0..3]; items at [5..7] unchanged. Final
  order: [1,2,3,4,0,5,6,7].

The hit-testing in on_tile_pressing only fires the FIRST match per
PRESSING event, so a fast finger crossing 4 tiles in one motion
event hits tile [4] directly (skipping [1..3]). Verifies the
multi-step shift logic in on_tile_pressing handles the |i-cur|>1
case correctly.
"""

TEST = {
    "name": "config_drag_reorder_insert",
    "description": "Drag from row 0 to row 4 inserts (shifts intermediate tiles by 1).",
    "args": [],
    # Tile geometry (from the existing config_drag_reorder test):
    # tile_w=239, tile_h=42, col_gap=8, row_gap=4, list at (16, 312)
    # with pad 6 -> first tile content at (22, 318). Column 0 rows
    # 0..5 mid-y values: 339, 385, 431, 477, 523, 569.
    # name label x range with swatch-on-left: 64..251.
    "script": (
        "echo before-drag\n"
        "chan_id 0\n"
        "chan_id 1\n"
        "chan_id 2\n"
        "chan_id 3\n"
        "chan_id 4\n"
        "chan_id 5\n"
        "tap 1002 16\n"           # gear -> settings overlay
        "sleep 200\n"
        "press 100 339\n"         # press tile [0]'s name (col 0, row 0)
        "sleep 600\n"             # > LVGL's 400 ms long-press default
        # One large move directly to row 4 mid-y. The pressing event
        # hits tile [4] in a single shot, so the multi-step shift
        # path inside on_tile_pressing has to handle |i-cur|=4.
        "move 100 523\n"          # drag onto tile [4] (col 0, row 4)
        "sleep 200\n"
        "release\n"
        "sleep 500\n"
        "echo after-drag\n"
        "chan_id 0\n"
        "chan_id 1\n"
        "chan_id 2\n"
        "chan_id 3\n"
        "chan_id 4\n"
        "chan_id 5\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "before-drag",
            "OK chan_id idx=0 ms_id=0",
            "OK chan_id idx=1 ms_id=1",
            "OK chan_id idx=2 ms_id=2",
            "OK chan_id idx=3 ms_id=3",
            "OK chan_id idx=4 ms_id=4",
            "OK chan_id idx=5 ms_id=5",
            "after-drag",
            # INSERT semantics: dragged ms_id 0 moved from slot 0 to
            # slot 4; ms_ids 1..4 each shifted up by one slot; ms_id
            # 5 (and 6, 7) keep their original slots.
            "OK chan_id idx=0 ms_id=1",
            "OK chan_id idx=1 ms_id=2",
            "OK chan_id idx=2 ms_id=3",
            "OK chan_id idx=3 ms_id=4",
            "OK chan_id idx=4 ms_id=0",
            "OK chan_id idx=5 ms_id=5",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
    # HW parity: same gesture path on hardware. Don't assume the
    # device's selected channel set has any particular ms_ids; just
    # capture pre/post and verify no panic.
    "hw_compatible": True,
    "hw_script": (
        "sleep 3500\n"
        "chan_id 0\n"
        "chan_id 1\n"
        "chan_id 2\n"
        "chan_id 3\n"
        "chan_id 4\n"
        "tap 1002 16\n"
        "sleep 1500\n"
        "press 100 339\n"
        "sleep 600\n"
        "move 100 523\n"
        "sleep 200\n"
        "release\n"
        "sleep 800\n"
        "chan_id 0\n"
        "chan_id 1\n"
        "chan_id 2\n"
        "chan_id 3\n"
        "chan_id 4\n"
    ),
    "hw_expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK chan_id idx=0",
            "OK chan_id idx=4",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "panic",
            "abort",
        ],
    },
}
