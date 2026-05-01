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
├── partitions.csv               4 MB app + 8 MB FATFS
├── sdkconfig.defaults           PSRAM, hosted WiFi, LVGL flags
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml        managed-component dependencies
│   ├── secrets.h.template       template for local main/secrets.h (gitignored)
│   ├── esp_ui_main.c            entry point
│   ├── app_config.[ch]          NVS-backed per-musician channel selection (M2)
│   ├── app_state.[ch]           channel data (mutex-guarded)
│   ├── app_wifi.[ch]            esp_wifi_remote + retry helper
│   ├── app_ms_client.h          backend-agnostic MS client interface
│   ├── app_ms_client_ws.c       REST + WebSocket implementation (M1)
│   ├── app_display.[ch]         panel + touch + LVGL bring-up
│   ├── app_ui.[ch]              LVGL paged fader screen (M2)
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

## Milestones

- **M1** (done): end-to-end vertical slice — 3 hard-coded faders read & write live over WiFi.
- **M2** (in progress): paged UI (`lv_tileview` + page indicator) and NVS-persisted channel selection.
- **M2.5**: postmortem logging to SD card (FATFS + coredump + WS/REST frame log) so off-bench failures are recoverable.
- **M3**: UX polish (mute/solo, color tags, low-light theme, meters).
- **M4**: BLE/SoftAP provisioning.
- **M5**: configurable OSC backend (selectable at runtime via NVS).
- **M6**: 3D-printed mic-stand enclosure.

See `C:\Users\samallon\.claude\plans\fluffy-singing-moth.md` for the full project plan.

## License

MIT — see [LICENSE](LICENSE).
