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
│   ├── app_state.[ch]           channel data (mutex-guarded)
│   ├── app_wifi.[ch]            esp_wifi_remote + retry helper
│   ├── app_ms_client.h          backend-agnostic MS client interface
│   ├── app_ms_client_ws.c       REST + WebSocket implementation (M1)
│   ├── app_display.[ch]         panel + touch + LVGL bring-up
│   ├── app_ui.[ch]              LVGL fader screen
│   └── pytest_esp_ui.py         host-side smoke tests
└── README.md
```

## Mixing Station

Reference URLs for development. Mixing Station's HTTP/WebSocket port is configurable in its app settings — substitute whatever port you've bound it to (the firmware reads it from `APP_MS_PORT` in `main/secrets.h`):

- App base:        `http://<MS_HOST>:<MS_PORT>/`
- REST API docs:   `http://<MS_HOST>:<MS_PORT>/#/rest-api`
- Data explorer:   `http://<MS_HOST>:<MS_PORT>/#/data-explorer` ← use this to discover exact channel paths

The actual subscription/REST paths used by `app_ms_client_ws.c` are placeholders pending verification against the data-explorer (search the source for `TODO(unbox)`).

## Milestones

- **M1** (in progress): end-to-end vertical slice — 3 hard-coded faders read & write live over WiFi.
- **M2**: paged UI, persisted channel selection.
- **M3**: UX polish (mute/solo, color tags, low-light theme, meters).
- **M4**: BLE/SoftAP provisioning.
- **M5**: configurable OSC backend (selectable at runtime via NVS).
- **M6**: 3D-printed mic-stand enclosure.

See `C:\Users\samallon\.claude\plans\fluffy-singing-moth.md` for the full project plan.

## License

MIT — see [LICENSE](LICENSE).
