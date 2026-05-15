# Stage Monitor Mix Controller (esp_ui)

A purpose-built touchscreen "fader box" that lets a musician on stage control their own monitor mix. Talks over WiFi to a [Mixing Station](https://mixingstation.app/) instance running on a laptop/PC, which proxies to a Soundcraft Si Expression 2 mixer.

## Hardware

- **Elecrow CrowPanel Advanced 10.1"** — 1024×600 IPS capacitive touch, MIPI-DSI, GT911 touch
- **ESP32-P4** as host (UI + LVGL + app logic)
- **ESP32-C6** as WiFi co-processor via ESP-Hosted (SDIO)
- 32 MB PSRAM, 16 MB flash

## Build

```bash
# from this directory, with ESP-IDF v6.0.1 environment active
cp main/secrets.h.template main/secrets.h
# edit main/secrets.h — fill in WiFi SSID/password and MS host/port
idf.py set-target esp32p4
idf.py build flash monitor
```

The first build will download managed components from the ESP Component Registry (LVGL, panel/touch drivers, esp_websocket_client, esp_wifi_remote, esp_hosted).

### Credentials

WiFi password and other secrets live ONLY in `main/secrets.h`, which is gitignored. The committed `main/secrets.h.template` is a placeholder model — the build fails until you copy it to `secrets.h` and fill in real values. Never commit `secrets.h`. Never put real credentials in `sdkconfig.defaults` or any committed file.

A `pre-commit` git hook at `tools/git-hooks/pre-commit` enforces this: it blocks commits that include `main/secrets.h`, that mutate `main/secrets.h.template` to remove its placeholders, or that introduce credential `#define`s anywhere else. To activate the hook on a fresh clone:

```bash
git config --local core.hooksPath tools/git-hooks
```

(Already set on the original clone. Bypass on a known false-positive with `git commit --no-verify`.)

## Project layout

```
esp_ui/
├── CMakeLists.txt
├── partitions.csv               4 MB app + 64 KB coredump + 8 MB FATFS
├── sdkconfig.defaults           PSRAM, hosted WiFi, LVGL, coredump-to-flash
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml        managed-component dependencies
│   ├── secrets.h.template       template for local main/secrets.h (gitignored)
│   ├── esp_ui_main.c            entry point
│   ├── app_config.[ch]          NVS-backed per-musician channel selection (M2)
│   ├── app_state.[ch]           channel data (mutex-guarded)
│   ├── app_wifi.[ch]            split radio bring-up vs SSID-connect helpers
│   ├── app_ms_client.h          backend-agnostic MS client interface
│   ├── app_ms_client_ws.c       REST + WebSocket implementation (M1)
│   ├── app_display.[ch]         panel + touch + LVGL bring-up
│   ├── app_ui.[ch]              LVGL paged fader screen (M2)
│   ├── app_storage.[ch]         microSD mount on SDMMC slot 0 (M2.5a)
│   ├── app_coredump.[ch]        flush flash coredump → SD on boot (M2.5a)
│   ├── app_console.[ch]         UART REPL (M2.5a) — ls, cat-b64, coredump-b64
│   ├── app_logd.[ch]            rolling SD log: WS/wifi/UI events + heartbeat (M2.5c)
│   └── pytest_esp_ui.py         host-side smoke tests
└── README.md
```

## Mixing Station

Reference URLs for development. Mixing Station's HTTP/WebSocket port is configurable in its app settings — substitute whatever port you've bound it to (the firmware reads it from `APP_MS_PORT` in `main/secrets.h`):

- App base:        `http://<MS_HOST>:<MS_PORT>/`
- REST API docs:   `http://<MS_HOST>:<MS_PORT>/#/rest-api`
- Data explorer:   `http://<MS_HOST>:<MS_PORT>/#/data-explorer` — useful for discovering exact channel paths
- WebSocket URL:   `ws://<MS_HOST>:<MS_PORT>/` — single connection for subscribe + set, JSON envelopes shaped like HTTP requests

`app_ms_client_ws.c` opens one WS to the test instance and (on each connect) subscribes per-tracked-channel to `ch.<N>.levelData.0.lvl` (norm 0..1) and `ch.<N>.cfg.name`. Sets are sent as `POST /console/data/set/...` envelopes through the same socket. Protocol verified via `repro_ms_probe*.py` (gitignored).

## Channel selection (M2)

The list of MS channels the device tracks is stored in NVS under namespace `monmix`, key `chan_ids` (a blob of `int32_t[]` MS channel IDs, 0-indexed). On first boot the firmware seeds NVS with a 12-channel default (IDs 0–11). To pick a different set:

- **Quick way**: edit the `s_default_ids[]` array in `main/app_config.c`, then run `idf.py erase-flash flash` once. Subsequent boots will pick up the new defaults.
- **Reset to defaults**: `idf.py erase-flash` clears NVS; the next boot reseeds from `s_default_ids[]`.

An on-device editor lands in M4 (BLE/SoftAP provisioning).

## Postmortem logging (M2.5a)

A panic on the device writes an ELF coredump to a 64 KB `coredump` flash partition. The next clean boot (after `app_wifi_init_radio()` brings up the SDMMC controller) mounts the microSD card on SDMMC slot 0 and copies the dump to `/sdcard/coredump-NNNN.elf` before doing anything that could itself crash. The flash partition is then erased so the next crash gets a fresh slot.

If no SD card is inserted, the dump stays put in flash and is recovered on the first boot with a card present. Decoding host-side:

```bash
espcoredump.py info_corefile -c coredump-0001.elf build/esp_ui.elf
```

### UART console (M2.5a+)

The firmware runs an `esp_console` REPL on UART0 (`monmix> ` prompt). Connect with `idf.py monitor` or any serial terminal at 115200 N81. Available commands:

| Command | Source | Notes |
|---|---|---|
| `crash` | esp_hosted | Writes to `0` so the panic handler captures a dump. |
| `reboot` | esp_hosted | Software reset. |
| `mem-dump` / `task-dump` / `cpu-dump` / `heap-trace` / `sock-dump` / `host-power-save` | esp_hosted | Diagnostic. |
| `ls [path]` | this repo | Defaults to `/sdcard`. |
| `cat-b64 <path>` | this repo | Base64-prints a file between `===BEGIN BASE64 …===` / `===END BASE64===` markers, so a host script can extract it without picking up interleaved log lines. |
| `coredump-b64` | this repo | Same framing, but reads the flash `coredump` partition directly — useful when SD never mounted and the dump is still in flash. |
| `screenshot` | this repo | Captures the LVGL screen as RGB565, deflate-compresses (≈80× ratio on solid-color UIs), base64-streams it. Decoded by `tools/fetch_screenshot.py` to PNG. |
| `touch <x> <y> [tap\|down\|up]` | this repo | Drives a virtual LVGL pointer indev. Coordinates are LVGL logical pixels — the same frame as the `screenshot` PNG, so what you see is what you tap. |
| `level-format [norm\|db]` | this repo | Switch the per-fader value readout between 0–100 and dB. Persisted in `/sdcard/monmix-prefs.json`. |
| `signal-indicator [none\|signal-present\|meter]` | this repo | Toggle the per-channel green-dot indicator. Persisted in prefs. |
| `set-color <ch_id> <0..7\|-1>` | this repo | Paint a colored stripe on the channel's scribble strip from an 8-color palette. `-1` clears. Local-only — does NOT touch MS's `cfg.color`. Persisted in prefs. |
| `log-trace [on\|off]` | this repo | Query or toggle the disk-logger's TRACE gate. Persisted in NVS; survives reboots. |
| `help` | esp_console | Lists all of the above. |

### Closed dev loop

`screenshot` + `touch` give a host-driven UI verification loop without needing a finger on the panel:

```bash
python tools/fetch_screenshot.py COM3 before.png       # capture state
# eyeball the PNG, find a button at (x, y) in logical coords
echo "touch X Y tap" | <serial-terminal>                # drive input
python tools/fetch_screenshot.py COM3 after.png        # confirm state changed
```

Bare-filename outputs land in `tmp/screenshots/` (gitignored). Pass an absolute path or any path with separators to override. The same default applies to `tools/run_steps.py` (`shot:NAME` → `tmp/screenshots/NAME.png`) and to `tools/fetch_b64.py` (bare local-out → `tmp/logs/`). Coredump ELFs and `monmix-NNNN.log` files pulled with `fetch_b64.py` therefore default to `tmp/logs/`.

### Verifying the pipeline end-to-end

```bash
# 1. Trigger a panic over the console.
echo crash | <serial-terminal-of-choice> COM3

# 2. After the panic, the device reboots, the boot's
#    app_coredump_flush_to_sd() copies the dump to
#    /sdcard/coredump-NNNN.elf, and the REPL comes back up.

# 3. Pull the dump back over UART (no SD card removal needed):
python tools/fetch_b64.py COM3 /sdcard/coredump-NNNN.elf coredump.elf

# 4. Decode it against the matching ELF:
espcoredump.py info_corefile -c coredump.elf -t raw build/esp_ui.elf
```

If the SD card is missing or failed to mount, swap step 3 for
`python tools/fetch_b64.py COM3 coredump coredump.elf` — same script, but it
sends `coredump-b64` to read the flash `coredump` partition directly.

On the second boot you should see `app_coredump: saved <N>-byte coredump to /sdcard/coredump-0001.elf` in the serial log. Pull the SD card, copy the file off, and decode with `espcoredump.py info_corefile`.

## Runtime log (M2.5c)

`app_logd.c` writes one timestamped line per event to `/sdcard/monmix-NNNN.log`. Files rotate at 256 KB, the newest 128 are kept (≈32 MB cap). All emit calls are non-blocking; a background task drains a queue, fsyncs every 16 entries or 5 s. A heartbeat task adds a heap-stats line every 10 s so quiet periods still leave time markers. Captured today: WiFi connect/disconnect/got-ip, MS WS connect/disconnect/error/rx/tx, user fader changes.

TRACE-level lines (rx/tx, fader-drag updates) are gated by the `log-trace` console toggle; INFO/WARN/ERROR always write. Default is ON. Disable with `log-trace off` (persisted in NVS) if SD wear becomes a concern.

Pull a log file with `python tools/fetch_b64.py COM3 /sdcard/monmix-NNNN.log out.log`.

## Milestones

- **M1** (done): end-to-end vertical slice — 3 hard-coded faders read & write live over WiFi.
- **M2** (done): paged UI (`lv_tileview` + page indicator) and NVS-persisted channel selection.
- **M2.5a** (done): microSD bring-up + coredump persisted to `/sdcard/coredump-NNNN.elf` on boot.
- **M2.5b** (dropped 2026-05-04): "did we crash since last power-on?" indicator. Reliability is now good enough that a passive crash indicator is no longer warranted.
- **M2.5c** (done): rolling WS/wifi/UI event log to SD (`/sdcard/monmix-NNNN.log`) with a runtime trace toggle. Built after the 2026-05-01 SDIO-storm incident showed M2.5a alone misses non-panic failure modes.
- **M3** (done): per-channel mute, dB readout (with `-INF` at floor), local SD-stored channel color tags, signal-present indicator, low-light dark theme. Plus the dev-loop tooling (UART REPL, `screenshot`+`touch`) that landed alongside.
- **M3+** (done): on-device settings UX, channel selection, and reliability hardening. Settings overlay covers level format, signal indicator, theme, and channel list with color swatches. WiFi (SSID + password + scan list) and Mixing Station (host + port) credentials editable on-device — WiFi save reboots, MS save lives via `ws_reconnect`. Channel picker shows every strip on the connected console (4 cols × 20 rows = 80 channels on Si Expression, no scroll, column-major) with a 16-channel cap and a confirm-before-restart dialog. Reliability: per-cap heap heartbeat + integrity check, WDT-on-panic, `esp_log` quiesce around base64 streams, WS reconnect watchdog with wifi-reassociate to clear ESP-Hosted wedges, TCP keepalive + WS ping/pong for fast disconnect detection. Channel-array storage moved to PSRAM (`EXT_RAM_BSS_ATTR`) so internal SRAM stays headroom for FreeRTOS.
- **M4** (done 2026-05-04): 3D-printed mic-stand enclosure. Print run completed; STLs in the sibling `case/` directory at the umbrella root.
- **M5**: configurable OSC backend (selectable at runtime via NVS).
- **M6** (cancelled): BLE/SoftAP provisioning — unnecessary, on-device touch UI covers credential entry.
- **M7**: display power management. Idle-timeout blanks the panel with a 30-s warning dialog + live countdown that any touch cancels (resetting the timer). Timeout duration is picked at each wake from a fixed menu — 1 / 2 / 4 / 8 / 12 / 24 h, hard cap at 24 h, no persistent "always-on" preference. No selection within 30 s of wake = back to sleep, so accidental touch wakes don't leave the panel running. Scheduled always-on windows are configured via SD-card JSON only (no UI), defaulting to `Sun 08:15–11:45`; idle timer is paused inside any active window. When blank, backlight/MIPI-DSI/C6/SDIO drop to a power-saving state; GT911 touch IRQ wakes the device. Requires NTP-sourced wall clock for the schedule.
- **M8** (done): display preferences. Settings panel gains a backlight brightness slider (LEDC PWM, sensible floor so a mis-tap can't render the panel unreadable) and a 180° rotation toggle (for mic-stand mounting orientation flexibility — 90/270 not offered, the fader UI is landscape-only). Both persisted to NVS (with SD-mirror) and applied at boot.

See `C:\Users\samallon\.claude\plans\fluffy-singing-moth.md` for the full project plan.

## License

MIT — see [LICENSE](LICENSE).
