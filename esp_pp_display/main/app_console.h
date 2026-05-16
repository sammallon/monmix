#pragma once

// Bring up an esp_console REPL on UART0 ("monpp> " prompt) and register
// our own commands (`ls`, `cat-b64`, `coredump-b64`, `set-bright`, …).
//
// Call as early as possible in app_main — the REPL only depends on UART0
// (live since boot) and esp_partition (SPI flash, separate peripheral from
// SDMMC), so it's safe to start before WiFi/SD/display bring-up. Doing so
// gives us a recovery channel for panics that occur during any of those
// later stages: the next boot's console comes up in ~700 ms and
// `coredump-b64` streams the flash partition out over UART.
//
// ESP-Hosted's diagnostic commands (`crash`, `reboot`, `mem-dump`, …) get
// registered later, during esp_wifi_init() in app_wifi_init_radio(). They
// land in the same global esp_console registry our REPL is walking, so
// they appear retroactively without a restart.
//
// The REPL runs on its own task; this function returns immediately.
void app_console_init(void);
