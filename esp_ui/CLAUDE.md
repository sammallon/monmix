# monmix — project-level Claude notes

Stage-monitor mix controller firmware. ESP32-P4 (host) + ESP32-C6
(WiFi co-processor over SDIO via ESP-Hosted), CrowPanel Advanced 10.1"
(EK79007 MIPI-DSI panel + GT911 touch). Talks WebSocket to a Mixing
Station instance proxying a Soundcraft Si Expression 2 mixer. Personal
project (not work). All code is C; no C++.

## Build / flash / monitor

EIM-managed IDF v6.0.1 lives at `C:\esp\v6.0.1\esp-idf` with a non-standard
Python venv at `C:\Espressif\tools\python\v6.0.1\venv`. **Always invoke
through PowerShell, not git bash** — `eim run` aborts with "MSys/Mingw is
no longer supported" before doing anything from a bash subshell.

Use the `PowerShell` tool with the profile-source preamble:

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1' *> $null
$env:PYTHONIOENCODING='utf-8'
idf.py build           # or flash, monitor, set-target, etc.
idf.py -p COM3 flash
```

`PYTHONIOENCODING=utf-8` is mandatory — idf.py emits Unicode (≥, ANSI
colors) that breaks default Windows CP1252.

EIM's "currently selected" version can flip when other projects switch
it. If a build dies on a `riscv32-esp-elf-gcc.exe: '-march=...zaamo...'`
error that's the v5.5.4 toolchain choking on v6.0.1 flags — re-source
the profile.

The serial port is **COM3** (CH340K USB-UART bridge) for both flash and
console. Baud is **921600** for the REPL (boosted from default 115200,
otherwise b64-streaming a coredump takes 140s).

## Hardware quirks worth remembering

- **C6 reset GPIO is 32, not the IDF default 54.** Set
  `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=32` in `sdkconfig.defaults`
  (already there). With this, post-flash USB-cycle dance is no longer
  required. CrowPanel-specific; verified against the schematic XML.
- **The P4 SDMMC controller is a singleton shared with ESP-Hosted.**
  Init order is forced: `app_wifi_init_radio()` → (esp_wifi_init runs
  sdmmc_host_init internally) → `app_storage_init()` mounts SD on slot 0
  with dummy host.init/deinit. App can't reorder this without breaking
  one or the other. See `app_wifi.c` and `app_storage.c`.
- **REPL bootstrap.** We start `esp_console` ourselves first thing in
  app_main. ESP-Hosted's diagnostic commands (`crash`, `reboot`,
  `mem-dump`) get registered later inside `esp_wifi_init` — they
  register against the same global esp_console registry and appear
  retroactively.

## Architecture / locking

- **LVGL is a single-threaded task pinned to CPU 0** (`task_affinity = 0`
  in `lvgl_port_init`). Anything from a non-LVGL task that touches
  widgets, the timer list, or `lv_async_call` MUST hold `lvgl_port_lock`.
  WS / wifi event tasks are non-LVGL — they get the lock before
  `lv_async_call`, then release. See `app_ui_set_status`,
  `on_state_change`, `on_wifi_state_change`, `on_ms_state_change`.
- **State changes coalesce via dirty flags + a single async sweep.**
  Every WS broadcast sets a per-channel dirty bit and queues *one*
  `apply_pending` if not already queued. The sweep reads fresh state
  at apply time so the last value wins. Avoids the per-message
  malloc + queued-async storm during fader drag.
- **lv_async_call from non-LVGL tasks.** Without holding lvgl_port_lock
  the wifi/WS events would race with LVGL's task on the timer list, and
  occasional UI updates would silently drop. The "final user-released
  fader value" was the visible symptom of this bug.
- **Outbound SETs are rate-limited to ~20 Hz per channel.** Every SET
  produces a server-snap echo on the same WS, doubling on-wire traffic.
  Drag at full screen-touch frequency without rate-limiting will
  monopolize the WS task on CPU 1. Final value is always sent on
  release so the rate-limiter can't swallow the last position.

## Mixing Station protocol shape

- One WS to `ws://<host>:<port>/`. JSON envelopes: `{method, path,
  body}`. Subscribe + set both ride this single connection.
- Subscribe to `ch.<n>.levelData.<m>.lvl` (norm 0..1), `.level` (dB),
  `.on` (bool, `true` = audible / NOT muted — flip for our user-facing
  bool). Plus `ch.<n>.cfg.name` for scribble-strip names.
- Server-snap echo: every SET produces a broadcast back with the
  quantized value MS landed on. Subscribe handler is the source of
  truth, not the SET path.
- `MIX 1` in the MS UI = MIX index 0 in the wire protocol. Currently
  one mix bus is hardcoded.

## Preferences boundary

- **NVS is primary** for local UI prefs (level format, signal indicator
  mode, channel color tags, theme). Each key has a parallel `_mt` u64
  mtime (uptime-ms basis, monotonic across reboots via a stored floor).
  See `app_prefs.c`.
- **`/sdcard/monmix-prefs.json`** is an NVS-mirror backup — written
  after every NVS commit. Lets the user copy prefs off the card or
  edit offline. Per-key `_mt` fields in the JSON carry the mtime so
  conflict resolution at boot picks the newer side. **Not for
  credentials.** Atomic write via tmp + rename (FATFS doesn't do
  POSIX rename-overwrite, so unlink-first).
- **Boot reconcile rule** (see `merge_bags` in `app_prefs.c`): newer
  mtime wins per key; missing on both -> install default and persist
  to both. Migration: a legacy SD-only JSON without `_mt` fields parses
  with mtime=0 and seeds NVS on the first boot of the new model.
- **NVS** also holds the channel selection (ids that show up as
  faders) -- owned by `app_config.c`, separate code path from the
  prefs above.
- **`main/secrets.h`** (gitignored, copy of `secrets.h.template`) holds
  `APP_WIFI_SSID`, `APP_WIFI_PASSWORD`, `APP_MS_HOST`, `APP_MS_PORT`.
  Migrating these to runtime-editable storage is queued (#38) — the
  password specifically must NOT land in plaintext on SD; use NVS
  (encrypted if NVS-enc is on) and default the WiFi panel field to
  obscured with hold-to-reveal.

## Postmortem / dev-loop tooling

- **Coredump → flash partition → SD on next boot.**
  `app_coredump_flush_to_sd()` runs early in app_main. If SD didn't
  mount, the dump stays in flash and `coredump-b64` streams it over
  UART. Decode with `espcoredump.py info_corefile`.
- **Runtime ring log** at `/sdcard/monmix-NNNN.log` (rotating). Trace
  level toggleable at runtime via `log-trace on|off` console command.
  Built after the 2026-05-01 SDIO-storm incident showed coredumps
  alone miss non-panic failure modes.
- **Closed-loop UI dev-loop.** `tools/fetch_screenshot.py` and
  `tools/run_steps.py` round-trip RGB565+deflate+base64 frames over
  UART. Synthetic touch via `touch x y tap` console command. Default
  output dirs are `tmp/screenshots/` and `tmp/logs/` — bare filenames
  go there; absolute paths or paths with separators pass through.
- **Console `screenshot` command** renders into a PSRAM-allocated
  draw buffer (default LVGL allocator only hands out DMA-internal RAM,
  too small for full-frame). Compresses with the ROM miniz via a
  pre-allocated `tdefl_compressor` (ROM miniz is built with
  `MINIZ_NO_MALLOC`, so the canned `tdefl_compress_mem_to_mem` path
  doesn't work).

## Coding conventions

- Tone of code comments mirrors the user's voice: terse, focused on
  *why* (a constraint, an incident, a workaround). No what-it-does
  narration. No multi-paragraph docstrings.
- ASCII only in source-string literals — LVGL's default Montserrat
  doesn't carry em-dashes or ∞. Past commits learned this twice.
- Don't add planning files, decision docs, or summaries unless the
  user asks. Work from conversation context.
- One commit per logical change. Test on hardware before committing
  when behavior is observable. Push only when asked.

## Current milestones

See README.md for the full list. As of 2026-05-04: M3+ done, M4 done
(3D-printed enclosure complete). M3+ covers on-device editing of
WiFi/MS credentials, channel picker across the full strip count of
the connected console (cap 16 on the fader UI), and the reliability
hardening pass (heap heartbeat, WS reconnect watchdog with
wifi-reassoc, TCP keepalive, WS ping/pong, error-event
instrumentation). M5 = OSC backend; M6 cancelled (touch UI covers provisioning);
M7 = display power mgmt; M8 done (brightness + rotation prefs,
landed in the 2026-05-04 UI polish batch — see plan
`during-and-after-yesterday-s-adaptive-fog.md`).

UI polish batch in flight (plan `during-and-after-yesterday-s-adaptive-fog.md`):
pilot-recovered items P1–P11 + open issues #30, #35, #36, #43, #51 +
M8 brightness/rotation. Includes a foundational persistence rework
(NVS-primary, SD as backup with mtime-based conflict resolution) —
this reverses the prior "SD-only prefs" direction. M2.5b
crash-since-poweron indicator dropped (reliability sufficient).
#28 display flicker stays deferred (bench-only).

SDIO storm investigation (the "spontaneous WS disconnect" workstream)
is **resolved 2026-05-03** — root cause was a CMD53-byte-mode wedge in
the C6 SLC HOST peripheral; workaround landed via CMD52 fallback for
TOKEN_RDATA polling. No further action needed. See memory
`project_outstanding_wifi_investigation` for the full forensic record.
