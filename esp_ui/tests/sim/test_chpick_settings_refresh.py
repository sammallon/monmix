"""Channel-picker save refreshes the settings overlay's channel grid.

Pilot bug 2026-05-09: after picking a new channel set, the fader UI
updated but the config panel (settings overlay) still showed the
stale list until reboot. The settings overlay's tile grid was built
once at first-open and never rebuilt on app_state changes.

Fix: chpick_apply_async calls settings_invalidate() which deletes
the overlay and nullifies its child widget pointers; the next
settings_open() rebuilds against the fresh app_state. If the overlay
was visible at invalidation time (chpick was reached from it), the
invalidate helper auto-reopens so the user's flow continues with
the new tiles.

Test plan:
1. Open settings; dump_tiles -> 8 tiles (mock default 0..7).
2. Run chpick_save with a different 4-id list.
3. Open settings again (gear tap); dump_tiles -> 4 tiles.
4. The post-save chan_id queries match the new id list, confirming
   that the rebuild reads from the same app_state the fader UI does.
"""

TEST = {
    "name": "chpick_settings_refresh",
    "description": "Settings overlay rebuilds tile grid after chpick save.",
    "args": [],
    "script": (
        "echo before-chpick\n"
        # Phase 1: open settings, dump tiles (default 8 channels).
        "tap 1002 16\n"           # gear -> settings overlay
        "sleep 200\n"
        "dump_tiles\n"
        "echo after-initial-dump\n"
        # Phase 2: change channel selection. chpick_save runs the
        # same chpick_apply_async path the picker's Save button
        # takes -- which now calls settings_invalidate(). Since
        # settings was visible, invalidate re-opens it with the
        # new grid; we should see 4 tiles next.
        "chpick_save 2,5,9,12\n"
        "sleep 500\n"
        "echo after-chpick-save\n"
        "dump_tiles\n"
        "echo after-postsave-dump\n"
        # chan_id confirms app_state and the settings tiles are
        # backed by the same id list. Slot 4 should now be empty
        # (count=4), proving the count actually shrank vs settings
        # tiles being orphaned at i=4..7.
        "chan_id 0\n"
        "chan_id 1\n"
        "chan_id 2\n"
        "chan_id 3\n"
        "chan_id 4\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "before-chpick",
            # Pre-save: 8 tiles (mock default).
            "settings_tile i=0",
            "settings_tile i=7",
            "settings_tile_count=8",
            "after-initial-dump",
            # Save with 4 ids; chpick_apply_async invalidates +
            # rebuilds the settings overlay.
            "OK chpick_save n=4",
            "after-chpick-save",
            # Post-save: rebuild left only 4 tiles in the overlay.
            # Without the fix the overlay would still hold 8 tiles
            # (built once at first-open and never refreshed), and
            # this count assertion would fail.
            "settings_tile_count=4",
            "after-postsave-dump",
            # New ids land at slots 0..3; slot 4 doesn't exist in
            # app_state. Same fact verified two ways for clarity.
            "OK chan_id idx=0 ms_id=2",
            "OK chan_id idx=1 ms_id=5",
            "OK chan_id idx=2 ms_id=9",
            "OK chan_id idx=3 ms_id=12",
            "OK chan_id idx=4 ms_id=-1",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
    # PC sim only -- needs the dump_tiles + chpick_save script
    # commands wired into pc_main.c.
    "hw_compatible": False,
}
