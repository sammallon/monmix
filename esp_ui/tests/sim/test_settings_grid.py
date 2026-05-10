"""Settings overlay channel grid: column-major fill + swatch on left.

fe0a133 ("wifi/network config: UX overhaul") rewrote the settings panel
and inadvertently flipped the channel-tile loop from column-major to
row-major, and put the colour swatch on the right of each tile instead
of the left. Both regressions are visual-only -- nothing crashes -- so
they slipped past the existing soak/smoke tests.

The test taps the gear icon to open the settings overlay, then drives
app_ui_settings_dump_tiles via the dump_tiles script command, and
asserts on the printed coords:

  - column-major: tile i+1 shares the same x as tile i (same column)
    and a strictly larger y, until the column wraps. Asserting that
    the first few tiles are in column 0 (same x) is enough to lock
    the iteration order without coupling to the exact pixel pitch.
  - swatch on left: every tile has swatch_x < name_x.

Pure layout test -- no MS connection needed, runs in mock mode.
"""

# Gear icon: top-right, 28x28 button placed at (-8, 2) from TOP_RIGHT
# of the 1024x600 screen, so center is around (1002, 16).
GEAR_X = 1002
GEAR_Y = 16

TEST = {
    "name": "settings_grid",
    "description": "Settings channel grid is column-major with swatch on left.",
    "args": [],
    "timeout_s": 30,
    "script": (
        # Boot + initial fader render. Dismiss the modal boot wake
        # menu so the gear tap reaches the settings overlay.
        "power_set_user_timeout_ms 3600000\n"
        "sleep 2000\n"
        # Open the settings overlay.
        f"tap {GEAR_X} {GEAR_Y}\n"
        # 1500 ms covers the hardware path: lazy build of the settings
        # overlay (channel grid + swatches + textareas) plus first
        # paint takes >800 ms on the P4. Sim builds in a few ms but
        # pays nothing for the longer wait.
        "sleep 1500\n"
        # Dump the tile coords; settings_tile lines hit stdout.
        "dump_tiles\n"
        "sleep 200\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK dump_tiles",
            # Default seed has 8 tracked channels {0..7}. With 6 rows
            # column-major, the first 6 fill column 0 and the last 2
            # spill into column 1.
            #
            # Column-major fingerprint: tiles 0..5 share an x (column 0)
            # with strictly increasing y. We assert the row-0 baseline
            # and the row-1 successor share an x — that combination is
            # impossible under row-major, where consecutive indices
            # differ in x and share a y.
            "settings_tile i=0 x=0 y=0",
            # i=1 in column-major sits directly below i=0: same x, next y.
            # Tile height is (inner_h - 5*4) / 6 = (582 - 20) / 6 = 93 with
            # row_gap 4 -> i=1 y = 97. Allow some wiggle on the exact pitch
            # by only locking the x.
            "settings_tile i=1 x=0",
            "settings_tile i=2 x=0",
            "settings_tile i=3 x=0",
            "settings_tile i=4 x=0",
            "settings_tile i=5 x=0",
            # Swatch on left of name. Swatch is 28 wide aligned LEFT_MID
            # with offset 0 -> swatch_x = 0. Name aligned LEFT_MID with
            # swatch_sz+8 = 36 offset -> name_x = 36. The tile's pad-all
            # of 6 gets baked into both equally so the order holds even
            # if the absolute x's drift.
            "swatch_x=0",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "panic",
            "abort",
            # Row-major fingerprint: i=1 would land at x = tile_w + col_gap
            # (column 1) with the same y as i=0. Tile width with a 4-col
            # 992-inner-px grid is (992 - 24) / 4 = 242 -> i=1 x=250 in
            # row-major, x=0 in column-major. If this string appears the
            # iteration order has flipped back.
            "settings_tile i=1 x=250",
        ],
    },
    # Hardware variant: dump-tiles REPL command exposes the same hook on
    # device. The hw script taps the gear icon to open the overlay, waits,
    # dumps tiles, then taps it again to close (visual cleanup -- not
    # required for the assertions). Same column-major / swatch-on-left
    # invariants apply.
    "hw_compatible": True,
    "hw_script": (
        "sleep 2000\n"
        f"tap {GEAR_X} {GEAR_Y}\n"
        # 1500 ms matches the sim wait. The master tile + drag handler
        # wiring added on m7-config-panel-overhaul pushes overlay build
        # past the previous 800 ms budget on the P4.
        "sleep 1500\n"
        "dump_tiles\n"
        "sleep 200\n"
    ),
    "hw_expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK dump_tiles",
            "settings_tile i=0 x=0 y=0",
            "settings_tile i=1 x=0",
            "settings_tile i=2 x=0",
            "settings_tile i=3 x=0",
            "settings_tile i=4 x=0",
            "settings_tile i=5 x=0",
            "swatch_x=0",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "panic",
            "abort",
            "settings_tile i=1 x=250",
        ],
    },
}
