#pragma once

#include <stddef.h>

// Apply the persisted display-timezone pref to the C library so localtime_r
// reflects the user's chosen display zone. Pref stores an IANA name (e.g.
// "America/Los_Angeles"); we look up the matching POSIX TZ string in the
// embedded table and call setenv("TZ", posix, 1) + tzset(). Safe to call
// before WiFi. Logs format from monotonic uptime (esp_log_timestamp) so they
// remain TZ-independent regardless of this setting.
//
// Backward compat: if the stored pref doesn't match any IANA name in the
// table (e.g. a legacy POSIX TZ string), it's used directly. setenv accepts
// either form.
void app_time_apply_tz(void);

// Bring up SNTP using the persisted ntp_server pref. Caller must have a
// network route -- call after app_wifi_wait_connected. Idempotent: a
// second call updates the server and re-arms.
void app_time_init(void);

// IANA timezone enumeration for the settings overlay dropdown. Names are
// stable strings -- safe to keep pointers across calls. count() is the
// number of entries; iana(i) returns the IANA name for index i.
size_t app_time_zone_count(void);
const char *app_time_zone_iana(size_t idx);
