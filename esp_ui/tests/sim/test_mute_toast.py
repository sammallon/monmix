"""Regression: tapping a fader's MUTE button with MUTE EN off used to
NULL-deref inside lv_obj_remove_flag because build_toast() never
assigned the static s_toast pointer. Fixed in 29e6e1c.

Test: tap mute on ch1 (with the safety toggle defaulting off) and
make sure the sim doesn't crash and the LVGL assertion path doesn't
fire.
"""

TEST = {
    "name": "mute_toast_no_crash",
    "description": "Tap mute with MUTE EN off, expect amber toast, no crash.",
    "args": [],
    "script": (
        "sleep 800\n"
        # ch 4 mute button, roughly (298, 498) on the 1024x600 panel
        "tap 298 498\n"
        "sleep 300\n"
        # second tap exercises the timer-reuse branch in toast_show
        "tap 552 498\n"
        "sleep 300\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "OK tap 298 498",
            "OK tap 552 498",
            "OK quit",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
            "lv_obj_remove_flag",
        ],
    },
    # On hardware the symptom of the original bug is a hard panic;
    # the success criterion is just "no crash, REPL still alive after".
    "hw_compatible": True,
    "hw_expect": {
        "exit_code": 0,
        "stdout_not_contains": ["panic", "abort", "ERROR"],
    },
}
