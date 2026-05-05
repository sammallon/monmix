"""Boot the sim, mount UI, quit cleanly. Catches any new breakage in
the LVGL/SDL bring-up path or app_ui_init.
"""

TEST = {
    "name": "boot_smoke",
    "description": "Sim boots, faders mount, exits clean. Mock MS path.",
    "args": [],
    "script": (
        "sleep 800\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            "UI shell mounted",
            "faders mounted",
            "OK quit",
        ],
        "stdout_not_contains": [
            "LV_ASSERT",
            "Assertion failed",
        ],
    },
    # Hardware parity: tablet boots, faders mount, ready for input.
    # The "OK quit" line is sim-only; on device the run_steps.py
    # session ends naturally when the script completes.
    "hw_compatible": True,
    "hw_expect": {
        "exit_code": 0,
        "stdout_not_contains": ["error", "ERROR", "panic", "abort"],
    },
}
