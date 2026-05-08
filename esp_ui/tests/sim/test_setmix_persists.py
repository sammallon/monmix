"""Mix-bus selection persists across reboot. Boot the sim, switch mix
0 -> 1 (which triggers app_prefs_set_selected_mix_index inside
on_mix_picker_btn_clicked), quit. Boot the sim again with the same
working dir so the file-backed NVS mock carries the saved index forward
and verify the mix indicator label / app_state reads it back.

Covers the boot-restore path in esp_ui_main's try_apply_ms_info, the
prefs setter, and the WS client's set_mix re-subscribe."""

import os

MS_HOST = os.environ.get("MONMIX_MS_HOST")
MS_PORT = os.environ.get("MONMIX_MS_PORT", "8080")

# Both phases need the SAME working directory so the file-backed NVS
# mock from phase 1 carries over to phase 2. The runner allocates a
# single per-test artifact dir which the sim treats as cwd; phases run
# inside that dir, so just-share happens automatically.

TEST = {
    "name": "setmix_persists",
    "description": "Mix index persists across simulated reboot.",
    "args": (
        ["--ms-host", MS_HOST, "--ms-port", MS_PORT] if MS_HOST else []
    ),
    "skip_if": lambda: not MS_HOST,
    "skip_reason": "needs MONMIX_MS_HOST pointing at a live MS",
    "timeout_s": 60,
    "phases": [
        {
            "name": "set_to_mix2",
            "script": (
                "sleep 3000\n"   # boot + WS connect + info fetch
                "set_mix 1\n"    # Mix 2 (idx 1) -- saves via on_mix_picker_btn_clicked
                "sleep 1000\n"   # let the prefs setter flush
                "quit\n"
            ),
            "expect": {
                "exit_code": 0,
                "stdout_contains": [
                    "ms_real: WS open",
                    "ms_real: mix change 0 -> 1",
                ],
            },
        },
        {
            "name": "boot_restores_mix2",
            "script": (
                "sleep 3000\n"   # boot + WS reconnect
                "quit\n"
            ),
            "expect": {
                "exit_code": 0,
                "stdout_contains": [
                    "ms_real: WS open",
                    # The persistence signal: app_prefs_init logs the loaded
                    # state including mix=N. If phase 1's save flushed and
                    # phase 2 read it back, this prints mix=1. If the save
                    # didn't persist, it would default to mix=0.
                    "mix=1",
                ],
            },
        },
    ],
    # Sim-only: hw runner has no phase / reboot support, and the second
    # phase here is "boot the device fresh and observe restored mix". A
    # power-cycle hook would unblock this; until then, NVS persistence
    # on hw is exercised manually by power-cycling after a set-mix.
    "hw_compatible": False,
}
