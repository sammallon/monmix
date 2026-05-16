// Mock app_ota for pc_sim. Network OTA isn't supported in the
// simulator -- this stub keeps app_power.c (which gates sleep on OTA
// progress) link-clean. Always reports "no OTA in flight."
#include "app_ota.h"

#include <stdio.h>

bool app_ota_start(const char *url)
{
    (void) url;
    return false;
}

void app_ota_mark_running_valid(void) { /* no-op in sim */ }

bool app_ota_in_progress(void)
{
    return false;
}
