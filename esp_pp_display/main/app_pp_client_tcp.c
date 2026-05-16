#include "app_pp_client.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "app_pp_state.h"

static const char *TAG = "pp_tcp";

// One persistent socket. The subscribe stream AND any outbound one-shot
// envelopes (sets, triggers) share it -- verified empirically with
// repro_pp_extended.py probe 6: PP fan-routes responses by url so both
// streams interleave cleanly on the same TCP connection.
//
// Sized for status/slide messages with full-stanza text. Hymn-class
// lyrics are usually <500 bytes per slide; timers/current with 8 entries
// is ~700 bytes; we leave generous headroom.
#define LINE_BUF_SIZE     8192

// Outbound headroom -- stage messages can carry user-entered text and we
// don't want a tight cap surfacing as a silent send failure. 1 KB clears
// any plausible APP_PP_STAGE_MSG_MAX growth.
#define OUTBOUND_BUF_SIZE 1024

// Pre-built subscribe envelope. NOTE: deliberately omits
// `timer/system_time` -- that emits a 1 Hz tick forever which would
// defeat the activity-based screen sleep. Clock is rendered locally
// from SNTP-synced system time.
static const char SUBSCRIBE_ENV[] =
    "{\"url\":\"v1/status/updates\",\"method\":\"POST\","
    "\"body\":[\"status/slide\",\"timers/current\",\"stage/message\"],"
    "\"chunked\":true}\r\n";

#define MAX_OBSERVERS 4

typedef struct {
    app_pp_on_change_t cb;
    void              *ctx;
} observer_t;

static SemaphoreHandle_t  s_sock_mu;
static int                s_sock = -1;
static _Atomic int        s_state = APP_PP_CONN_DISCONNECTED;
static TaskHandle_t       s_task;
static observer_t         s_obs[MAX_OBSERVERS];
static size_t             s_obs_count;
static SemaphoreHandle_t  s_obs_mu;

// Force-reconnect flag. Set by resubscribe(); the read loop sees it on
// the next recv timeout (~1s) and exits to trigger a reconnect cycle.
static _Atomic bool s_force_reconnect;

// Line accumulator. Static (not stack) -- 8 KB on the task stack would
// blow the budget. Owned by tcp_task; no other task touches it.
static char   s_line_buf[LINE_BUF_SIZE];
static size_t s_line_buf_len;
// Resync state. When a single line overflows LINE_BUF_SIZE we don't have
// the bytes to parse it anyway -- swallow the rest of that line so we
// resume parsing at the next newline rather than feeding mid-message
// fragments into cJSON.
static bool   s_discard_until_newline;

static void set_state(app_pp_conn_state_t s);

// --- Observer dispatch -----------------------------------------------------

static void notify_state_change(void)
{
    observer_t snap[MAX_OBSERVERS];
    size_t     n = 0;
    xSemaphoreTake(s_obs_mu, portMAX_DELAY);
    n = s_obs_count;
    memcpy(snap, s_obs, n * sizeof(observer_t));
    xSemaphoreGive(s_obs_mu);
    for (size_t i = 0; i < n; ++i) {
        if (snap[i].cb) snap[i].cb(snap[i].ctx);
    }
}

static void set_state(app_pp_conn_state_t s)
{
    int prev = atomic_exchange(&s_state, (int) s);
    if (prev != (int) s) {
        notify_state_change();
    }
}

// --- JSON helpers ----------------------------------------------------------

static void copy_str_field(char *dst, size_t dst_size, cJSON *src)
{
    if (!dst || dst_size == 0) return;
    if (cJSON_IsString(src) && src->valuestring) {
        strncpy(dst, src->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

// --- Inbound dispatch ------------------------------------------------------

static void handle_status_slide(cJSON *data)
{
    cJSON *current = cJSON_GetObjectItem(data, "current");
    cJSON *next    = cJSON_GetObjectItem(data, "next");

    app_pp_slide_t cs = {0};
    app_pp_slide_t ns = {0};

    if (cJSON_IsObject(current)) {
        cs.valid = true;
        copy_str_field(cs.text,  sizeof(cs.text),  cJSON_GetObjectItem(current, "text"));
        copy_str_field(cs.notes, sizeof(cs.notes), cJSON_GetObjectItem(current, "notes"));
        copy_str_field(cs.uuid,  sizeof(cs.uuid),  cJSON_GetObjectItem(current, "uuid"));
    }
    if (cJSON_IsObject(next)) {
        ns.valid = true;
        copy_str_field(ns.text,  sizeof(ns.text),  cJSON_GetObjectItem(next, "text"));
        copy_str_field(ns.notes, sizeof(ns.notes), cJSON_GetObjectItem(next, "notes"));
        copy_str_field(ns.uuid,  sizeof(ns.uuid),  cJSON_GetObjectItem(next, "uuid"));
    }
    app_pp_state_set_slides(&cs, &ns);
}

static void handle_timers_current(cJSON *arr)
{
    app_pp_timer_t out[APP_PP_MAX_TIMERS] = {0};
    size_t n = 0;
    cJSON *t = NULL;
    cJSON_ArrayForEach(t, arr) {
        if (n >= APP_PP_MAX_TIMERS) break;
        if (!cJSON_IsObject(t)) continue;
        cJSON *id    = cJSON_GetObjectItem(t, "id");
        cJSON *time  = cJSON_GetObjectItem(t, "time");
        cJSON *state = cJSON_GetObjectItem(t, "state");
        if (!cJSON_IsObject(id)) continue;
        copy_str_field(out[n].uuid,     sizeof(out[n].uuid),     cJSON_GetObjectItem(id, "uuid"));
        copy_str_field(out[n].name,     sizeof(out[n].name),     cJSON_GetObjectItem(id, "name"));
        copy_str_field(out[n].time_str, sizeof(out[n].time_str), time);
        if (cJSON_IsString(state) && state->valuestring) {
            if (strcmp(state->valuestring, "running") == 0)      out[n].state = APP_PP_TIMER_RUNNING;
            else if (strcmp(state->valuestring, "overrun") == 0) out[n].state = APP_PP_TIMER_OVERRUN;
            else                                                  out[n].state = APP_PP_TIMER_STOPPED;
        }
        n++;
    }
    app_pp_state_set_timers(out, n);
}

static void handle_stage_message(cJSON *data)
{
    const char *msg = (cJSON_IsString(data) && data->valuestring)
                          ? data->valuestring
                          : "";
    app_pp_state_set_stage_message(msg);
}

static void dispatch_inner(const char *url, cJSON *data)
{
    if (strcmp(url, "status/slide") == 0)        handle_status_slide(data);
    else if (strcmp(url, "timers/current") == 0) handle_timers_current(data);
    else if (strcmp(url, "stage/message") == 0)  handle_stage_message(data);
    // `timer/system_time` is intentionally NOT in our subscribe body, but
    // PP's aggregator still emits it (~1 Hz) regardless -- empirically
    // confirmed with repro_pp_cadence.py. Drop it here so it doesn't bump
    // the activity timestamp; clock display reads SNTP-synced local time
    // instead.
    else if (strcmp(url, "timer/system_time") == 0) { /* ignore */ }
    // Other inner urls (one-shot GETs) ignored -- we don't issue them today.
}

static void handle_line(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) return;

    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsString(err)) {
        cJSON *u = cJSON_GetObjectItem(root, "url");
        ESP_LOGW(TAG, "PP err: %s (url=%s)",
                 err->valuestring,
                 (cJSON_IsString(u) && u->valuestring) ? u->valuestring : "?");
        cJSON_Delete(root);
        return;
    }

    cJSON *url  = cJSON_GetObjectItem(root, "url");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsString(url) || !data) {
        cJSON_Delete(root);
        return;
    }

    // Aggregator envelope: outer url == "v1/status/updates", inner data
    // is the wrapped subscribe envelope.
    if (strcmp(url->valuestring, "v1/status/updates") == 0 &&
        cJSON_IsObject(data)) {
        cJSON *inner_url  = cJSON_GetObjectItem(data, "url");
        cJSON *inner_data = cJSON_GetObjectItem(data, "data");
        if (cJSON_IsString(inner_url) && inner_data) {
            dispatch_inner(inner_url->valuestring, inner_data);
        }
    }
    cJSON_Delete(root);
}

// --- Outbound --------------------------------------------------------------

static bool send_line(const char *buf, size_t len)
{
    xSemaphoreTake(s_sock_mu, portMAX_DELAY);
    int sock = s_sock;
    bool ok = false;
    if (sock >= 0) {
        ssize_t n = send(sock, buf, len, 0);
        ok = (n == (ssize_t) len);
        if (!ok) {
            ESP_LOGW(TAG, "send failed (%d/%u): %s",
                     (int) n, (unsigned) len, strerror(errno));
        }
    }
    xSemaphoreGive(s_sock_mu);
    return ok;
}

static bool send_envelope_static(const char *literal)
{
    return send_line(literal, strlen(literal));
}

// Build + send an envelope where `body` is a JSON-encoded string. cJSON
// handles escaping for arbitrary user input.
static bool send_string_body(const char *path, const char *method,
                             const char *body)
{
    cJSON *env = cJSON_CreateObject();
    if (!env) return false;
    cJSON_AddStringToObject(env, "url",    path);
    cJSON_AddStringToObject(env, "method", method);
    if (body) cJSON_AddStringToObject(env, "body", body);

    char *str = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    if (!str) return false;

    size_t len = strlen(str);
    if (len + 3 > OUTBOUND_BUF_SIZE) {
        ESP_LOGW(TAG, "envelope too large (%u)", (unsigned) len);
        cJSON_free(str);
        return false;
    }
    char out[OUTBOUND_BUF_SIZE];
    memcpy(out, str, len);
    out[len]     = '\r';
    out[len + 1] = '\n';
    out[len + 2] = '\0';
    cJSON_free(str);
    return send_line(out, len + 2);
}

// --- Read loop -------------------------------------------------------------

static void process_buffered_lines(void)
{
    char *p   = s_line_buf;
    char *end = s_line_buf + s_line_buf_len;
    while (p < end) {
        if (s_discard_until_newline) {
            char *nl = memchr(p, '\n', (size_t)(end - p));
            if (!nl) { p = end; break; }
            p = nl + 1;
            s_discard_until_newline = false;
            continue;
        }
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        *nl = '\0';
        // Trim trailing CR
        if (nl > p && *(nl - 1) == '\r') *(nl - 1) = '\0';
        if (*p) handle_line(p);
        p = nl + 1;
    }
    size_t leftover = (size_t)(end - p);
    if (leftover > 0 && p != s_line_buf) {
        memmove(s_line_buf, p, leftover);
    }
    s_line_buf_len = leftover;
}

static void read_loop(int sock)
{
    s_line_buf_len = 0;
    s_discard_until_newline = false;
    for (;;) {
        if (atomic_load(&s_force_reconnect)) {
            ESP_LOGI(TAG, "force-reconnect requested");
            atomic_store(&s_force_reconnect, false);
            return;
        }
        if (s_line_buf_len >= LINE_BUF_SIZE - 1) {
            // Single message longer than buffer. We don't have the bytes
            // to parse it anyway -- drop what we have and skip the rest
            // of that line so we don't feed mid-message fragments into
            // the JSON parser.
            ESP_LOGW(TAG, "line buffer overflowed (%u), discarding to next newline",
                     (unsigned) s_line_buf_len);
            s_line_buf_len = 0;
            s_discard_until_newline = true;
        }
        ssize_t n = recv(sock,
                         s_line_buf + s_line_buf_len,
                         LINE_BUF_SIZE - 1 - s_line_buf_len,
                         0);
        if (n > 0) {
            s_line_buf_len += (size_t) n;
            s_line_buf[s_line_buf_len] = '\0';
            process_buffered_lines();
            continue;
        }
        if (n == 0) {
            ESP_LOGI(TAG, "PP closed connection");
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Recv timeout. Probe 2 shows PP doesn't drop us during
            // brief silence; loop back to re-check s_force_reconnect.
            // (TCP keepalive will catch a genuinely dead peer at the
            // socket layer.)
            continue;
        }
        ESP_LOGW(TAG, "recv err: %s", strerror(errno));
        return;
    }
}

// --- Connect ---------------------------------------------------------------

static int connect_to_pp(void)
{
    const char *host = app_config_pp_host();
    uint16_t    port = app_config_pp_port();
    if (!host || !*host || port == 0) {
        ESP_LOGW(TAG, "no PP host/port configured");
        return -1;
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGW(TAG, "getaddrinfo(%s) failed (err=%d)", host, err);
        if (res) freeaddrinfo(res);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv_short = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv_short, sizeof(tv_short));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_short, sizeof(tv_short));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "connect(%s:%u) failed: %s",
                 host, (unsigned) port, strerror(errno));
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    // TCP keepalive so a dead peer is detected within ~60s. Failures are
    // non-fatal -- application-level recv timeout still rolls the
    // connection eventually -- but log so we know it degraded.
    int on = 1, idle = 30, intvl = 10, cnt = 3;
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE,   &on,    sizeof(on))    != 0 ||
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle))  != 0 ||
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) != 0 ||
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt))   != 0) {
        ESP_LOGW(TAG, "TCP keepalive setsockopt failed: %s", strerror(errno));
    }

    // Once connected, recv blocks 1s at a time so the read loop can poll
    // s_force_reconnect with low latency. Recv-timeout is cheap -- it
    // just unblocks EAGAIN and we re-enter.
    struct timeval tv_long = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_long, sizeof(tv_long));

    ESP_LOGI(TAG, "connected to %s:%u", host, (unsigned) port);
    return sock;
}

// --- Task ------------------------------------------------------------------

static void tcp_task(void *arg)
{
    int backoff_ms = 1000;
    for (;;) {
        set_state(APP_PP_CONN_CONNECTING);
        int sock = connect_to_pp();
        if (sock < 0) {
            set_state(APP_PP_CONN_DISCONNECTED);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            if (backoff_ms < 30000) {
                backoff_ms *= 2;
                if (backoff_ms > 30000) backoff_ms = 30000;
            }
            continue;
        }
        backoff_ms = 1000;

        xSemaphoreTake(s_sock_mu, portMAX_DELAY);
        s_sock = sock;
        xSemaphoreGive(s_sock_mu);

        if (!send_envelope_static(SUBSCRIBE_ENV)) {
            ESP_LOGW(TAG, "subscribe send failed");
            xSemaphoreTake(s_sock_mu, portMAX_DELAY);
            s_sock = -1;
            xSemaphoreGive(s_sock_mu);
            close(sock);
            set_state(APP_PP_CONN_RECONNECTING);
            continue;
        }

        set_state(APP_PP_CONN_CONNECTED);
        read_loop(sock);

        xSemaphoreTake(s_sock_mu, portMAX_DELAY);
        s_sock = -1;
        xSemaphoreGive(s_sock_mu);
        close(sock);
        set_state(APP_PP_CONN_RECONNECTING);
        // Small pause before reconnect attempt so we don't hot-spin on
        // a flaky link.
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// --- Iface impl ------------------------------------------------------------

static void iface_start(void)
{
    if (s_task) return;
    s_sock_mu = xSemaphoreCreateMutex();
    s_obs_mu  = xSemaphoreCreateMutex();
    atomic_store(&s_state, APP_PP_CONN_DISCONNECTED);
    atomic_store(&s_force_reconnect, false);
    BaseType_t ok = xTaskCreatePinnedToCore(
        tcp_task, "pp_tcp", 8192, NULL, 5, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task spawn failed");
        s_task = NULL;
    }
}

static app_pp_conn_state_t iface_get_state(void)
{
    return (app_pp_conn_state_t) atomic_load(&s_state);
}

static void iface_register_on_change(app_pp_on_change_t cb, void *ctx)
{
    if (!cb || !s_obs_mu) return;
    xSemaphoreTake(s_obs_mu, portMAX_DELAY);
    if (s_obs_count < MAX_OBSERVERS) {
        s_obs[s_obs_count].cb  = cb;
        s_obs[s_obs_count].ctx = ctx;
        s_obs_count++;
    } else {
        ESP_LOGW(TAG, "observer table full (max %d)", MAX_OBSERVERS);
    }
    xSemaphoreGive(s_obs_mu);
}

static bool iface_stage_message_put(const char *msg)
{
    if (!msg) return false;
    return send_string_body("v1/stage/message", "PUT", msg);
}

static bool iface_stage_message_clear(void)
{
    // DELETE with no body. Build inline.
    static const char ENV[] =
        "{\"url\":\"v1/stage/message\",\"method\":\"DELETE\"}\r\n";
    return send_envelope_static(ENV);
}

static bool iface_trigger_next(void)
{
    static const char ENV[] =
        "{\"url\":\"v1/presentation/active/next/trigger\"}\r\n";
    return send_envelope_static(ENV);
}

static bool iface_trigger_previous(void)
{
    static const char ENV[] =
        "{\"url\":\"v1/presentation/active/previous/trigger\"}\r\n";
    return send_envelope_static(ENV);
}

static bool iface_resubscribe(void)
{
    atomic_store(&s_force_reconnect, true);
    return true;
}

static const app_pp_client_iface_t IFACE = {
    .start                = iface_start,
    .get_state            = iface_get_state,
    .register_on_change   = iface_register_on_change,
    .stage_message_put    = iface_stage_message_put,
    .stage_message_clear  = iface_stage_message_clear,
    .trigger_next         = iface_trigger_next,
    .trigger_previous     = iface_trigger_previous,
    .resubscribe          = iface_resubscribe,
};

const app_pp_client_iface_t *app_pp_client_tcp(void)
{
    return &IFACE;
}
