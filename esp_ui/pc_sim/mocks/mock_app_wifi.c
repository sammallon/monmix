// Mock app_wifi.
//
// Mirror the host machine's hostname + first non-loopback IPv4 so the
// WiFi info panel shows real values rather than placeholders. Per user
// note: useful when running MS locally even though wifi state doesn't
// strictly matter for that flow.
//
// Plus: fire the state-change observer once after registration so
// on_wifi_state_change runs, which kicks start_clock_once on the LVGL
// side. Without that the status label sits at "Booting..." forever
// because clock_tick never gets created.
#include "app_wifi.h"

#include "lvgl.h"

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#define MAX_OBS 4
static struct { app_wifi_on_change_t cb; void *ctx; } s_obs[MAX_OBS];
static size_t s_obs_n;

static char s_ssid[64] = "sim-host";
static char s_ipv4[16] = "127.0.0.1";
static bool s_inited;

static void snapshot_host(void) {
    if (s_inited) return;
    s_inited = true;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
#endif

    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname) - 1) != 0 || !hostname[0]) {
        return;
    }
    snprintf(s_ssid, sizeof(s_ssid), "%s", hostname);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(hostname, NULL, &hints, &res) == 0) {
        for (struct addrinfo *p = res; p; p = p->ai_next) {
            if (p->ai_family != AF_INET) continue;
            const struct sockaddr_in *sa = (const struct sockaddr_in *)p->ai_addr;
            const unsigned char *b = (const unsigned char *)&sa->sin_addr;
            if (b[0] == 127) continue;
            snprintf(s_ipv4, sizeof(s_ipv4), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            break;
        }
        freeaddrinfo(res);
    }
}

void app_wifi_init_radio(void) {}
bool app_wifi_wait_connected(void) { return true; }
void app_wifi_apply_ip_config(void) {}
bool app_wifi_reconfigure(void) { return true; }

app_wifi_state_t app_wifi_get_state(void) { return APP_WIFI_STATE_CONNECTED; }
const char      *app_wifi_get_ssid(void)  { snapshot_host(); return s_ssid; }
void              app_wifi_format_ip(char *buf, size_t buflen) {
    snapshot_host();
    if (buf && buflen > 0) snprintf(buf, buflen, "%s", s_ipv4);
}
const char       *app_wifi_get_security_str(void) { return "WPA2-PSK"; }

// Periodic-ish ticker fed by app_ui's existing wifi_state_color polling.
// We can't fire from register_on_change directly -- lv_async_call from
// inside app_ui_init drops the queued cb on the floor under some
// configurations, which crashes silently. Instead, the FIRST call to
// app_wifi_get_state from a real LVGL callback (i.e. clock_tick or
// app_ui's own status-bar refresh) lazily fires the observer once.
static bool s_observer_fired;

void app_wifi_register_on_change(app_wifi_on_change_t cb, void *ctx) {
    if (s_obs_n < MAX_OBS) { s_obs[s_obs_n].cb = cb; s_obs[s_obs_n].ctx = ctx; s_obs_n++; }
}

// Manually invoked from pc_main after the UI is up. Fires the
// state-change observer chain so on_wifi_state_change runs and
// start_clock_once registers the clock timer. Idempotent.
void mock_app_wifi_fire_initial_change(void) {
    if (s_observer_fired) return;
    s_observer_fired = true;
    for (size_t i = 0; i < s_obs_n; ++i) s_obs[i].cb(s_obs[i].ctx);
}

app_wifi_scan_result_t app_wifi_scan_start(app_wifi_scan_done_t done_cb, void *ctx) {
    (void)done_cb; (void)ctx;
    return APP_WIFI_SCAN_FAILED;
}

size_t app_wifi_scan_results(char (*dst)[33], size_t max_count) {
    (void)dst; (void)max_count;
    return 0;
}
