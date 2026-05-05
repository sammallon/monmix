#pragma once

// Apply the persisted POSIX TZ string to the C library so localtime_r
// reflects the user's chosen display zone. Safe to call before WiFi.
// Logs format from monotonic uptime (esp_log_timestamp) so they remain
// TZ-independent regardless of this setting.
void app_time_apply_tz(void);

// Bring up SNTP using the persisted ntp_server pref. Caller must have a
// network route -- call after app_wifi_wait_connected. Idempotent: a
// second call updates the server and re-arms.
void app_time_init(void);
