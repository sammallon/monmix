// Stub of esp_sntp.h. The only symbol app_ui.c references is the
// notification callback registration; everything else flows through
// esp_netif_sntp.h.
#pragma once

#ifdef _WIN32
// Provide struct timeval. winsock2.h owns it on MSVC; pull it in via the
// CRT header that doesn't drag the whole socket API.
#include <sys/types.h>
#include <sys/timeb.h>
struct timeval {
    long tv_sec;
    long tv_usec;
};
#else
#include <sys/time.h>
#endif

typedef void (*sntp_sync_time_cb_t)(struct timeval *tv);
void sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t cb);
