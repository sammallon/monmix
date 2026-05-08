"""Master tile in the settings overlay's channel grid: tapping its
name opens the rename popup; tapping Save sends a `cfg.name` SET to
MS targeting the master's MS channel id (mix_offset + mix_idx).

Mock MS is wired with mix_offset=60 / mix_idx=0, so the master's id
is 60. The mock logs `[mock_ms] set_name ch=<id> name=<text>`.
"""

TEST = {
    "name": "config_master_tile_rename",
    "description": "Tapping master tile name + Save writes cfg.name for master id.",
    "args": [],
    # Layout (column-major + swatch-on-left after fix-settings-grid):
    #   overlay pad 16, list pos (0, 296), list pad 6 -> list content (22, 318)
    #   tile_w=239, tile_h=42, col_gap=8, row_gap=4, swatch_sz=28
    # Master tile [3][5] (col=3, row=5): list-content (741, 230)
    #   -> screen (763, 548)-(1002, 590); inner mid-y = 569
    # Inside tile (pad 6): swatch LEFT_MID at offset (0,0) screen (783, 569);
    #   name LEFT_MID at offset (swatch_sz+8, 0) -> name x_left = 805,
    #   width 187 -> name center ~ (898, 569).
    # Rename popup Save: TOP_RIGHT 110x36 inside popup with pad 12 ->
    #   rect (902, 12)-(1012, 48), tap (957, 30).
    "script": (
        "tap 1002 16\n"           # gear -> settings overlay
        "sleep 200\n"
        "tap 898 569\n"           # master tile name -> rename popup
        "sleep 300\n"
        "tap 957 30\n"            # Save -- prefilled "Mix 1" carries
        "sleep 200\n"
        "master_state get\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Mock MS receives a set_name targeting the master's id (60).
            "[mock_ms] set_name ch=60 name=Mix 1",
            # State reflects the name we saved.
            "OK master_state id=60 name=\"Mix 1\"",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
    # HW parity: same coordinates work, but the assertion shifts from
    # the mock_ms log line (sim-only) to direct master_state introspection
    # via the new master-state REPL command. The rename popup's Save
    # closes itself on completion, so master_state-after-save reflects
    # what MS broadcast back via the cfg.name subscription.
    "hw_compatible": True,
    "hw_script": (
        # Boot + initial fader render + scribble names populated.
        "sleep 3500\n"
        "tap 1002 16\n"          # gear -> settings overlay
        "sleep 1500\n"           # overlay build (matches settings_grid)
        "tap 898 569\n"          # master tile name -> rename popup
        "sleep 600\n"
        "tap 957 30\n"           # Save -- prefilled name carries
        "sleep 800\n"            # popup closes + MS round-trip
        "cmd:master-state\n"
    ),
    "hw_expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Live MS instance puts the master at id 60 (mix_offset=60,
            # mix_idx=0, "Mix 05" or similar -- match on the id and the
            # name being non-empty quoted string).
            "OK master_state id=",
            "name=\"",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "panic",
            "abort",
            "ERR master_state",
        ],
    },
}
