#pragma once

// Bring up an esp_console REPL on UART0 ("monmix> " prompt) and register our
// own commands (`ls`, `cat-b64`). The esp_hosted CLI registers its commands
// (`crash`, `reboot`, `mem-dump`, …) at transport_delayed_init time but never
// starts a REPL when CONFIG_ESP_HOSTED_CLI_NEW_INSTANCE is unset — so we
// piggy-back: those commands are already in esp_console's global registry
// and become reachable as soon as we start the REPL here.
//
// Call after WiFi/SD/display bring-up but before app_main returns. The REPL
// runs on its own task; this function returns immediately.
void app_console_init(void);
