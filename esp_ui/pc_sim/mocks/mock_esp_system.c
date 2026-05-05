#include "esp_system.h"

#include <stdio.h>
#include <stdlib.h>

void esp_restart(void) {
    fprintf(stdout, "[mock_system] esp_restart() called -> exit(0)\n");
    fflush(stdout);
    exit(0);
}

uint32_t esp_random(void) { return (uint32_t)rand(); }
