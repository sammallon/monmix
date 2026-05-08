#include "esp_system.h"

#include <stdio.h>
#include <stdlib.h>

#include "app_ms_client.h"

void esp_restart(void) {
    fprintf(stdout, "[mock_system] esp_restart() called -> exit(0)\n");
    fflush(stdout);
    exit(0);
}

uint32_t esp_random(void) { return (uint32_t)rand(); }

// app_reboot_graceful is defined in main/app_console.c which the sim
// build doesn't pull in. Provide a mock that mirrors the real shape:
// inform MS via shutdown_graceful, then reboot. The test framework
// keys off the mock_system esp_restart line for "did the restart
// actually fire" so we don't need to log anything else.
void app_reboot_graceful(void) {
    const ms_client_iface_t *ms = app_ms_client_ws();
    if (ms && ms->shutdown_graceful) ms->shutdown_graceful();
    esp_restart();
}
