// Stub of esp_netif_sntp.h. SNTP is disabled in the sim — clock_gettime
// is enough. The macros mirror IDF's shape so app_ui.c's apply_sntp_config
// compiles unchanged; the implementations in mock_esp_netif_sntp.c are
// no-ops.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_sntp.h"

typedef struct {
    int            num_servers;
    const char    *servers[3];
    void         (*sync_cb)(struct timeval *tv);
    bool           start;
    bool           smooth_sync;
    bool           server_from_dhcp;
    bool           renew_servers_after_new_IP;
    int            ip_event_to_renew;
    int            index_of_first_server;
} esp_sntp_config_t;

// Stub forms — the SNTP wrappers are no-op'd at runtime (see
// mock_esp_netif_sntp.c), so we don't need to faithfully extract the
// server list from the macro argument. Just give the compiler a valid
// struct literal so app_ui.c's apply_sntp_config TU parses cleanly.
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server_) \
    (esp_sntp_config_t){ .num_servers = 1, .start = true }

#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(n_, list_) \
    (esp_sntp_config_t){ .num_servers = (n_), .start = true }

// IDF helper that wraps a list of host strings into an array literal.
// Real expansion: (const char *[]){ __VA_ARGS__ }. The stub just makes
// the call site parse — the returned value is discarded by our
// stub-form CONFIG_MULTIPLE above.
#define ESP_SNTP_SERVER_LIST(...) ((void)0)

// One IP event id app_ui.c references when wiring DHCP-NTP renewal.
// Real value comes from esp_event.h; sim doesn't drive event loops so
// any constant works.
#ifndef IP_EVENT_STA_GOT_IP
#define IP_EVENT_STA_GOT_IP 0
#endif

int esp_netif_sntp_init(const esp_sntp_config_t *cfg);
int esp_netif_sntp_deinit(void);
int esp_netif_sntp_start(void);
