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
    # Coordinates from the 4x6 grid inside an overlay with pad 16.
    # List screen pos: (16, 312). list pad 6 -> list content (22,318).
    # Tile [3][5]: (763, 548)-(1002, 590). Tile content mid-y = 569.
    # Name label LEFT_MID rect ~ (769, 561)-(956, 577) -> tap (820, 569).
    # Save button at popup's TOP_RIGHT (110x36) -> rect ~(902,12)-(1012,48).
    "script": (
        "tap 1000 16\n"           # gear -> settings overlay
        "sleep 200\n"
        "tap 820 569\n"           # master tile name -> rename popup
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
    "hw_compatible": False,
}
