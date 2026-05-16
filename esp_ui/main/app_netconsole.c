// Networked admin console -- see app_netconsole.h for protocol.

#include "app_netconsole.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "secrets.h"

#include "app_logd.h"
#include "app_ota.h"
#include "app_wifi.h"

static const char *TAG = "netcon";

#ifndef APP_NET_TOKEN
// Build-time guard. secrets.h.template ships with APP_NET_TOKEN
// pre-defined; a personal build without the token has no business
// exposing a network REPL.
#error "APP_NET_TOKEN must be defined in secrets.h"
#endif

#define APP_NETCONSOLE_PORT     4242
#define NETCON_TASK_STACK       8192
#define NETCON_TASK_PRIO        4
#define LINE_BUF_MAX            256
// Send-side: bound how long we'll wait on the mutex before dropping a
// log line on the floor. Short -- we'd rather lose a log than block.
#define SEND_BLOCK_MS           500
// Receive-side: we don't impose a recv timeout. TCP keepalive
// (SO_KEEPALIVE + TCP_KEEPIDLE) is what detects truly dead connections;
// imposing a recv timeout here would force-disconnect idle clients
// (e.g. a host that's connected over VPN and not typing) which makes
// the netconsole feel "twitchy" for no reason.
#define KEEPALIVE_IDLE_S        30
#define KEEPALIVE_INTERVAL_S    10
#define KEEPALIVE_COUNT         3

static int                 s_listen_fd = -1;
static int                 s_client_fd = -1;
static bool                s_authed    = false;
static bool                s_log_tail  = false;
static SemaphoreHandle_t   s_send_mtx  = NULL;

// ─────────────────────────────────────────────────────────────────────────
// Send path
// ─────────────────────────────────────────────────────────────────────────

// Non-blocking, drop-on-full send. The vprintf hook + log subscriber
// can call this from any task; the send mutex serialises socket writes
// so two log lines can't interleave bytes on the wire.
static void send_raw(const char *buf, size_t len)
{
    if (s_client_fd < 0 || !s_send_mtx) return;
    if (xSemaphoreTake(s_send_mtx, pdMS_TO_TICKS(SEND_BLOCK_MS)) != pdTRUE) return;
    int fd = s_client_fd;
    if (fd >= 0) {
        // MSG_DONTWAIT keeps us from blocking when the stage WiFi link
        // backs up; lost log lines are acceptable for a debug surface.
        (void) send(fd, buf, len, MSG_DONTWAIT);
    }
    xSemaphoreGive(s_send_mtx);
}

void app_netconsole_send_line(const char *fmt, ...)
{
    if (!s_authed || s_client_fd < 0) return;
    char buf[LINE_BUF_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int) sizeof(buf) - 1) n = (int) sizeof(buf) - 2;
    if (buf[n - 1] != '\n') {
        buf[n++] = '\n';
    }
    send_raw(buf, (size_t) n);
}

bool app_netconsole_is_active(void)
{
    return s_authed && s_client_fd >= 0;
}

// ─────────────────────────────────────────────────────────────────────────
// Log streaming integration
// ─────────────────────────────────────────────────────────────────────────

// app_logd subscriber: forwards APP_LOGD_* lines (which already have a
// "[ts] [tag] L message" prefix + newline).
static void on_logd_line(const char *line, size_t len, void *ctx)
{
    (void) ctx;
    if (!s_log_tail) return;
    send_raw(line, len);
}

// Intercept ESP_LOGx output via the vprintf hook. The previous hook
// (UART writer) is preserved + still called so UART logs keep working.
static vprintf_like_t s_prev_vprintf;
static int net_vprintf_hook(const char *fmt, va_list ap)
{
    // The IDF log format already includes its own newline + ANSI color
    // codes. Format into a stack buffer first so we can mirror without
    // double-formatting. va_copy is mandatory -- the second vsnprintf
    // would otherwise see an exhausted argument list.
    int n_uart = 0;
    if (s_prev_vprintf) {
        va_list ap_uart;
        va_copy(ap_uart, ap);
        n_uart = s_prev_vprintf(fmt, ap_uart);
        va_end(ap_uart);
    }
    if (s_log_tail && s_client_fd >= 0) {
        char buf[LINE_BUF_MAX];
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        if (n > 0) {
            if (n >= (int) sizeof(buf)) n = (int) sizeof(buf) - 1;
            send_raw(buf, (size_t) n);
        }
    }
    return n_uart;
}

// ─────────────────────────────────────────────────────────────────────────
// Client lifecycle
// ─────────────────────────────────────────────────────────────────────────

static void send_str(const char *s) { send_raw(s, strlen(s)); }

static void close_client(void)
{
    s_log_tail = false;
    s_authed   = false;
    int fd = s_client_fd;
    s_client_fd = -1;
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Command dispatch
// ─────────────────────────────────────────────────────────────────────────

// Returns true if dispatch handled the command (caller continues).
// Returns false to close the connection (quit, auth-failure, etc).
static bool handle_command(char *line)
{
    // Strip leading whitespace
    while (*line == ' ' || *line == '\t') ++line;
    if (*line == '\0') return true;

    // Tokenise: first space splits cmd from args
    char *args = strchr(line, ' ');
    if (args) {
        *args = '\0';
        ++args;
        while (*args == ' ') ++args;
    } else {
        args = (char *) "";
    }
    const char *cmd = line;

    if (!s_authed) {
        if (strcmp(cmd, "auth") == 0) {
            if (strcmp(args, APP_NET_TOKEN) == 0) {
                s_authed = true;
                send_str("OK authed\n");
                ESP_LOGI(TAG, "client authed");
                return true;
            }
            send_str("ERR auth failed\n");
            ESP_LOGW(TAG, "auth failed");
            return false;
        }
        send_str("ERR auth required\n");
        return true;
    }

    if (strcmp(cmd, "ping") == 0) {
        send_str("pong\n");
        return true;
    }
    if (strcmp(cmd, "log-tail") == 0) {
        s_log_tail = true;
        send_str("OK log-tail\n");
        return true;
    }
    if (strcmp(cmd, "log-stop") == 0) {
        s_log_tail = false;
        send_str("OK log-stop\n");
        return true;
    }
    if (strcmp(cmd, "ota") == 0) {
        if (*args == '\0') {
            send_str("ERR ota: url required\n");
            return true;
        }
        // app_ota_start returns true if the OTA kicked off; the actual
        // progress + result are sent via app_netconsole_send_line from
        // within the OTA task.
        if (app_ota_start(args)) {
            send_str("OK ota\n");
        } else {
            send_str("ERR ota: already running or start failed\n");
        }
        return true;
    }
    if (strcmp(cmd, "reboot") == 0) {
        send_str("OK reboot\n");
        // Give the client time to receive + the kernel time to drain
        // before reboot; 200 ms is generous.
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return false;
    }
    if (strcmp(cmd, "quit") == 0) {
        send_str("OK bye\n");
        return false;
    }
    if (strcmp(cmd, "help") == 0) {
        send_str("commands: ping, log-tail, log-stop, ota <url>, reboot, quit\n");
        return true;
    }
    send_str("ERR unknown command\n");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Listener task
// ─────────────────────────────────────────────────────────────────────────

static void serve_client(int fd)
{
    s_client_fd = fd;
    s_authed    = false;
    s_log_tail  = false;
    send_str("monmix net-console v1\nauth: required\n");

    char    line[LINE_BUF_MAX];
    size_t  used = 0;
    while (1) {
        char *room = line + used;
        size_t cap = sizeof(line) - 1 - used;
        if (cap == 0) {
            // Oversized line; reset (silent drop is fine since
            // anything legitimate fits in 256B for now).
            used = 0;
            continue;
        }
        ssize_t r = recv(fd, room, cap, 0);
        if (r <= 0) break;
        used += (size_t) r;
        line[used] = '\0';

        // Drain complete lines.
        while (1) {
            char *nl = memchr(line, '\n', used);
            if (!nl) break;
            *nl = '\0';
            // Trim trailing CR if CRLF.
            if (nl > line && *(nl - 1) == '\r') *(nl - 1) = '\0';
            bool keep = handle_command(line);
            // Shift remainder.
            size_t consumed = (size_t) (nl - line) + 1;
            memmove(line, line + consumed, used - consumed);
            used -= consumed;
            line[used] = '\0';
            if (!keep) {
                close_client();
                return;
            }
        }
    }
    close_client();
}

static void netcon_task(void *arg)
{
    (void) arg;
    // Wait for WiFi. STA association is the only thing we need; we bind
    // to INADDR_ANY so we don't care which subnet the host lands in.
    while (app_wifi_get_state() != APP_WIFI_STATE_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "socket: %d", errno);
        vTaskDelete(NULL);
    }
    int yes = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(APP_NETCONSOLE_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind: %d", errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        vTaskDelete(NULL);
    }
    if (listen(s_listen_fd, 1) < 0) {
        ESP_LOGE(TAG, "listen: %d", errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "listening on :%d", APP_NETCONSOLE_PORT);

    while (1) {
        struct sockaddr_in c_addr;
        socklen_t clen = sizeof(c_addr);
        int cfd = accept(s_listen_fd, (struct sockaddr *) &c_addr, &clen);
        if (cfd < 0) {
            ESP_LOGW(TAG, "accept: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        // If a prior client is still attached, drop it first. Single-
        // client semantics: latest connector wins.
        if (s_client_fd >= 0) {
            close_client();
        }
        // No recv timeout: TCP keepalive does the real "is this client
        // still there" detection. SO_RCVTIMEO would force-disconnect
        // any idle client, which is exactly the wrong behavior for a
        // human-driven REPL (especially over VPN where 100-500 ms of
        // idle is normal between commands).
        int yes = 1;
        setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
        int idle_s   = KEEPALIVE_IDLE_S;
        int intvl_s  = KEEPALIVE_INTERVAL_S;
        int count    = KEEPALIVE_COUNT;
        setsockopt(cfd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle_s,  sizeof(idle_s));
        setsockopt(cfd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl_s, sizeof(intvl_s));
        setsockopt(cfd, IPPROTO_TCP, TCP_KEEPCNT,   &count,   sizeof(count));
        int nodelay = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &c_addr.sin_addr, ipbuf, sizeof(ipbuf));
        ESP_LOGI(TAG, "client connected from %s:%u",
                 ipbuf, (unsigned) ntohs(c_addr.sin_port));
        serve_client(cfd);
        ESP_LOGI(TAG, "client disconnected");
    }
}

// ─────────────────────────────────────────────────────────────────────────
// OTA validation timer
// ─────────────────────────────────────────────────────────────────────────

// One-shot timer fired after a settling window. If we're still running
// here, the firmware survived the boot + WiFi associate + early
// application code -- enough confidence to mark the running image
// valid so the bootloader stops queuing a rollback to the previous slot.
#define VALIDATE_DELAY_MS  (60u * 1000u)
static esp_timer_handle_t s_validate_timer;
static void validate_timer_cb(void *arg)
{
    (void) arg;
    app_ota_mark_running_valid();
}

// ─────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────

void app_netconsole_init(void)
{
    if (s_send_mtx) return;     // idempotent
    s_send_mtx = xSemaphoreCreateMutex();
    if (!s_send_mtx) {
        ESP_LOGE(TAG, "mutex alloc failed; netcon disabled");
        return;
    }
    // Mirror APP_LOGD_* lines.
    app_logd_subscribe(on_logd_line, NULL);
    // Mirror IDF ESP_LOGx output too. esp_log_set_vprintf returns the
    // prior hook so we can chain to it (UART writes still happen).
    s_prev_vprintf = esp_log_set_vprintf(net_vprintf_hook);

    xTaskCreatePinnedToCore(netcon_task, "netcon",
                            NETCON_TASK_STACK, NULL, NETCON_TASK_PRIO,
                            NULL, tskNO_AFFINITY);

    // Schedule the rollback-cancel after settling window.
    const esp_timer_create_args_t targs = {
        .callback        = validate_timer_cb,
        .name            = "ota_validate",
    };
    if (esp_timer_create(&targs, &s_validate_timer) == ESP_OK) {
        esp_timer_start_once(s_validate_timer, VALIDATE_DELAY_MS * 1000ULL);
    }
}
