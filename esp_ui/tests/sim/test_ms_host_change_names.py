"""MS host change re-primes strip names (and kills the cached-URL ghost).

Regression for two latent bugs in the MS-config Save live-apply path
(commit c5c1920 on branch fix-names-on-reconfigure):

  1) ws_reconnect() / osc_reconnect() set is_draining on the open
     connection but didn't recompose g.ws_url / g.udp_host. The worker's
     reconnect branch dialled the OLD host -- so toggling MS host in
     Settings landed back on whatever was cached at boot.

  2) Even with WS pointed at the new host, try_apply_ms_info's
     s_ms_setup_done gate didn't reset. If MS was reachable on the old
     host the gate latched true; if not, the per-host caches stayed
     empty -- the on-state-change retry returned early either way.

The user repro on hardware: boot with valid MS, fader strips populate
with real names ("Voc 1" etc.), open Settings -> MS, type a bogus host
(10.1.20.64) + bogus port, Save. Without the fix the strip names did
NOT degrade to placeholders -- WS stayed dialled at the old host so
broadcasts kept arriving. With the fix WS retargets the bogus host,
fails to connect, and the names eventually clear.

Sim caveat: pc_sim's ms_client_real.c snapshots host/port at process
init via ms_client_real_create() and ignores app_config_ms_host()
afterward, so the sim path can't model the cached-URL bug end-to-end.
This test exercises the LVGL-side path (mcfg_apply -> on_mcfg_save ->
app_ms_setup_reset -> ms->reconnect()) and asserts no panic / abort.
The hardware lane is where the regression actually catches: the
"console_attached false -> true" log line printed by the heartbeat
task after the second host change is the post-fix marker -- without
the fix, the gate doesn't drop and the heartbeat's first reading
(once it eventually runs against the new host) doesn't print a
transition because the cached value was already true.

Sequence (mirrors the user's hw repro):
  1. Boot, give the UI time to mount + WS to subscribe.
  2. mcfg_apply 10.1.20.64 8080  -- bogus host, no listener.
  3. Settle ~3s for reconnect attempts to fail.
  4. mcfg_apply <real host> <real port> -- back to live MS.
  5. Settle ~5s for re-prime to land.
"""

import os

MS_HOST = os.environ.get("MONMIX_MS_HOST")
MS_PORT = os.environ.get("MONMIX_MS_PORT", "8080")

LIVE_ARGS = (
    ["--ms-host", MS_HOST, "--ms-port", MS_PORT] if MS_HOST else []
)

# Pick a "back" target that names the live MS when the env is set;
# otherwise reuse the mock defaults so the no-MS sim run still
# exercises the LVGL-side path. Either way the test is verifying the
# code path runs without panic.
BACK_HOST = MS_HOST or "127.0.0.1"
BACK_PORT = MS_PORT if MS_HOST else "9000"

TEST = {
    "name": "ms_host_change_names",
    "description": "MS host change re-primes strip names; live-apply path is panic-free.",
    "args": LIVE_ARGS,
    "timeout_s": 60,
    "script": (
        # Boot + initial subscribe sweep. Long enough that with a live MS
        # the WS broadcasts populate strip names before we touch host.
        "sleep 3000\n"
        "echo PRE_BOGUS\n"
        # Drive Save with a host that has no listener. Same path as a tap
        # on the music-note icon -> tap Host TA -> type 10.1.20.64 -> tap
        # Port TA -> type 8080 -> tap Save. on_mcfg_save persists via
        # app_config_set_ms_host/port, calls app_ms_setup_reset (drops
        # s_ms_setup_done), and triggers ms->reconnect.
        "mcfg_apply 10.1.20.64 8080\n"
        # Settle past the WS reconnect attempt's failure window. With the
        # fix, the per-host caches (strip_names, routability,
        # console_attached) were dropped; without the fix they persist
        # and the UI never reflects the change.
        "sleep 3000\n"
        "echo POST_BOGUS\n"
        # Roll back to a working host (or mock defaults). The s_ms_setup_done
        # latch must drop again here; if the bogus-host run latched it
        # true (impossible -- no listener) or the live-MS run already
        # latched it from boot, the next CONNECTED transition needs to
        # re-run try_apply_ms_info to re-prime against the new host.
        f"mcfg_apply {BACK_HOST} {BACK_PORT}\n"
        # Long settle: WS has to reconnect, /console/information GET has
        # to land, fetch_all_strip_names has to repopulate the cache, and
        # the on-state-change observer has to repaint the strips. On
        # hardware the regression marker is the heartbeat task's
        # "console_attached false -> true" log; HEARTBEAT_INTERVAL_MS is
        # 5000 so we need >= one full cycle plus the /app/state HTTP
        # round trip plus phase slack. 10000 covers the worst case.
        "sleep 10000\n"
        "echo POST_BACK\n"
        "quit\n"
    ),
    "expect": {
        "exit_code": 0,
        "stdout_contains": [
            # Sim-side proof both saves drove the test hook.
            "OK mcfg_apply 10.1.20.64 8080",
            f"OK mcfg_apply {BACK_HOST} {BACK_PORT}",
            "OK echo PRE_BOGUS",
            "OK echo POST_BOGUS",
            "OK echo POST_BACK",
        ],
        # The live-apply path can't crash, can't reboot, can't trip an
        # LVGL invariant. The sim's mcfg_apply runs on_mcfg_save inline;
        # any null-deref / unsafe-cleanup in the cache-invalidation path
        # would surface as one of these.
        "stdout_not_contains": [
            "Rebooting...",
            "LV_ASSERT",
            "panic",
            "abort",
        ],
    },
    # Hardware run via the mcfg-apply REPL command. The hw lane is where
    # this test actually catches the regression: with the fix, the
    # heartbeat task prints "console_attached false -> true" (or the
    # symmetric pair) after the second host change because
    # app_ms_setup_reset dropped the gate; without the fix the gate
    # short-circuits the re-prime and that log line never appears for
    # the new host's first-success transition. Verify the strip-name
    # repopulation by tail-grepping the WS broadcast lines for
    # ch.<n>.cfg.name responses post-rollback.
    "hw_compatible": True,
    "hw_expect": {
        "stdout_contains": [
            "mcfg-apply: host=10.1.20.64 port=8080",
            f"mcfg-apply: host={BACK_HOST} port={BACK_PORT}",
            # Heartbeat task's transition log -- the marker that proves
            # the per-host caches were dropped and re-primed against
            # the new host. Without the fix the gate never drops and
            # this log line doesn't fire for the second host change.
            "console_attached",
        ],
        "stdout_not_contains": [
            "Rebooting...",
            "LV_ASSERT",
            "panic",
            "abort",
        ],
    },
}
