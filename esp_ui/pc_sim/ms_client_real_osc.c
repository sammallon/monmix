// Real Mixing Station client for the PC sim — OSC over UDP variant.
// Twin of ms_client_real.c (which is WS over TCP) so the sim can exercise
// either backend the firmware ships. Same iface, same threading model
// (one mongoose worker thread + LVGL main), same dispatch into app_state.
//
// Wire format (verified against the live MS instance, see
// reference_ms_internals memory):
//   client -> MS:
//     /con/n/<dotted> ,f <0..1>     SET normalized
//     /con/v/<dotted> ,f <units>    SET raw (dB)
//     /con/v/<dotted> ,s "<name>"   SET string (cfg.name)
//     /con/n/<dotted> (no args)     GET — MS replies on source port
//     /hi/n / /hi/v   (no args)     heartbeat-subscribe (every <=5 s);
//                                   from then on MS pushes every change
//                                   to the source UDP address
//   MS -> client (broadcast or GET reply):
//     /con/n/<dotted> ,<t> <v>      change notification or reply
//     /con/v/<dotted> ,<t> <v>      same, raw-units flavor
//
// Initial-state prime: empty /con/n/<path> GETs for every tracked
// channel's cfg.name + lvl + on, master, and each mix-bus name. Paced
// one per worker poll so MS's replies don't overflow our recv buffer.

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

#define MAX_OBSERVERS 4
#define MAX_MIX_BUSES 24

// Expectation table parameters mirror app_ms_client_osc.c so the sim
// exercises the same retry/watchdog logic. PC kernel UDP buffers make
// loss rare, but logical parity matters: any bug in the table that
// trips on the sim probably trips on hardware too.
#define OSC_EXPECT_SIZE          64
#define OSC_EXPECT_TIMEOUT_MS    1500
#define OSC_EXPECT_PATH_MAX      48
#define OSC_EXPECT_FLAG_WATCHDOG 0x01
#define OSC_WATCHDOG_INTERVAL_MS 10000
#define OSC_WATCHDOG_RETRIES     2

typedef struct {
    char     path[OSC_EXPECT_PATH_MAX];
    uint32_t sent_ms;
    uint8_t  retries_left;
    uint8_t  flags;
    char     fmt;
} osc_expect_t;

typedef struct outq_entry {
    uint8_t            *pkt;
    size_t              len;
    struct outq_entry  *next;
} outq_entry_t;

typedef struct {
    char            host[64];
    int             port;          // HTTP port (for /console/information)
    int             osc_port;      // UDP/OSC port
    char            udp_url[80];
    char            http_base[128];

    SDL_Thread     *thread;
    volatile bool   running;

    struct mg_mgr            mgr;
    struct mg_connection    *udp_conn;
    bool                     udp_open;
    uint32_t                 last_heartbeat_ms;
    uint32_t                 last_init_ms;
    bool                     info_fetched;
    bool                     primed;
    int                      prime_idx;
    int                      prime_total;

    SDL_mutex      *outq_mtx;
    outq_entry_t   *outq_head;
    outq_entry_t   *outq_tail;

    app_ms_state_t      state;
    app_level_format_t  level_fmt;
    int                 mix_idx;
    int                 mix_offset;
    int                 mix_count;
    char                mix_names[MAX_MIX_BUSES][32];
    bool                mix_list_received;

    struct {
        app_ms_on_change_t cb;
        void              *ctx;
    } observers[MAX_OBSERVERS];
    size_t observer_count;

    osc_expect_t expect[OSC_EXPECT_SIZE];
    uint32_t     last_watchdog_ms;
    uint32_t     last_expect_tick_ms;
} osc_real_t;

static osc_real_t g_osc;
static char       g_osc_strip_name_buf[64];

// ────────────────────────────────────────────────────────────────────────────
// OSC packet helpers (must match main/app_ms_client_osc.c byte-for-byte)
// ────────────────────────────────────────────────────────────────────────────

static size_t osc_padded(size_t bytes_incl_nul) {
    return (bytes_incl_nul + 3u) & ~(size_t)3u;
}

static size_t osc_build(uint8_t *out, size_t out_size,
                        const char *addr, const char *types,
                        const float *fargs, size_t nargs) {
    size_t alen   = strlen(addr) + 1;
    size_t apad   = osc_padded(alen);
    size_t tcount = (types ? strlen(types) : 0);
    size_t tlen   = 1 + tcount + 1;
    size_t tpad   = osc_padded(tlen);
    size_t total  = apad + tpad + nargs * 4u;
    if (total > out_size) return 0;
    memset(out, 0, total);
    memcpy(out, addr, alen - 1);
    size_t pos = apad;
    out[pos] = ',';
    if (tcount) memcpy(out + pos + 1, types, tcount);
    pos += tpad;
    for (size_t i = 0; i < nargs; ++i) {
        uint32_t bits;
        memcpy(&bits, &fargs[i], 4);
        out[pos + 0] = (uint8_t)(bits >> 24);
        out[pos + 1] = (uint8_t)(bits >> 16);
        out[pos + 2] = (uint8_t)(bits >> 8);
        out[pos + 3] = (uint8_t)(bits);
        pos += 4;
    }
    return pos;
}

static bool osc_split(const uint8_t *buf, size_t len,
                      const char **out_addr,
                      const char **out_types,
                      const uint8_t **out_args, size_t *out_args_len) {
    if (len < 8) return false;
    size_t alen = strnlen((const char *)buf, len);
    if (alen == len) return false;
    size_t apad = osc_padded(alen + 1);
    if (apad >= len) return false;
    if (buf[apad] != ',') return false;
    size_t tlen = strnlen((const char *)(buf + apad), len - apad);
    if (apad + tlen >= len) return false;
    size_t tpad = apad + osc_padded(tlen + 1);
    if (tpad > len) return false;
    *out_addr     = (const char *)buf;
    *out_types    = (const char *)(buf + apad + 1);
    *out_args     = buf + tpad;
    *out_args_len = len - tpad;
    return true;
}

static bool osc_first_scalar(const char *types, const uint8_t *args, size_t args_len,
                             float *out_f, bool *out_is_bool) {
    if (!types || !*types) return false;
    char t = types[0];
    if (t == 'T') { *out_f = 1.0f; *out_is_bool = true;  return true; }
    if (t == 'F') { *out_f = 0.0f; *out_is_bool = true;  return true; }
    if (args_len < 4) return false;
    uint32_t bits = ((uint32_t)args[0] << 24) | ((uint32_t)args[1] << 16) |
                    ((uint32_t)args[2] << 8)  | ((uint32_t)args[3]);
    if (t == 'f') {
        float v;
        memcpy(&v, &bits, 4);
        *out_f = v; *out_is_bool = false;
        return true;
    }
    if (t == 'i') {
        int32_t iv = (int32_t)bits;
        *out_f = (float)iv; *out_is_bool = false;
        return true;
    }
    return false;
}

// ────────────────────────────────────────────────────────────────────────────
// Expectation table -- mirrors app_ms_client_osc.c so the same retry +
// watchdog logic exercises in the sim. Worker-thread-only access (the
// sim's mongoose UDP read fires the worker via MG_EV_READ -> handle_inbound,
// so all writes/reads happen on one thread, no mutex needed).
// ────────────────────────────────────────────────────────────────────────────

static int osc_send_pkt(const uint8_t *bytes, size_t len);  // forward

static bool expect_register(const char *path, char fmt, uint8_t retries, uint8_t flags) {
    uint32_t now = SDL_GetTicks();
    for (int i = 0; i < OSC_EXPECT_SIZE; ++i) {
        if (g_osc.expect[i].path[0]) continue;
        strncpy(g_osc.expect[i].path, path, sizeof(g_osc.expect[i].path) - 1);
        g_osc.expect[i].path[sizeof(g_osc.expect[i].path) - 1] = 0;
        g_osc.expect[i].sent_ms      = now;
        g_osc.expect[i].retries_left = retries;
        g_osc.expect[i].flags        = flags;
        g_osc.expect[i].fmt          = fmt;
        return true;
    }
    return false;
}

static void expect_match(const char *dotted) {
    for (int i = 0; i < OSC_EXPECT_SIZE; ++i) {
        if (g_osc.expect[i].path[0] == 0) continue;
        if (strcmp(g_osc.expect[i].path, dotted) == 0) {
            g_osc.expect[i].path[0] = 0;
            return;
        }
    }
}

static int expect_tick(void) {
    uint32_t now = SDL_GetTicks();
    int watchdog_failed = 0;
    for (int i = 0; i < OSC_EXPECT_SIZE; ++i) {
        if (g_osc.expect[i].path[0] == 0) continue;
        if (now - g_osc.expect[i].sent_ms < OSC_EXPECT_TIMEOUT_MS) continue;
        if (g_osc.expect[i].retries_left == 0) {
            if (g_osc.expect[i].flags & OSC_EXPECT_FLAG_WATCHDOG) ++watchdog_failed;
            g_osc.expect[i].path[0] = 0;
            continue;
        }
        char addr[80];
        snprintf(addr, sizeof(addr), "/con/%c/%s", g_osc.expect[i].fmt, g_osc.expect[i].path);
        uint8_t pkt[128];
        size_t n = osc_build(pkt, sizeof(pkt), addr, NULL, NULL, 0);
        if (n) osc_send_pkt(pkt, n);
        g_osc.expect[i].retries_left--;
        g_osc.expect[i].sent_ms = now;
    }
    return watchdog_failed;
}

// ────────────────────────────────────────────────────────────────────────────
// Observers + outbound queue
// ────────────────────────────────────────────────────────────────────────────

static void notify_state_change(app_ms_state_t s) {
    if (g_osc.state == s) return;
    g_osc.state = s;
    for (size_t i = 0; i < g_osc.observer_count; ++i) {
        g_osc.observers[i].cb(g_osc.observers[i].ctx);
    }
}

static void outq_push(uint8_t *pkt, size_t len) {
    outq_entry_t *e = (outq_entry_t *)malloc(sizeof *e);
    if (!e) { free(pkt); return; }
    e->pkt = pkt; e->len = len; e->next = NULL;
    SDL_LockMutex(g_osc.outq_mtx);
    if (g_osc.outq_tail) g_osc.outq_tail->next = e; else g_osc.outq_head = e;
    g_osc.outq_tail = e;
    SDL_UnlockMutex(g_osc.outq_mtx);
}

static outq_entry_t *outq_drain(void) {
    SDL_LockMutex(g_osc.outq_mtx);
    outq_entry_t *h = g_osc.outq_head;
    g_osc.outq_head = g_osc.outq_tail = NULL;
    SDL_UnlockMutex(g_osc.outq_mtx);
    return h;
}

// ────────────────────────────────────────────────────────────────────────────
// SET / GET helpers
// ────────────────────────────────────────────────────────────────────────────

static int master_channel_id(void) {
    app_channel_t m;
    if (!app_state_master_get(&m) || m.id < 0) return -1;
    return m.id;
}

static void enqueue_set_norm(const char *dotted, float v) {
    char addr[100];
    snprintf(addr, sizeof(addr), "/con/n/%s", dotted);
    uint8_t *pkt = (uint8_t *)malloc(256);
    if (!pkt) return;
    size_t n = osc_build(pkt, 256, addr, "f", &v, 1);
    if (!n) { free(pkt); return; }
    outq_push(pkt, n);
}

static void enqueue_set_val_db(const char *dotted, float db) {
    char addr[100];
    snprintf(addr, sizeof(addr), "/con/v/%s", dotted);
    uint8_t *pkt = (uint8_t *)malloc(256);
    if (!pkt) return;
    size_t n = osc_build(pkt, 256, addr, "f", &db, 1);
    if (!n) { free(pkt); return; }
    outq_push(pkt, n);
}

static void enqueue_set_bool(const char *dotted, bool v) {
    char addr[100];
    snprintf(addr, sizeof(addr), "/con/n/%s", dotted);
    float f = v ? 1.0f : 0.0f;
    uint8_t *pkt = (uint8_t *)malloc(256);
    if (!pkt) return;
    size_t n = osc_build(pkt, 256, addr, "f", &f, 1);
    if (!n) { free(pkt); return; }
    outq_push(pkt, n);
}

static void enqueue_set_str(const char *dotted, const char *s) {
    char addr[100];
    snprintf(addr, sizeof(addr), "/con/v/%s", dotted);
    size_t alen = strlen(addr) + 1;
    size_t apad = osc_padded(alen);
    size_t tlen = 3;
    size_t tpad = osc_padded(tlen);
    size_t slen = strlen(s) + 1;
    size_t spad = osc_padded(slen);
    size_t total = apad + tpad + spad;
    uint8_t *pkt = (uint8_t *)malloc(total);
    if (!pkt) return;
    memset(pkt, 0, total);
    memcpy(pkt, addr, alen - 1);
    pkt[apad] = ',';
    pkt[apad + 1] = 's';
    memcpy(pkt + apad + tpad, s, slen - 1);
    outq_push(pkt, total);
}

static void enqueue_get(const char *dotted, char fmt) {
    char addr[100];
    snprintf(addr, sizeof(addr), "/con/%c/%s", fmt, dotted);
    uint8_t *pkt = (uint8_t *)malloc(128);
    if (!pkt) return;
    size_t n = osc_build(pkt, 128, addr, NULL, NULL, 0);
    if (!n) { free(pkt); return; }
    expect_register(dotted, fmt, 2, 0);
    outq_push(pkt, n);
}

// Send directly via mongoose (bypasses outq -- used by expect_tick for
// retries so the slot's clock matches when the bytes actually leave).
static int osc_send_pkt(const uint8_t *bytes, size_t len) {
    if (!g_osc.udp_conn || !g_osc.udp_open) return -1;
    mg_send(g_osc.udp_conn, bytes, len);
    return 0;
}

// ────────────────────────────────────────────────────────────────────────────
// Inbound dispatch (mirrors firmware app_ms_client_osc.c)
// ────────────────────────────────────────────────────────────────────────────

static const char *parse_ch_path(const char *path, int *out_id) {
    if (strncmp(path, "ch.", 3) != 0) return NULL;
    const char *p = path + 3;
    int id = 0;
    if (*p < '0' || *p > '9') return NULL;
    while (*p >= '0' && *p <= '9') { id = id * 10 + (*p - '0'); ++p; }
    if (*p != '.') return NULL;
    *out_id = id;
    return p + 1;
}

static void handle_inbound(const char *addr, const char *types,
                           const uint8_t *args, size_t args_len) {
    char fmt;
    const char *dotted;
    if (strncmp(addr, "/con/n/", 7) == 0) { fmt = 'n'; dotted = addr + 7; }
    else if (strncmp(addr, "/con/v/", 7) == 0) { fmt = 'v'; dotted = addr + 7; }
    else return;

    expect_match(dotted);

    int id = -1;
    const char *suf = parse_ch_path(dotted, &id);
    if (!suf) return;

    int  idx       = app_state_idx_for_id(id);
    bool is_master = (id == master_channel_id());

    if (strcmp(suf, "cfg.name") == 0 && types && types[0] == 's') {
        const char *name = (const char *)args;
        if (idx >= 0)        app_state_set_name(idx, name, true);
        else if (is_master)  app_state_master_set_name(name, true);
        if (g_osc.mix_count > 0 && id >= g_osc.mix_offset &&
            id < g_osc.mix_offset + g_osc.mix_count) {
            int slot = id - g_osc.mix_offset;
            strncpy(g_osc.mix_names[slot], name, sizeof(g_osc.mix_names[slot]) - 1);
            g_osc.mix_names[slot][sizeof(g_osc.mix_names[slot]) - 1] = 0;
        }
        return;
    }

    float vf;
    bool  vbool;
    if (!osc_first_scalar(types, args, args_len, &vf, &vbool)) return;

    char level_prefix[32];
    snprintf(level_prefix, sizeof(level_prefix), "levelData.%d.", g_osc.mix_idx);
    size_t plen = strlen(level_prefix);
    if (strncmp(suf, level_prefix, plen) == 0) {
        const char *tail = suf + plen;
        if (idx >= 0 && strcmp(tail, "lvl") == 0) {
            if (fmt == 'v' && g_osc.level_fmt == APP_LEVEL_FORMAT_DB) {
                app_state_set_level_db(idx, vf, true);
            } else if (fmt == 'n' && g_osc.level_fmt != APP_LEVEL_FORMAT_DB) {
                app_state_set_level(idx, vf, true);
            }
        } else if (idx >= 0 && strcmp(tail, "on") == 0) {
            bool on = (vf > 0.5f);
            app_state_set_mute(idx, !on, true);
        }
    } else if (is_master) {
        if (strcmp(suf, "mix.lvl") == 0) {
            if (fmt == 'v' && g_osc.level_fmt == APP_LEVEL_FORMAT_DB) {
                app_state_master_set_level_db(vf, true);
            } else if (fmt == 'n' && g_osc.level_fmt != APP_LEVEL_FORMAT_DB) {
                app_state_master_set_level(vf, true);
            }
        } else if (strcmp(suf, "mix.on") == 0) {
            app_state_master_set_mute(!(vf > 0.5f), true);
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// HTTP /console/information for mix layout
// ────────────────────────────────────────────────────────────────────────────

static void info_evt(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_CONNECT) {
        mg_printf(c, "GET /console/information HTTP/1.0\r\nHost: %s:%d\r\n\r\n",
                  g_osc.host, g_osc.port);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        struct mg_str body = hm->body;
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
                g_osc.mix_offset        = (int)off;
                g_osc.mix_count         = (int)cnt;
                g_osc.mix_list_received = true;
                g_osc.info_fetched      = true;
                printf("ms_real_osc: mix offset=%d count=%d\n",
                       g_osc.mix_offset, g_osc.mix_count);
                fflush(stdout);
                app_ui_set_mix_count(g_osc.mix_count);
            }
            free(name);
        }
        double total = 0;
        if (mg_json_get_num(body, "$.totalChannels", &total) && total > 0) {
            app_ui_set_channel_total((int)total);
        }
        c->is_draining = 1;
    } else if (ev == MG_EV_ERROR) {
        printf("ms_real_osc: info GET error: %s\n", (const char *)ev_data);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// UDP event handler
// ────────────────────────────────────────────────────────────────────────────

static void udp_evt(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_OPEN) {
        printf("ms_real_osc: UDP open to %s\n", g_osc.udp_url);
        fflush(stdout);
        g_osc.udp_open = true;
        notify_state_change(APP_MS_STATE_CONNECTED);
    } else if (ev == MG_EV_READ) {
        const char *addr; const char *types;
        const uint8_t *args; size_t args_len;
        if (osc_split(c->recv.buf, c->recv.len, &addr, &types, &args, &args_len)) {
            handle_inbound(addr, types, args, args_len);
        }
        c->recv.len = 0;
    } else if (ev == MG_EV_ERROR) {
        fprintf(stderr, "ms_real_osc: UDP error: %s\n", (const char *)ev_data);
        notify_state_change(APP_MS_STATE_ERROR);
    } else if (ev == MG_EV_CLOSE) {
        g_osc.udp_open = false;
        g_osc.udp_conn = NULL;
        g_osc.primed   = false;
        g_osc.prime_idx = 0;
        memset(g_osc.expect, 0, sizeof(g_osc.expect));
        notify_state_change(APP_MS_STATE_DISCONNECTED);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Heartbeat + paced GET prime (one path per poll iteration)
// ────────────────────────────────────────────────────────────────────────────

static void send_heartbeat(void) {
    if (!g_osc.udp_conn) return;
    uint8_t pkt[16];
    size_t n = osc_build(pkt, sizeof(pkt), "/hi/n", NULL, NULL, 0);
    if (n) mg_send(g_osc.udp_conn, pkt, n);
}

static bool prime_step(void) {
    int idx       = g_osc.prime_idx;
    int tracked   = (int)app_state_count();
    int mid       = master_channel_id();
    int has_master = (mid >= 0) ? 1 : 0;
    int mixes     = (g_osc.mix_count < MAX_MIX_BUSES ? g_osc.mix_count : MAX_MIX_BUSES);
    char fmt      = (g_osc.level_fmt == APP_LEVEL_FORMAT_DB) ? 'v' : 'n';

    const int per_ch = 3;
    int track_total  = tracked * per_ch;
    int master_total = has_master * 3;
    int total        = track_total + master_total + mixes;
    if (idx >= total) return false;

    char dotted[80];
    if (idx < track_total) {
        int ch_idx = idx / per_ch;
        int slot   = idx % per_ch;
        int id     = app_state_id_for_idx((size_t)ch_idx);
        if (id >= 0) {
            switch (slot) {
            case 0:
                snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", id);
                enqueue_get(dotted, 'n');
                break;
            case 1:
                snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.lvl", id, g_osc.mix_idx);
                enqueue_get(dotted, fmt);
                break;
            case 2:
                snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.on", id, g_osc.mix_idx);
                enqueue_get(dotted, 'n');
                break;
            }
        }
    } else if (idx < track_total + master_total) {
        int slot = idx - track_total;
        switch (slot) {
        case 0:
            snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", mid);
            enqueue_get(dotted, 'n');
            break;
        case 1:
            snprintf(dotted, sizeof(dotted), "ch.%d.mix.lvl", mid);
            enqueue_get(dotted, fmt);
            break;
        case 2:
            snprintf(dotted, sizeof(dotted), "ch.%d.mix.on", mid);
            enqueue_get(dotted, 'n');
            break;
        }
    } else {
        int slot = idx - track_total - master_total;
        snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", g_osc.mix_offset + slot);
        enqueue_get(dotted, 'n');
    }
    g_osc.prime_idx++;
    if (g_osc.prime_idx >= total) {
        g_osc.prime_total = total;
        g_osc.primed      = true;
        return false;
    }
    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// Worker thread
// ────────────────────────────────────────────────────────────────────────────

static int worker_thread(void *unused) {
    (void)unused;
    mg_mgr_init(&g_osc.mgr);
    notify_state_change(APP_MS_STATE_CONNECTING);

    char info_url[160];
    snprintf(info_url, sizeof(info_url), "%s/console/information", g_osc.http_base);
    mg_http_connect(&g_osc.mgr, info_url, info_evt, NULL);

    g_osc.udp_conn = mg_connect(&g_osc.mgr, g_osc.udp_url, udp_evt, NULL);

    while (g_osc.running) {
        mg_mgr_poll(&g_osc.mgr, 25);

        if (g_osc.udp_open && g_osc.udp_conn) {
            outq_entry_t *e = outq_drain();
            while (e) {
                mg_send(g_osc.udp_conn, e->pkt, e->len);
                outq_entry_t *next = e->next;
                free(e->pkt);
                free(e);
                e = next;
            }
        }

        uint32_t now = SDL_GetTicks();
        if (g_osc.udp_open && g_osc.udp_conn && now - g_osc.last_heartbeat_ms > 3000) {
            send_heartbeat();
            g_osc.last_heartbeat_ms = now;
        }

        if (g_osc.udp_open && !g_osc.primed && g_osc.mix_count > 0) {
            prime_step();
        }

        // Watchdog probe + expectation sweep mirror the firmware path
        // exactly so any breakage in the retry/timeout logic surfaces in
        // the sim too.
        if (g_osc.udp_open && g_osc.primed &&
            now - g_osc.last_watchdog_ms > OSC_WATCHDOG_INTERVAL_MS) {
            g_osc.last_watchdog_ms = now;
            int probe_id = (app_state_count() > 0) ? app_state_id_for_idx(0) : 0;
            if (probe_id < 0) probe_id = 0;
            char path[OSC_EXPECT_PATH_MAX];
            snprintf(path, sizeof(path), "ch.%d.cfg.name", probe_id);
            if (expect_register(path, 'n', OSC_WATCHDOG_RETRIES, OSC_EXPECT_FLAG_WATCHDOG)) {
                char addr[80];
                snprintf(addr, sizeof(addr), "/con/n/%s", path);
                uint8_t pkt[128];
                size_t n = osc_build(pkt, sizeof(pkt), addr, NULL, NULL, 0);
                if (n) osc_send_pkt(pkt, n);
            }
        }
        if (now - g_osc.last_expect_tick_ms > 250) {
            g_osc.last_expect_tick_ms = now;
            int wd_failed = expect_tick();
            if (wd_failed > 0 && g_osc.udp_open) {
                printf("ms_real_osc: watchdog %d fail(s) -- forcing reconnect\n", wd_failed);
                fflush(stdout);
                if (g_osc.udp_conn) g_osc.udp_conn->is_closing = 1;
            }
        }

        if (!g_osc.udp_conn) {
            if (now - g_osc.last_init_ms > 2000) {
                g_osc.last_init_ms = now;
                notify_state_change(APP_MS_STATE_CONNECTING);
                if (!g_osc.info_fetched) {
                    mg_http_connect(&g_osc.mgr, info_url, info_evt, NULL);
                }
                g_osc.udp_conn = mg_connect(&g_osc.mgr, g_osc.udp_url, udp_evt, NULL);
            }
        }
    }
    mg_mgr_free(&g_osc.mgr);
    return 0;
}

// ────────────────────────────────────────────────────────────────────────────
// ms_client_iface_t
// ────────────────────────────────────────────────────────────────────────────

static bool m_start(void) { return true; /* started by ms_client_real_osc_create */ }
static void m_stop(void) {
    g_osc.running = false;
    if (g_osc.thread) {
        SDL_WaitThread(g_osc.thread, NULL);
        g_osc.thread = NULL;
    }
}

static void m_set_level(int id, float level) {
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.lvl", id, g_osc.mix_idx);
    if (g_osc.level_fmt == APP_LEVEL_FORMAT_DB) {
        enqueue_set_val_db(dotted, app_position_to_db(level));
    } else {
        enqueue_set_norm(dotted, level);
    }
}

static void m_set_mute(int id, bool mute) {
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.on", id, g_osc.mix_idx);
    enqueue_set_bool(dotted, !mute);
}

static void m_set_name(int id, const char *name) {
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", id);
    enqueue_set_str(dotted, name ? name : "");
}

static void m_set_master_level(float level) {
    int mid = master_channel_id();
    if (mid < 0) return;
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.lvl", mid);
    enqueue_set_norm(dotted, level);
}

static void m_set_master_mute(bool mute) {
    int mid = master_channel_id();
    if (mid < 0) return;
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.on", mid);
    enqueue_set_bool(dotted, !mute);
}

static app_ms_state_t m_get_state(void) { return g_osc.state; }
static const char    *m_get_host (void) { return g_osc.host;  }
static int            m_get_port (void) { return g_osc.osc_port; }

static void m_register(app_ms_on_change_t cb, void *ctx) {
    if (g_osc.observer_count < MAX_OBSERVERS) {
        g_osc.observers[g_osc.observer_count].cb  = cb;
        g_osc.observers[g_osc.observer_count].ctx = ctx;
        g_osc.observer_count++;
    }
}

static int  m_get_mix(void)   { return g_osc.mix_idx; }
static void m_set_mix(int idx) {
    g_osc.mix_idx = idx;
    // Heartbeat-subscribe means we don't re-subscribe on mix change. The
    // next /con/n/<...lvl> for the new mix index lands automatically on
    // the next change OR via a fresh prime.
    g_osc.primed   = false;
    g_osc.prime_idx = 0;
}

static void m_set_mix_layout(int off, int cnt) {
    g_osc.mix_offset = off;
    g_osc.mix_count  = cnt;
    if (cnt > 0) g_osc.mix_list_received = true;
}

static const char *m_get_mix_name(int idx) {
    if (idx < 0 || idx >= g_osc.mix_count || idx >= MAX_MIX_BUSES) return NULL;
    if (g_osc.mix_names[idx][0] == 0) return NULL;
    return g_osc.mix_names[idx];
}

static bool m_is_mix_routed     (int idx)     { (void)idx; return true; }
static void m_fetch_mix_routing (void)        {}
static bool m_is_mix_list_ready (void)        { return g_osc.mix_list_received; }
static void m_resubscribe       (void)        { /* heartbeat covers all */ }
static void m_reconnect         (void)        {
    if (g_osc.udp_conn) g_osc.udp_conn->is_closing = 1;
}
static void m_fetch_all_strip_names(int total) { (void)total; }
static const char *m_get_strip_name(int id) {
    snprintf(g_osc_strip_name_buf, sizeof(g_osc_strip_name_buf), "CH %02d", id + 1);
    return g_osc_strip_name_buf;
}
static void m_fetch_channel_routability(int total) { (void)total; }
static bool m_is_channel_routable      (int id)    { (void)id; return true; }
static void m_set_meter_enabled(bool on)           { (void)on; }
static void m_set_level_format(app_level_format_t f) {
    if (g_osc.level_fmt == f) return;
    g_osc.level_fmt = f;
    // No re-subscribe needed -- handle_inbound filters by active format.
}

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

const ms_client_iface_t *ms_client_real_osc_create(const char *host, int http_port, int osc_port) {
    snprintf(g_osc.host, sizeof(g_osc.host), "%s", host);
    g_osc.port      = http_port;
    g_osc.osc_port  = osc_port;
    snprintf(g_osc.udp_url,   sizeof(g_osc.udp_url),   "udp://%s:%d", host, osc_port);
    snprintf(g_osc.http_base, sizeof(g_osc.http_base), "http://%s:%d", host, http_port);

    g_osc.outq_mtx  = SDL_CreateMutex();
    g_osc.running   = true;
    g_osc.state     = APP_MS_STATE_BOOT;
    g_osc.level_fmt = app_prefs_get_level_format();
    g_osc.thread    = SDL_CreateThread(worker_thread, "ms_real_osc", NULL);
    return &s_iface;
}
