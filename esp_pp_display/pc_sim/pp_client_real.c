// PC-side ProPresenter TCP client. Connects to a live PP instance on
// host:63306, sends the same aggregator subscribe envelope the firmware
// uses, parses newline-JSON broadcasts, and writes them into app_pp_state.
//
// Mirrors main/app_pp_client_tcp.c in structure but uses Winsock / BSD
// sockets directly instead of going through lwIP, and an SDL_Thread for
// the read loop instead of a FreeRTOS task.

// Winsock first, before anything else can pull in winsock 1.1 via
// windows.h. Otherwise we get the dual-include disaster: winsock.h
// from windows.h declares struct sockaddr_in / hostent / etc., then
// winsock2.h tries to redefine them and MSVC chokes.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int ssize_t;
#define MSG_NOSIGNAL 0
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#define closesocket close
#define SOCKET int
#define INVALID_SOCKET (-1)
#endif

#include "pp_client_real.h"
#include "app_pp_state.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define LINE_BUF_SIZE 8192

static const char SUBSCRIBE_ENV[] =
    "{\"url\":\"v1/status/updates\",\"method\":\"POST\","
    "\"body\":[\"status/slide\",\"timers/current\",\"stage/message\"],"
    "\"chunked\":true}\r\n";

#define MAX_OBS 4
typedef struct { app_pp_on_change_t cb; void *ctx; } obs_t;

static char     s_pp_host[128];
static uint16_t s_pp_port;
static SDL_Thread *s_thread;
static volatile int s_running;
static SOCKET s_sock = INVALID_SOCKET;
static SDL_mutex *s_sock_mu;
static app_pp_conn_state_t s_state = APP_PP_CONN_DISCONNECTED;
static SDL_mutex *s_obs_mu;
static obs_t s_obs[MAX_OBS];
static size_t s_obs_count;

static void notify_state(void)
{
    obs_t snap[MAX_OBS];
    size_t n;
    SDL_LockMutex(s_obs_mu);
    n = s_obs_count;
    memcpy(snap, s_obs, n * sizeof(obs_t));
    SDL_UnlockMutex(s_obs_mu);
    for (size_t i = 0; i < n; ++i) snap[i].cb(snap[i].ctx);
}

static void set_state(app_pp_conn_state_t s)
{
    if (s == s_state) return;
    s_state = s;
    notify_state();
}

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

static void handle_status_slide(cJSON *data)
{
    cJSON *current = cJSON_GetObjectItem(data, "current");
    cJSON *next    = cJSON_GetObjectItem(data, "next");
    app_pp_slide_t cs = {0}, ns = {0};
    if (cJSON_IsObject(current)) {
        cs.valid = 1;
        copy_str_field(cs.text,  sizeof(cs.text),  cJSON_GetObjectItem(current, "text"));
        copy_str_field(cs.notes, sizeof(cs.notes), cJSON_GetObjectItem(current, "notes"));
        copy_str_field(cs.uuid,  sizeof(cs.uuid),  cJSON_GetObjectItem(current, "uuid"));
    }
    if (cJSON_IsObject(next)) {
        ns.valid = 1;
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
            else                                                 out[n].state = APP_PP_TIMER_STOPPED;
        }
        n++;
    }
    app_pp_state_set_timers(out, n);
}

static void handle_stage_message(cJSON *data)
{
    const char *msg = (cJSON_IsString(data) && data->valuestring)
                          ? data->valuestring : "";
    app_pp_state_set_stage_message(msg);
}

static void dispatch_inner(const char *url, cJSON *data)
{
    if      (strcmp(url, "status/slide") == 0)      handle_status_slide(data);
    else if (strcmp(url, "timers/current") == 0)    handle_timers_current(data);
    else if (strcmp(url, "stage/message") == 0)     handle_stage_message(data);
    else if (strcmp(url, "timer/system_time") == 0) { /* drop */ }
}

static void handle_line(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) return;
    cJSON *url  = cJSON_GetObjectItem(root, "url");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsString(url) && data &&
        strcmp(url->valuestring, "v1/status/updates") == 0 &&
        cJSON_IsObject(data)) {
        cJSON *inner_url  = cJSON_GetObjectItem(data, "url");
        cJSON *inner_data = cJSON_GetObjectItem(data, "data");
        if (cJSON_IsString(inner_url) && inner_data) {
            dispatch_inner(inner_url->valuestring, inner_data);
        }
    }
    cJSON_Delete(root);
}

static SOCKET connect_to_pp(void)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", s_pp_port);
    struct addrinfo hints = { 0 };
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(s_pp_host, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "[pp_real] getaddrinfo(%s) failed\n", s_pp_host);
        if (res) freeaddrinfo(res);
        return INVALID_SOCKET;
    }
    SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "[pp_real] socket() failed\n");
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }
    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        fprintf(stderr, "[pp_real] connect(%s:%u) failed\n", s_pp_host, s_pp_port);
        closesocket(sock);
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }
    freeaddrinfo(res);
    fprintf(stderr, "[pp_real] connected to %s:%u\n", s_pp_host, s_pp_port);
    return sock;
}

static int read_loop(SOCKET sock)
{
    char buf[LINE_BUF_SIZE];
    size_t len = 0;
    int discard = 0;
    while (s_running) {
        if (len >= LINE_BUF_SIZE - 1) {
            fprintf(stderr, "[pp_real] line overflow, discarding to newline\n");
            len = 0;
            discard = 1;
        }
        ssize_t n = recv(sock, buf + len, (int)(LINE_BUF_SIZE - 1 - len), 0);
        if (n <= 0) {
            if (n == 0) fprintf(stderr, "[pp_real] PP closed connection\n");
            else        fprintf(stderr, "[pp_real] recv err\n");
            return -1;
        }
        len += (size_t)n;
        buf[len] = '\0';
        char *p   = buf;
        char *end = buf + len;
        while (p < end) {
            if (discard) {
                char *nl = memchr(p, '\n', (size_t)(end - p));
                if (!nl) { p = end; break; }
                p = nl + 1;
                discard = 0;
                continue;
            }
            char *nl = memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            *nl = '\0';
            if (nl > p && *(nl - 1) == '\r') *(nl - 1) = '\0';
            if (*p) handle_line(p);
            p = nl + 1;
        }
        size_t leftover = (size_t)(end - p);
        if (leftover > 0 && p != buf) memmove(buf, p, leftover);
        len = leftover;
    }
    return 0;
}

static int read_thread_fn(void *arg)
{
    (void) arg;
    int backoff_ms = 1000;
    while (s_running) {
        set_state(APP_PP_CONN_CONNECTING);
        SOCKET sock = connect_to_pp();
        if (sock == INVALID_SOCKET) {
            set_state(APP_PP_CONN_DISCONNECTED);
            SDL_Delay(backoff_ms);
            if (backoff_ms < 30000) {
                backoff_ms *= 2;
                if (backoff_ms > 30000) backoff_ms = 30000;
            }
            continue;
        }
        backoff_ms = 1000;
        SDL_LockMutex(s_sock_mu);
        s_sock = sock;
        SDL_UnlockMutex(s_sock_mu);
        if (send(sock, SUBSCRIBE_ENV, (int)strlen(SUBSCRIBE_ENV), 0) <= 0) {
            fprintf(stderr, "[pp_real] subscribe send failed\n");
            closesocket(sock);
            SDL_LockMutex(s_sock_mu);
            s_sock = INVALID_SOCKET;
            SDL_UnlockMutex(s_sock_mu);
            set_state(APP_PP_CONN_RECONNECTING);
            continue;
        }
        set_state(APP_PP_CONN_CONNECTED);
        read_loop(sock);
        SDL_LockMutex(s_sock_mu);
        s_sock = INVALID_SOCKET;
        SDL_UnlockMutex(s_sock_mu);
        closesocket(sock);
        set_state(APP_PP_CONN_RECONNECTING);
        SDL_Delay(500);
    }
    return 0;
}

static bool send_line(const char *buf, size_t len)
{
    SDL_LockMutex(s_sock_mu);
    SOCKET sock = s_sock;
    bool ok = false;
    if (sock != INVALID_SOCKET) {
        ok = send(sock, buf, (int)len, 0) == (int)len;
    }
    SDL_UnlockMutex(s_sock_mu);
    return ok;
}

static void real_start(void) { /* started in pp_client_real_init */ }
static app_pp_conn_state_t real_get_state(void) { return s_state; }
static void real_register_on_change(app_pp_on_change_t cb, void *ctx)
{
    if (!cb) return;
    SDL_LockMutex(s_obs_mu);
    if (s_obs_count < MAX_OBS) {
        s_obs[s_obs_count].cb  = cb;
        s_obs[s_obs_count].ctx = ctx;
        s_obs_count++;
    }
    SDL_UnlockMutex(s_obs_mu);
}

static bool real_stage_message_put(const char *msg)
{
    if (!msg) return false;
    cJSON *env = cJSON_CreateObject();
    cJSON_AddStringToObject(env, "url",    "v1/stage/message");
    cJSON_AddStringToObject(env, "method", "PUT");
    cJSON_AddStringToObject(env, "body",   msg);
    char *str = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    if (!str) return false;
    size_t len = strlen(str);
    char out[1024];
    if (len + 3 > sizeof(out)) { cJSON_free(str); return false; }
    memcpy(out, str, len);
    out[len] = '\r'; out[len + 1] = '\n'; out[len + 2] = '\0';
    cJSON_free(str);
    return send_line(out, len + 2);
}

static bool real_stage_message_clear(void)
{
    const char ENV[] = "{\"url\":\"v1/stage/message\",\"method\":\"DELETE\"}\r\n";
    return send_line(ENV, sizeof(ENV) - 1);
}

static bool real_trigger_next(void)
{
    const char ENV[] = "{\"url\":\"v1/presentation/active/next/trigger\"}\r\n";
    return send_line(ENV, sizeof(ENV) - 1);
}

static bool real_trigger_previous(void)
{
    const char ENV[] = "{\"url\":\"v1/presentation/active/previous/trigger\"}\r\n";
    return send_line(ENV, sizeof(ENV) - 1);
}

static bool real_resubscribe(void)
{
    SDL_LockMutex(s_sock_mu);
    SOCKET sock = s_sock;
    s_sock = INVALID_SOCKET;
    SDL_UnlockMutex(s_sock_mu);
    if (sock != INVALID_SOCKET) closesocket(sock);
    return true;
}

static const app_pp_client_iface_t IFACE = {
    .start               = real_start,
    .get_state           = real_get_state,
    .register_on_change  = real_register_on_change,
    .stage_message_put   = real_stage_message_put,
    .stage_message_clear = real_stage_message_clear,
    .trigger_next        = real_trigger_next,
    .trigger_previous    = real_trigger_previous,
    .resubscribe         = real_resubscribe,
};

const app_pp_client_iface_t *pp_client_real(const char *host, uint16_t port)
{
#ifdef _WIN32
    WSADATA wsa;
    static int s_wsa_inited = 0;
    if (!s_wsa_inited) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "[pp_real] WSAStartup failed\n");
            return NULL;
        }
        s_wsa_inited = 1;
    }
#endif
    strncpy(s_pp_host, host, sizeof(s_pp_host) - 1);
    s_pp_port = port;
    s_sock_mu = SDL_CreateMutex();
    s_obs_mu  = SDL_CreateMutex();
    s_running = 1;
    s_thread  = SDL_CreateThread(read_thread_fn, "pp_real", NULL);
    return &IFACE;
}
