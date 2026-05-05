#include "app_time.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"

#include "app_prefs.h"

static const char *TAG = "app_time";

void app_time_apply_tz(void)
{
    char tz[APP_PREFS_STR_MAX];
    app_prefs_get_display_tz(tz, sizeof(tz));
    if (tz[0] == '\0') return;
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "TZ='%s' applied", tz);
}

// SNTP bring-up is handled in app_ui's start_clock_once on IP-up; that path
// reads the ntp_server pref. Kept here as a stub so the prototype compiles
// for callers that don't want to depend on app_ui internals.
void app_time_init(void)
{
}
