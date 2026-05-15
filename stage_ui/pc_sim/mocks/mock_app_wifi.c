// Mock app_wifi for the sim. Reports CONNECTED with a synthetic SSID
// (uses the host's hostname so the WiFi panel shows recognizable values
// when running locally). scan_results returns a small canned list so
// the Scan button has something to show.
#include "app_wifi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
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

static const char *s_canned_scan[] = {
    "Stage-WiFi", "Booth-2.4G", "GuestNet", "Crew-5G", "WorshipBand",
};

static void snapshot_host(void) {
    if (s_inited) return;
    s_inited = true;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
#endif
    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname) - 1) != 0 || !hostname[0]) return;
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

void app_wifi_init_radio(void)        {}
bool app_wifi_wait_connected(void)    { return true; }
bool app_wifi_reconfigure(void) {
    fprintf(stdout, "[mock_wifi] reconfigure\n");
    return true;
}

app_wifi_state_t app_wifi_get_state(void) { return APP_WIFI_STATE_CONNECTED; }

const char *app_wifi_get_ssid(void)        { snapshot_host(); return s_ssid; }
void        app_wifi_format_ip(char *buf, size_t buflen) {
    snapshot_host();
    if (buf && buflen > 0) snprintf(buf, buflen, "%s", s_ipv4);
}
const char *app_wifi_get_security_str(void) { return "WPA2-PSK"; }

void app_wifi_register_on_change(app_wifi_on_change_t cb, void *ctx) {
    if (s_obs_n < MAX_OBS) { s_obs[s_obs_n].cb = cb; s_obs[s_obs_n].ctx = ctx; ++s_obs_n; }
}

app_wifi_scan_result_t app_wifi_scan_start(app_wifi_scan_done_t done_cb, void *ctx) {
    (void)done_cb; (void)ctx;
    return APP_WIFI_SCAN_STARTED;
}

size_t app_wifi_scan_results(char (*dst)[33], size_t max_count) {
    size_t n_canned = sizeof(s_canned_scan) / sizeof(s_canned_scan[0]);
    size_t n = (n_canned < max_count) ? n_canned : max_count;
    for (size_t i = 0; i < n; ++i) {
        snprintf(dst[i], 33, "%s", s_canned_scan[i]);
    }
    return n;
}
