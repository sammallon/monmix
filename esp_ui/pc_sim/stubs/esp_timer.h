// Stub of esp_timer.h. Only esp_timer_get_time() is referenced by app_ui.c
// (used as a millisecond clock for rate-limiting). Backed by
// QueryPerformanceCounter on Windows (microseconds).
#pragma once

#include <stdint.h>

int64_t esp_timer_get_time(void);
