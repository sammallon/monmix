// Real Mixing Station client for the PC sim. Implements the same
// ms_client_iface_t the mock does, but talks WebSocket + REST to a live
// MS instance via mongoose. Mirrors the protocol semantics worked out in
// main/app_ms_client_ws.c (and the reference_mixing_station_protocol
// memory) but skips the watchdog/SET-tracker/per-PR polish — for sim
// use the network is local and reliable.
//
// Threading mirrors the tablet:
//   Worker thread       — runs mg_mgr_poll, parses inbound broadcasts,
//                          drains the outbound queue, calls into
//                          app_state_set_* directly. app_state itself is
//                          mutex-guarded, and on-change callbacks fire
//                          synchronously back into app_ui.c which uses
//                          lvgl_port_lock + lv_async_call to bridge into
//                          LVGL — exactly the tablet's pattern, so
//                          timing-related races reproduce here.
//   Main thread (LVGL)  — calls iface->set_level/set_mute/set_name etc.
//                          which push JSON onto the mutex-guarded queue.

#include "app_ms_client.h"

#include "app_state.h"
#include "app_prefs.h"
#include "app_ui.h"

#include <SDL.h>
#include "mongoose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define MS_GET_PREFIX     "/console/data/get/"
#define MS_GET_PREFIX_LEN (sizeof(MS_GET_PREFIX) - 1)
#define MAX_OBSERVERS     4

typedef struct outq_entry {
    char               *json;
    struct outq_entry  *next;
} outq_entry_t;

typedef struct {
    char            host[64];
    int             port;
    char            ws_url[128];
    char            http_base[128];

    SDL_Thread     *thread;
    volatile bool   running;

    struct mg_mgr   mgr;
    struct mg_connection *ws_conn;
    bool                  ws_open;
    bool                  subscribed;

    SDL_mutex      *outq_mtx;
    outq_entry_t   *outq_head;
    outq_entry_t   *outq_tail;

    app_ms_state_t  state;
    app_level_format_t level_fmt;
    int             mix_idx;
    int             mix_offset;
    int             mix_count;

    struct {
        app_ms_on_change_t cb;
        void              *ctx;
    } observers[MAX_OBSERVERS];
    size_t observer_count;
} ms_real_t;

static ms_real_t g_ms;
static char      g_strip_name_buf[64];

// ────────────────────────────────────────────────────────────────────────────
// State + observers
// ────────────────────────────────────────────────────────────────────────────

static void notify_state_change(app_ms_state_t s) {
    g_ms.state = s;
    for (size_t i = 0; i < g_ms.observer_count; ++i) {
        g_ms.observers[i].cb(g_ms.observers[i].ctx);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Outbound queue
// ────────────────────────────────────────────────────────────────────────────

// strdup is non-portable in C99 mode; this lives in the same TU so it
// doesn't pollute global namespace.
static char *strdup_safe(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static void outq_push(const char *json) {
    outq_entry_t *e = (outq_entry_t *)malloc(sizeof *e);
    if (!e) return;
    e->json = strdup_safe(json);
    e->next = NULL;
    SDL_LockMutex(g_ms.outq_mtx);
    if (g_ms.outq_tail) g_ms.outq_tail->next = e; else g_ms.outq_head = e;
    g_ms.outq_tail = e;
    SDL_UnlockMutex(g_ms.outq_mtx);
}

static outq_entry_t *outq_drain(void) {
    SDL_LockMutex(g_ms.outq_mtx);
    outq_entry_t *h = g_ms.outq_head;
    g_ms.outq_head = g_ms.outq_tail = NULL;
    SDL_UnlockMutex(g_ms.outq_mtx);
    return h;
}

// ────────────────────────────────────────────────────────────────────────────
// JSON helpers
// ────────────────────────────────────────────────────────────────────────────

static void send_subscribe(const char *dotted, const char *fmt) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"POST\",\"path\":\"/console/data/subscribe\","
             "\"body\":{\"path\":\"%s\",\"format\":\"%s\"}}",
             dotted, fmt);
    outq_push(buf);
}

static void send_set_norm(const char *dotted, float v) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"POST\",\"path\":\"/console/data/set/%s/norm\","
             "\"body\":{\"value\":%.6f}}",
             dotted, v);
    outq_push(buf);
}

static void send_set_bool(const char *dotted, bool v) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"POST\",\"path\":\"/console/data/set/%s/val\","
             "\"body\":{\"value\":%s}}",
             dotted, v ? "true" : "false");
    outq_push(buf);
}

static void send_set_str(const char *dotted, const char *s) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"POST\",\"path\":\"/console/data/set/%s/val\","
             "\"body\":{\"value\":\"%s\"}}",
             dotted, s);
    outq_push(buf);
}

// Subscribe to everything for one channel index.
static void subscribe_channel(int ch_id, int mix_idx) {
    char dotted[64];
    snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", ch_id);
    send_subscribe(dotted, "val");
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.lvl", ch_id, mix_idx);
    send_subscribe(dotted, "norm");
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.on", ch_id, mix_idx);
    send_subscribe(dotted, "val");
}

static void subscribe_master(int master_id) {
    char dotted[64];
    snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", master_id);
    send_subscribe(dotted, "val");
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.lvl", master_id);
    send_subscribe(dotted, "norm");
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.on", master_id);
    send_subscribe(dotted, "val");
}

static void subscribe_all(void) {
    size_t n = app_state_count();
    for (size_t i = 0; i < n; ++i) {
        int id = app_state_id_for_idx(i);
        if (id >= 0) subscribe_channel(id, g_ms.mix_idx);
    }
    app_channel_t master;
    if (app_state_master_get(&master) && master.id >= 0) {
        subscribe_master(master.id);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Inbound broadcast routing
// ────────────────────────────────────────────────────────────────────────────

// Parse a path like "ch.5.levelData.0.lvl" into ch_id + suffix.
// Returns suffix pointer on success, NULL on no-match.
static const char *parse_ch_path(const char *path, int *out_id) {
    if (strncmp(path, "ch.", 3) != 0) return NULL;
    const char *p = path + 3;
    int id = 0;
    if (*p < '0' || *p > '9') return NULL;
    while (*p >= '0' && *p <= '9') { id = id * 10 + (*p - '0'); ++p; }
    if (*p != '.') return NULL;
    *out_id = id;
    return p + 1;  // skip dot
}

static void route_inbound(const char *path, double num_value, const char *str_value, bool is_bool, bool bool_value) {
    int id;
    const char *suf = parse_ch_path(path, &id);
    if (!suf) return;

    int idx = app_state_idx_for_id(id);
    bool is_master = false;
    app_channel_t master;
    if (app_state_master_get(&master) && master.id == id) is_master = true;

    if (strcmp(suf, "cfg.name") == 0 && str_value) {
        if (idx >= 0)       app_state_set_name(idx, str_value, true);
        else if (is_master) app_state_master_set_name(str_value, true);
        return;
    }
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "levelData.%d.", g_ms.mix_idx);
    size_t plen = strlen(prefix);
    if (strncmp(suf, prefix, plen) == 0) {
        const char *tail = suf + plen;
        if (strcmp(tail, "lvl") == 0 && idx >= 0) {
            app_state_set_level(idx, (float)num_value, true);
        } else if (strcmp(tail, "on") == 0 && idx >= 0 && is_bool) {
            // MS reports "on" = audible = NOT muted.
            app_state_set_mute(idx, !bool_value, true);
        }
        return;
    }
    if (is_master) {
        if (strcmp(suf, "mix.lvl") == 0) {
            app_state_master_set_level((float)num_value, true);
        } else if (strcmp(suf, "mix.on") == 0 && is_bool) {
            app_state_master_set_mute(!bool_value, true);
        }
    }
}

static void handle_ws_message(struct mg_str data) {
    // Expect {"path":"/console/data/get/<dotted>","method":"GET","body":{...}}
    int path_off = mg_json_get(data, "$.path", NULL);
    if (path_off < 0) return;

    char *path_str = mg_json_get_str(data, "$.path");
    if (!path_str) return;
    if (strncmp(path_str, MS_GET_PREFIX, MS_GET_PREFIX_LEN) != 0) {
        free(path_str);
        return;
    }
    const char *dotted = path_str + MS_GET_PREFIX_LEN;

    char *body_str = mg_json_get_str(data, "$.body.value");
    double num_val = 0.0;
    bool   is_bool = false, bool_val = false;
    int    body_num_off = mg_json_get(data, "$.body.value", NULL);
    if (body_num_off >= 0) {
        // Try as number, then bool.
        if (mg_json_get_num(data, "$.body.value", &num_val)) {
            // numeric
        } else {
            bool b;
            if (mg_json_get_bool(data, "$.body.value", &b)) {
                is_bool = true;
                bool_val = b;
            }
        }
    }

    route_inbound(dotted, num_val, body_str, is_bool, bool_val);

    free(path_str);
    if (body_str) free(body_str);
}

// ────────────────────────────────────────────────────────────────────────────
// mongoose event handler
// ────────────────────────────────────────────────────────────────────────────

static void ws_evt(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_OPEN) {
        // TCP connected; nothing to do until WS handshake completes.
    } else if (ev == MG_EV_ERROR) {
        fprintf(stderr, "ms_real: connection error: %s\n", (const char *)ev_data);
        notify_state_change(APP_MS_STATE_ERROR);
    } else if (ev == MG_EV_WS_OPEN) {
        printf("ms_real: WS open to %s\n", g_ms.ws_url);
        fflush(stdout);
        g_ms.ws_open = true;
        notify_state_change(APP_MS_STATE_CONNECTED);
        if (!g_ms.subscribed) {
            subscribe_all();
            g_ms.subscribed = true;
        }
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *m = (struct mg_ws_message *)ev_data;
        handle_ws_message(m->data);
    } else if (ev == MG_EV_CLOSE) {
        printf("ms_real: WS closed\n");
        fflush(stdout);
        g_ms.ws_open    = false;
        g_ms.ws_conn    = NULL;
        g_ms.subscribed = false;
        notify_state_change(APP_MS_STATE_DISCONNECTED);
    }
}

static void info_evt(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_CONNECT) {
        // mg_http_connect opens the TCP socket but doesn't issue the
        // request. Send the GET on connect; mg_http_get_request_uri
        // requires a URL with the path included for that to work.
        mg_printf(c, "GET /console/information HTTP/1.0\r\nHost: %s:%d\r\n\r\n",
                  g_ms.host, g_ms.port);
    } else if (ev == MG_EV_ERROR) {
        printf("ms_real: info GET error: %s\n", (const char *)ev_data);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        struct mg_str body = hm->body;

        // Walk channelTypes[i] until the array ends. Real MS reports
        // name="Mix" (with offset+count); stash both and tell the UI so
        // the mix-indicator pill becomes eligible to show.
        for (int i = 0; i < 32; ++i) {
            char namepath[40], offpath[40], cntpath[40];
            snprintf(namepath, sizeof(namepath), "$.channelTypes[%d].name",   i);
            snprintf(offpath,  sizeof(offpath),  "$.channelTypes[%d].offset", i);
            snprintf(cntpath,  sizeof(cntpath),  "$.channelTypes[%d].count",  i);
            char *name = mg_json_get_str(body, namepath);
            if (!name) break;
            double off = 0, cnt = 0;
            mg_json_get_num(body, offpath, &off);
            mg_json_get_num(body, cntpath, &cnt);
            if (strcmp(name, "Mix") == 0) {
                g_ms.mix_offset = (int)off;
                g_ms.mix_count  = (int)cnt;
                printf("ms_real: mix offset=%d count=%d\n",
                       g_ms.mix_offset, g_ms.mix_count);
                app_ui_set_mix_count(g_ms.mix_count);
            }
            free(name);
        }

        double total = 0;
        if (mg_json_get_num(body, "$.totalChannels", &total) && total > 0) {
            app_ui_set_channel_total((int)total);
        }

        c->is_draining = 1;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Worker thread
// ────────────────────────────────────────────────────────────────────────────

static int worker_thread(void *unused) {
    (void)unused;
    mg_mgr_init(&g_ms.mgr);
    notify_state_change(APP_MS_STATE_CONNECTING);

    // Kick off REST /console/information once to discover mix layout.
    char info_url[128];
    snprintf(info_url, sizeof(info_url), "%s/console/information", g_ms.http_base);
    mg_http_connect(&g_ms.mgr, info_url, info_evt, NULL);

    // Open WS.
    g_ms.ws_conn = mg_ws_connect(&g_ms.mgr, g_ms.ws_url, ws_evt, NULL, NULL);

    uint32_t last_reconnect_attempt_ms = 0;

    while (g_ms.running) {
        mg_mgr_poll(&g_ms.mgr, 25);

        // Drain outbound queue if WS is up.
        if (g_ms.ws_open && g_ms.ws_conn) {
            outq_entry_t *e = outq_drain();
            while (e) {
                mg_ws_send(g_ms.ws_conn, e->json, strlen(e->json), WEBSOCKET_OP_TEXT);
                outq_entry_t *next = e->next;
                free(e->json);
                free(e);
                e = next;
            }
        }

        // Reconnect if dropped. mongoose closes the conn on error/disconnect
        // and we don't have a built-in retry — re-attempt every 2 s.
        if (!g_ms.ws_conn) {
            uint32_t now = SDL_GetTicks();
            if (now - last_reconnect_attempt_ms > 2000) {
                last_reconnect_attempt_ms = now;
                g_ms.ws_conn = mg_ws_connect(&g_ms.mgr, g_ms.ws_url, ws_evt, NULL, NULL);
            }
        }
    }

    mg_mgr_free(&g_ms.mgr);
    return 0;
}

// ────────────────────────────────────────────────────────────────────────────
// ms_client_iface_t implementations
// ────────────────────────────────────────────────────────────────────────────

static bool m_start(void) { return true; /* started by ms_client_real_create */ }
static void m_stop(void)  {
    g_ms.running = false;
    if (g_ms.thread) {
        SDL_WaitThread(g_ms.thread, NULL);
        g_ms.thread = NULL;
    }
}

static void m_set_level(int id, float level) {
    char dotted[64];
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.lvl", id, g_ms.mix_idx);
    send_set_norm(dotted, level);
}

static void m_set_mute(int id, bool mute) {
    char dotted[64];
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.on", id, g_ms.mix_idx);
    // MS "on" = audible = NOT muted.
    send_set_bool(dotted, !mute);
}

static void m_set_name(int id, const char *name) {
    char dotted[64];
    snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", id);
    send_set_str(dotted, name ? name : "");
}

static void m_set_master_level(float level) {
    app_channel_t master;
    if (!app_state_master_get(&master) || master.id < 0) return;
    char dotted[64];
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.lvl", master.id);
    send_set_norm(dotted, level);
}

static void m_set_master_mute(bool mute) {
    app_channel_t master;
    if (!app_state_master_get(&master) || master.id < 0) return;
    char dotted[64];
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.on", master.id);
    send_set_bool(dotted, !mute);
}

static app_ms_state_t m_get_state(void) { return g_ms.state; }
static const char    *m_get_host (void) { return g_ms.host; }
static int            m_get_port (void) { return g_ms.port; }

static void m_register(app_ms_on_change_t cb, void *ctx) {
    if (g_ms.observer_count < MAX_OBSERVERS) {
        g_ms.observers[g_ms.observer_count].cb  = cb;
        g_ms.observers[g_ms.observer_count].ctx = ctx;
        g_ms.observer_count++;
    }
}

static int m_get_mix(void)          { return g_ms.mix_idx; }
static void m_set_mix(int idx)      {
    g_ms.mix_idx = idx;
    g_ms.subscribed = false;
    if (g_ms.ws_open) subscribe_all();
}
static void m_set_mix_layout(int off, int cnt) { g_ms.mix_offset = off; g_ms.mix_count = cnt; }
static const char *m_get_mix_name(int idx) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "Mix %d", idx + 1);
    return buf;
}
static bool m_is_mix_routed(int idx)   { (void)idx; return true; }
static void m_fetch_mix_routing(void)  {}
static bool m_is_mix_list_ready(void)  { return g_ms.ws_open && g_ms.mix_count > 0; }
static void m_resubscribe(void)        { if (g_ms.ws_open) { g_ms.subscribed = false; subscribe_all(); } }
static void m_reconnect(void) {
    if (g_ms.ws_conn) g_ms.ws_conn->is_draining = 1;
}

static void m_fetch_all_strip_names(int total) { (void)total; /* WS subscribe path covers it */ }
static const char *m_get_strip_name(int id) {
    snprintf(g_strip_name_buf, sizeof(g_strip_name_buf), "CH %02d", id + 1);
    return g_strip_name_buf;
}
static void m_fetch_channel_routability(int total) { (void)total; }
static bool m_is_channel_routable(int id)          { (void)id; return true; }
static void m_set_meter_enabled(bool on)           { (void)on; }
static void m_set_level_format(app_level_format_t f) { g_ms.level_fmt = f; }

static const ms_client_iface_t s_iface = {
    .start                       = m_start,
    .set_level                   = m_set_level,
    .set_mute                    = m_set_mute,
    .set_name                    = m_set_name,
    .stop                        = m_stop,
    .get_state                   = m_get_state,
    .get_host                    = m_get_host,
    .get_port                    = m_get_port,
    .register_on_change          = m_register,
    .get_mix                     = m_get_mix,
    .set_mix                     = m_set_mix,
    .set_mix_layout              = m_set_mix_layout,
    .get_mix_name                = m_get_mix_name,
    .is_mix_routed               = m_is_mix_routed,
    .fetch_mix_routing           = m_fetch_mix_routing,
    .is_mix_list_ready           = m_is_mix_list_ready,
    .resubscribe                 = m_resubscribe,
    .reconnect                   = m_reconnect,
    .set_master_level            = m_set_master_level,
    .set_master_mute             = m_set_master_mute,
    .fetch_all_strip_names       = m_fetch_all_strip_names,
    .get_strip_name              = m_get_strip_name,
    .fetch_channel_routability   = m_fetch_channel_routability,
    .is_channel_routable         = m_is_channel_routable,
    .set_meter_enabled           = m_set_meter_enabled,
    .set_level_format            = m_set_level_format,
};

const ms_client_iface_t *ms_client_real_create(const char *host, int port) {
    snprintf(g_ms.host, sizeof(g_ms.host), "%s", host);
    g_ms.port = port;
    snprintf(g_ms.ws_url,    sizeof(g_ms.ws_url),    "ws://%s:%d/", host, port);
    snprintf(g_ms.http_base, sizeof(g_ms.http_base), "http://%s:%d", host, port);

    g_ms.outq_mtx = SDL_CreateMutex();
    g_ms.running  = true;
    g_ms.state    = APP_MS_STATE_BOOT;
    g_ms.thread   = SDL_CreateThread(worker_thread, "ms_real_ws", NULL);
    return &s_iface;
}
