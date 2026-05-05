// Stub of esp_system.h. esp_restart() in the sim just exits cleanly —
// the user can re-launch the binary if they want to test boot-path code.
#pragma once

#include <stdint.h>

void esp_restart(void);

uint32_t esp_random(void);
