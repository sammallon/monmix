# esp_pp_display

ProPresenter stage-display firmware for the CrowPanel Advanced 10.1"
(ESP32-P4 host + ESP32-C6 WiFi co-processor over SDIO via ESP-Hosted).

Sibling project to `esp_ui/` (monmix mixer fader controller); shares
hardware bring-up modules and the monmix umbrella repo.

## Build

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1' *> $null
$env:PYTHONIOENCODING='utf-8'
cd S:\playground\monmix\esp_pp_display
# First time: copy the template, fill in your WiFi + ProPresenter host
Copy-Item main\secrets.h.template main\secrets.h
notepad main\secrets.h
idf.py build
idf.py -p COM3 flash
```

## ProPresenter API

Two transports, same data:
- **TCP socket on port 63306** — primary; newline-JSON envelopes,
  persistent connection, `chunked:true` for subscriptions.
- HTTP REST on port 1025 — Swagger UI for discovery at
  `http://<host>:1025/v1/doc/index.html`.

The firmware uses the TCP transport and subscribes to:
- `status/slide` — current + next slide text/notes/uuid
- `timers/current` — all timer values + run state
- `stage/message` — engineer-pushed message to musicians

Notably **not** subscribed to `timer/system_time` — that's a 1 Hz tick
forever which would defeat the activity-based sleep heuristic. Clock
is computed locally from SNTP-synced system time.

## Phase status

- A — skeleton + hardware bring-up (current)
- B — ProPresenter TCP client + REPL probes
- C — stage display LVGL UI (current/next slide, timer, stage message)
- D — settings overlay (WiFi/PP host/brightness/rotation)
- E — PC simulator + tests (mirrors esp_ui's pc_sim discipline)
- F — power management (1 h activity-based sleep) + polish + ship

See plan.md (session-local) for details.
