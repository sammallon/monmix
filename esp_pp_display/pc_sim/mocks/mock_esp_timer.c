// esp_timer_get_time -> microseconds since the first call. QPC has
// sub-microsecond resolution on modern Windows, plenty for the uses in
// app_ui.c (millisecond-grain rate-limit timestamps).
#include "esp_timer.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static LARGE_INTEGER s_freq;
static LARGE_INTEGER s_origin;
static int           s_inited;

int64_t esp_timer_get_time(void) {
    if (!s_inited) {
        QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&s_origin);
        s_inited = 1;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return ((now.QuadPart - s_origin.QuadPart) * 1000000LL) / s_freq.QuadPart;
}
#else
#include <time.h>
int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}
#endif
