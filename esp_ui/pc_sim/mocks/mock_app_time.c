// Minimal IANA list so the Settings overlay's TZ dropdown populates.
// app_time_apply_tz is a no-op — the sim shows its host clock as-is.
#include "app_time.h"

static const char *s_zones[] = {
    "America/Los_Angeles",
    "America/New_York",
    "Europe/London",
    "Europe/Berlin",
    "Asia/Tokyo",
    "Australia/Sydney",
};

void   app_time_apply_tz(void) {}
void   app_time_init(void)     {}
size_t app_time_zone_count(void) { return sizeof(s_zones) / sizeof(s_zones[0]); }

const char *app_time_zone_iana(size_t idx) {
    if (idx >= app_time_zone_count()) return "";
    return s_zones[idx];
}
