// stage_ui — ProPresenter stage display.
//
// Sibling project to esp_ui (the monmix fader controller). Same CrowPanel
// Advanced 10.1" hardware, different application: show the current and
// next presentation slide for a performer on stage, with buttons (and a
// tappable next-slide preview) to advance / go back.
//
// SKELETON ROUND — sim-first.
// This commit gives you:
//   - The shared module skeleton in main/ (compiles natively + on hardware)
//   - A SDL2 + LVGL native build under pc_sim/
//   - A mock PP client that rotates through canned slide payloads so the
//     UI animates without a live ProPresenter
// Hardware build (ESP-IDF scaffolding, platform module ports from esp_ui,
// real HTTP/1.1 chunked-stream client) is a later round.
//
// Investigation findings, including endpoint catalogue, wire-format
// notes, and the full reuse-vs-rewrite map, live in INVESTIGATION.md
// inside this branch's worktree.
//
//
// BUILD (Windows, MSVC):
//   cd stage_ui\pc_sim
//   .\build.ps1
//   .\build-windows\stage_ui_sim.exe
//
// BUILD (Linux / WSL):
//   cd stage_ui/pc_sim
//   bash build.sh
//   ./build-linux/stage_ui_sim
//
//
// LICENSE: MIT, same as monmix.
//
// SECURITY: this directory contains no credentials. The Wi-Fi/PP-host
// runtime configuration story (analogous to esp_ui/main/secrets.h.template)
// is part of the later hardware round.
