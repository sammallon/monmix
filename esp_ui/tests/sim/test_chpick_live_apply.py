"""Channel-picker live apply: Save replaces channels without rebooting.

The picker's Save path used to esp_restart(). Now it stops the MS
worker, reseeds app_state from the new id list, rebuilds the fader UI
in place, and restarts the worker. Two consecutive saves in one phase
exercise the start-after-stop task race fixed in ms_client_ws::ws_stop;
a single save couldn't catch a join that was missed, since the join's
absence would only bite when the second start runs while the first
stop's task is still tearing down.

The test drives the apply via a script command (`chpick_save`) that
calls app_ui_chpick_apply directly. Driving the picker UI through taps
in headless mode would mean cross-referencing pixel coords to the
checkbox grid; the apply path is what we want to exercise, and it's
identical whether reached from the popup's Save button or from the
test hook.

Mock MS: validates the lifecycle (start/stop, present_channels,
no crash, no esp_restart). With a live MS (MONMIX_MS_HOST set), the
real WS client cycles through stop/start with a fresh subscribe sweep
on each start -- the WS-open log line tags both connect cycles.
"""

import os

MS_HOST = os.environ.get("MONMIX_MS_HOST")
MS_PORT = os.environ.get("MONMIX_MS_PORT", "8080")

LIVE_ARGS = (
    ["--ms-host", MS_HOST, "--ms-port", MS_PORT] if MS_HOST else []
)

# When MS is reachable, demand empirical evidence the post-save
# subscribe targets the NEW id list. Without MS the mock client doesn't
# emit ms_real: lines; the chpick: log lines alone prove the lifecycle.
EXTRA_CONTAINS = (
    [
        # WS lifecycle visible across saves.
        "ms_real: WS open",
        "ms_real: WS closed",
        # After the first save, broadcasts arrive for ch.9 -- which
        # wasn't in the default {0..7} set. Empirical proof that the
        # post-save subscribe sweep targets the NEW id list.
        "ms_real: lvl ch=9",
    ]
    if MS_HOST else []
)

# Default seed has 8 ids {0..7}. Swap to 4 different ids, then back
# to 8 -- different counts on each transition so the chpick log lines
# carry distinct (current=N now=M) pairs.
NEW_IDS  = "2,5,9,12"
BACK_IDS = "0,1,2,3,4,5,6,7"

TEST = {
    "name": "chpick_live_apply",
    "description": "Channel picker Save rebuilds UI live, no reboot.",
    "args": LIVE_ARGS,
    "timeout_s": 60,
    # Single phase: in-memory app_config in the sim doesn't persist
    # between process invocations, so we exercise the start-after-stop
    # race by doing two saves back-to-back in one process.
    "script": (
        # Boot + initial fader render.
        "sleep 3000\n"
        "echo PRE_SAVE_1\n"
        # First save: drops the default 8 ids, swaps to 4. Stops worker
        # (joins via 1500 ms bound), reseeds app_state, present_channels
        # rebuilds tileview, restarts worker.
        f"chpick_save {NEW_IDS}\n"
        # Settle: long enough for the WS reconnect to land on a live
        # MS, well past the join timeout when one is unavailable.
        "sleep 3000\n"
        "echo POST_SAVE_1\n"
        # Second save back to 8: this is the cycle that catches a
        # missed join. The first stop's worker has had time to die,
        # but the second stop's worker is still freeing mgr when this
        # start runs -- without the join, the new task races the old.
        f"chpick_save {BACK_IDS}\n"
        "sleep 3000\n"
        "echo POST_SAVE_2\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Boot baseline -- 8 ids from the mock app_config default.
            "chpick: live-apply start (current=8)",
            "chpick: live-apply done (now=4)",
            "OK chpick_save n=4",
            # Second save: 4 -> 8. If start-after-stop was racing,
            # this either crashes or app_state would be inconsistent.
            "chpick: live-apply start (current=4)",
            "chpick: live-apply done (now=8)",
            "OK chpick_save n=8",
            # Post-save markers reached without abort.
            "OK echo POST_SAVE_1",
            "OK echo POST_SAVE_2",
        ] + EXTRA_CONTAINS,
        # Critical: the live-apply path doesn't reboot the device, the
        # rebuild doesn't trigger an LVGL invariant, and nothing panics.
        "stdout_not_contains": [
            "Rebooting...",
            "LV_ASSERT",
            "panic",
            "abort",
        ],
    },
    # Hardware variant: same shape but driven through the REPL would
    # need a chpick-save console command on the device which doesn't
    # exist. Sim coverage is sufficient -- the firmware path is the
    # same code (chpick_apply_async).
    "hw_compatible": False,
}
