#include "app_time.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "app_prefs.h"

static const char *TAG = "app_time";

// IANA name -> POSIX TZ table. Curated list of zones the device is most
// likely to ship in -- not a full IANA database (~600+ zones with multi-KB
// rule strings). Adding zones is a one-line edit; the dropdown enumerates
// from this table at build time.
//
// POSIX TZ syntax: STD<offset>DST,M<month>.<week>.<day>,M<month>.<week>.<day>
// where offset is hours WEST of UTC (so PST is +8, JST is -9). DST rules use
// "Mm.w.d" with w=1..5 (5 = last) and d=0..6 (0 = Sunday).
typedef struct {
    const char *iana;
    const char *posix;
} tz_entry_t;

static const tz_entry_t s_zones[] = {
    // North America
    { "America/Los_Angeles",  "PST8PDT,M3.2.0,M11.1.0"          },
    { "America/Denver",       "MST7MDT,M3.2.0,M11.1.0"          },
    { "America/Phoenix",      "MST7"                            },  // no DST
    { "America/Chicago",      "CST6CDT,M3.2.0,M11.1.0"          },
    { "America/New_York",     "EST5EDT,M3.2.0,M11.1.0"          },
    { "America/Anchorage",    "AKST9AKDT,M3.2.0,M11.1.0"        },
    { "Pacific/Honolulu",     "HST10"                           },
    { "America/Toronto",      "EST5EDT,M3.2.0,M11.1.0"          },
    { "America/Vancouver",    "PST8PDT,M3.2.0,M11.1.0"          },
    { "America/Mexico_City",  "CST6CDT,M4.1.0,M10.5.0"          },
    { "America/Sao_Paulo",    "BRT3"                            },  // no DST since 2019
    // Europe / Africa
    { "UTC",                  "UTC0"                            },
    { "Europe/London",        "GMT0BST,M3.5.0/1,M10.5.0"        },
    { "Europe/Paris",         "CET-1CEST,M3.5.0,M10.5.0/3"      },
    { "Europe/Berlin",        "CET-1CEST,M3.5.0,M10.5.0/3"      },
    { "Europe/Athens",        "EET-2EEST,M3.5.0/3,M10.5.0/4"    },
    { "Europe/Moscow",        "MSK-3"                           },
    { "Africa/Cairo",         "EET-2"                           },
    { "Africa/Johannesburg",  "SAST-2"                          },
    // Asia / Pacific
    { "Asia/Dubai",           "GST-4"                           },
    { "Asia/Kolkata",         "IST-5:30"                        },
    { "Asia/Bangkok",         "ICT-7"                           },
    { "Asia/Shanghai",        "CST-8"                           },
    { "Asia/Singapore",       "SGT-8"                           },
    { "Asia/Seoul",           "KST-9"                           },
    { "Asia/Tokyo",           "JST-9"                           },
    { "Australia/Perth",      "AWST-8"                          },
    { "Australia/Sydney",     "AEST-10AEDT,M10.1.0,M4.1.0/3"    },
    { "Pacific/Auckland",     "NZST-12NZDT,M9.5.0,M4.1.0/3"     },
};

#define NUM_ZONES (sizeof(s_zones) / sizeof(s_zones[0]))

static const char *posix_for_iana(const char *iana)
{
    for (size_t i = 0; i < NUM_ZONES; ++i) {
        if (strcmp(iana, s_zones[i].iana) == 0) return s_zones[i].posix;
    }
    return NULL;
}

void app_time_apply_tz(void)
{
    char tz[APP_PREFS_STR_MAX];
    app_prefs_get_display_tz(tz, sizeof(tz));
    if (tz[0] == '\0') return;
    // Translate IANA -> POSIX if the saved value matches our table; otherwise
    // pass through as-is so a legacy-stored POSIX string still works.
    const char *posix = posix_for_iana(tz);
    const char *applied = posix ? posix : tz;
    setenv("TZ", applied, 1);
    tzset();
    ESP_LOGI(TAG, "TZ='%s' (%s) applied", tz, posix ? "iana" : "posix-passthrough");
}

void app_time_init(void)
{
    // SNTP bring-up is handled in app_ui's start_clock_once on IP-up; that
    // path reads the ntp_server pref. Kept here as a stub so the prototype
    // compiles for callers that don't want to depend on app_ui internals.
}

size_t app_time_zone_count(void)
{
    return NUM_ZONES;
}

const char *app_time_zone_iana(size_t idx)
{
    if (idx >= NUM_ZONES) return NULL;
    return s_zones[idx].iana;
}
