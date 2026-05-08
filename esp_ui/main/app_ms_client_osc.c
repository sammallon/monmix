// Mongoose-backed Mixing Station client over OSC. Same iface and dispatch
// shape as app_ms_client_ws.c; reuses the same /console/information +
// /app/mixers/offline REST init via the same mg_mgr. Differences:
//
//   * SUBSCRIBE = heartbeat. We send /hi/n every 3 s and MS pushes ALL
//     parameter changes back to our UDP source. There is no per-path
//     subscribe on the wire (per MS docs); the firmware filters incoming
//     packets against the tracked channel set.
//   * SET = OSC packet. /con/n/<dotted> ,f <0..1> for norm; /con/v/<dotted>
//     ,f <units> for raw (dB). Bool fields ride ,f 0.0/1.0 (also accept
//     ,T/,F + ,i on receive in case MS varies).
//   * RECEIVE = same /con/[nv]/<dotted> ,f <value> packets land on our UDP
//     source port. Aliases (lvl/level/mix.sends.0.lvl) all push separately;
//     we accept the canonical one for each app_state slot.
//
// Boot init is identical to WS: HTTP /app/mixers/offline (best-effort
// attach) and HTTP /console/information (mix layout). After init, the WS
// connect step is replaced by binding a UDP socket and starting the
// heartbeat. Mongoose handles both UDP and HTTP in the same mgr, single
// task, same poll loop -- no extra event loop or worker thread.

#include "app_config.h"
#include "app_logd.h"
#include "app_ms_client.h"
#include "app_prefs.h"
#include "app_state.h"
#include "osc_expect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "mongoose.h"

#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"

static const char *TAG = "ms_osc";

#define MAX_MIX_BUSES        24
#define WORKER_STACK         8192
#define WORKER_PRIO          5
#define MGR_POLL_MS          25
#define HEARTBEAT_MS         3000
#define RECONNECT_RETRY_MS   2000

// Inbound ring depth for OSC packets that pass the udp_recv filter and
// need worker processing. 32 is enough headroom that even a brief 1500-
// msg/sec burst (e.g. scene recall) doesn't overflow before the worker
// drains, while staying small enough that pbuf-pool pressure is bounded.
#define OSC_RX_QUEUE_DEPTH   32

// Per-path expectation tracker. Both prime resilience and MS-down
// detection (watchdog) ride osc_expect.{h,c}; this header just sizes
// the slot array and the timing constants.
#define OSC_EXPECT_SIZE          64
#define OSC_EXPECT_TIMEOUT_MS    1500
#define OSC_WATCHDOG_INTERVAL_MS 10000   // probe every 10 s when otherwise quiet
#define OSC_WATCHDOG_RETRIES     2       // 3 sends total over 4.5 s

// Largest OSC packet we accept inbound. Real MS frames are typically
// 30-60 bytes (/con/n/<dotted> ,f <float>); 192 covers any reasonable
// path length on Si Expression-class boards plus a string arg.
#define OSC_RX_MAX_PKT       192

// Tracked-channel bitmap. Each bit index = MS channel id. Set by the
// worker when channel selection changes; read by the udp_recv filter
// (TCP/IP task ctx) on every inbound packet. Atomic word writes are
// safe enough for a hint-style filter -- a bit briefly out of sync just
// drops or admits one packet, which is harmless.
#define OSC_FILTER_BITMAP_WORDS 8         // 256 ids
typedef struct {
    uint32_t bits[OSC_FILTER_BITMAP_WORDS];
} osc_id_bitmap_t;

typedef struct {
    uint16_t len;
    uint8_t  pkt[OSC_RX_MAX_PKT];
} osc_rx_entry_t;

// ────────────────────────────────────────────────────────────────────────────
// OSC packet helpers
// ────────────────────────────────────────────────────────────────────────────

// OSC strings: NUL-terminate, then pad with NULs to a 4-byte boundary.
static size_t osc_padded(size_t bytes_incl_nul) {
    return (bytes_incl_nul + 3u) & ~3u;
}

// Build an OSC packet for `<addr> ,<types> <args...>`. Only ,f and ,
// (no-arg) are emitted; we never need any other type on the send side.
// Returns total packet length, or 0 on overflow.
static size_t osc_build(uint8_t *out, size_t out_size,
                        const char *addr, const char *types,
                        const float *fargs, size_t nargs) {
    size_t alen   = strlen(addr) + 1;            // include NUL
    size_t apad   = osc_padded(alen);
    size_t tcount = (types ? strlen(types) : 0); // chars after the comma
    size_t tlen   = 1 + tcount + 1;              // ',' + tags + NUL
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

// Inbound parse: split the packet into address, type-tag string, and the
// raw arg-bytes pointer. Returns false on malformed input. Does NOT decode
// individual args -- the dispatch layer handles ,f / ,i / ,T / ,F since
// MS may use any of these for bool fields.
static bool osc_split(const uint8_t *buf, size_t len,
                      const char **out_addr,
                      const char **out_types,
                      const uint8_t **out_args, size_t *out_args_len) {
    if (len < 8) return false;
    // Address: NUL-terminated within first segment.
    size_t alen = strnlen((const char *)buf, len);
    if (alen == len) return false;
    size_t apad = osc_padded(alen + 1);
    if (apad >= len) return false;
    if (buf[apad] != ',') return false;
    size_t tlen = strnlen((const char *)(buf + apad), len - apad);
    if (apad + tlen >= len) return false;
    size_t tpad = apad + osc_padded(tlen + 1);
    if (tpad > len) return false;
    *out_addr      = (const char *)buf;
    *out_types     = (const char *)(buf + apad + 1);  // skip leading ','
    *out_args      = buf + tpad;
    *out_args_len  = len - tpad;
    return true;
}

// Decode the first scalar arg as a generic number (float / int / bool).
// Returns true if recognized; the caller decides how to interpret the
// number based on the path suffix.
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
        *out_f       = v;
        *out_is_bool = false;
        return true;
    }
    if (t == 'i') {
        int32_t iv = (int32_t)bits;
        *out_f       = (float)iv;
        *out_is_bool = false;
        return true;
    }
    return false;
}

// ────────────────────────────────────────────────────────────────────────────
// State (mirrors app_ms_client_ws.c shape so the iface methods feel the same)
// ────────────────────────────────────────────────────────────────────────────

typedef struct outq_entry {
    uint8_t            *pkt;
    size_t              len;
    struct outq_entry  *next;
} outq_entry_t;

static struct {
    SemaphoreHandle_t       outq_mtx;
    outq_entry_t           *outq_head;
    outq_entry_t           *outq_tail;

    TaskHandle_t            task;
    volatile bool           running;
    // Bounded join: see app_ms_client_ws.c::ws_stop. Picker live-apply
    // restarts the worker immediately after stopping it; without the
    // join the second osc_task races the dying one on the global g.
    SemaphoreHandle_t       exit_sig;

    // Mongoose owns only the boot-time HTTP fetches now; the OSC pipe is
    // raw udp_pcb so the udp_recv callback can drop irrelevant packets in
    // TCP/IP-task context before they hit any per-socket recv mbox.
    struct mg_mgr           mgr;

    // Raw LwIP UDP PCB. udp_bind to ephemeral local port, udp_connect to
    // MS:osc_port so udp_send uses the bound (host:port) pair. udp_recv
    // callback fires for every datagram received on the PCB; the filter
    // there decides keep-or-drop without touching the recv mbox.
    struct udp_pcb         *pcb;
    bool                    pcb_open;
    bool                    pcb_remote_ok;     // udp_connect succeeded
    ip_addr_t               remote_ip;
    uint16_t                remote_port;

    // Inbound ring: udp_recv (TCP/IP task) pushes; worker drains. xQueue
    // is thread-safe across both.
    QueueHandle_t           rx_q;
    uint64_t                rx_kept;
    uint64_t                rx_dropped;

    // Filter bitmap of tracked MS channel ids. Read by the recv callback,
    // written by the worker when the picker / mix layout changes.
    osc_id_bitmap_t         filter;

    // Expectation table -- worker-only access (handle_inbound runs on the
    // worker via drain_rx, so no mutex needed).
    osc_expect_t            expect;
    osc_expect_slot_t       expect_slots[OSC_EXPECT_SIZE];
    uint32_t                last_watchdog_ms;
    uint32_t                last_expect_tick_ms;

    uint32_t                last_heartbeat_ms;
    uint32_t                last_init_ms;
    bool                    info_fetched;
    bool                    primed;
    int                     prime_idx;
    int                     prime_total;

    char                    udp_host[80];
    int                     udp_port;
    char                    http_base[128];
    char                    info_url[160];
    char                    offline_url[160];

    app_ms_state_t          state;
    app_level_format_t      level_format;
    int                     mix_idx;
    int                     mix_offset;
    int                     mix_count;
    char                    mix_names[MAX_MIX_BUSES][32];
    bool                    mix_list_received;

    struct {
        app_ms_on_change_t cb;
        void              *ctx;
    } subscribers[4];
    size_t                  subscriber_count;
} g;

// ────────────────────────────────────────────────────────────────────────────
// Filter bitmap (read in TCP/IP task ctx, written by worker)
// ────────────────────────────────────────────────────────────────────────────

static inline bool filter_test(int id) {
    if (id < 0 || id >= (int)(OSC_FILTER_BITMAP_WORDS * 32)) return false;
    return (g.filter.bits[id >> 5] & (1u << (id & 31))) != 0;
}

static inline void filter_set(int id) {
    if (id < 0 || id >= (int)(OSC_FILTER_BITMAP_WORDS * 32)) return;
    g.filter.bits[id >> 5] |= (1u << (id & 31));
}

// Forward decl: expect retry callback emits via udp_send_bytes, which
// is defined further down the file with the rest of the LwIP-raw send
// path.
static err_t udp_send_bytes(const uint8_t *bytes, size_t len);

// Retry callback -- osc_expect_tick invokes this for any slot whose
// timeout fires while retries_left > 0. We rebuild the OSC packet from
// the slot's path + fmt and emit it via the same direct path the
// initial GET used.
static void osc_expect_retry_send(const char *path, char fmt, void *user) {
    (void)user;
    char addr[80];
    snprintf(addr, sizeof(addr), "/con/%c/%s", fmt, path);
    uint8_t pkt[128];
    size_t n = osc_build(pkt, sizeof(pkt), addr, NULL, NULL, 0);
    if (n) udp_send_bytes(pkt, n);
}

// Worker-side helpers that wrap the module API with our esp_timer clock,
// kept thin so the OSC client reads top-to-bottom in one mental model.
static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}
static inline bool expect_register(const char *path, char fmt, uint8_t retries, uint8_t flags) {
    return osc_expect_register(&g.expect, path, fmt, retries, flags, now_ms());
}
static inline void expect_match(const char *dotted) {
    osc_expect_match(&g.expect, dotted);
}
static inline int expect_tick(void) {
    return osc_expect_tick(&g.expect, now_ms());
}

// Recompute the bitmap from app_state's tracked channels + master + mix
// bus range. Called by the worker whenever the layout could have changed
// (set_mix, set_mix_layout, resubscribe).
static void filter_rebuild(void) {
    osc_id_bitmap_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    size_t n = app_state_count();
    for (size_t i = 0; i < n; ++i) {
        int id = app_state_id_for_idx(i);
        if (id >= 0 && id < (int)(OSC_FILTER_BITMAP_WORDS * 32)) {
            fresh.bits[id >> 5] |= (1u << (id & 31));
        }
    }
    if (g.mix_count > 0) {
        for (int i = 0; i < g.mix_count; ++i) {
            int id = g.mix_offset + i;
            if (id >= 0 && id < (int)(OSC_FILTER_BITMAP_WORDS * 32)) {
                fresh.bits[id >> 5] |= (1u << (id & 31));
            }
        }
    }
    // Master id is the active mix bus (offset + mix_idx).
    if (g.mix_count > 0 && g.mix_idx >= 0 && g.mix_idx < g.mix_count) {
        int mid = g.mix_offset + g.mix_idx;
        if (mid >= 0 && mid < (int)(OSC_FILTER_BITMAP_WORDS * 32)) {
            fresh.bits[mid >> 5] |= (1u << (mid & 31));
        }
    }
    // Single word-store per slot. Atomic enough for filter-hint use; a
    // brief stale read drops or admits one packet, no correctness risk.
    for (int i = 0; i < OSC_FILTER_BITMAP_WORDS; ++i) g.filter.bits[i] = fresh.bits[i];
}

// ────────────────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────────────────

static void notify_subscribers(void) {
    for (size_t i = 0; i < g.subscriber_count; ++i) {
        if (g.subscribers[i].cb) g.subscribers[i].cb(g.subscribers[i].ctx);
    }
}

static void set_state(app_ms_state_t s) {
    if (g.state == s) return;
    g.state = s;
    notify_subscribers();
}

// Outbound queue -- non-mongoose tasks (LVGL / console) push raw OSC bytes
// here; the worker pops + sends once the UDP "connection" is open.
static void outq_push(uint8_t *pkt, size_t len) {
    outq_entry_t *e = (outq_entry_t *)malloc(sizeof(outq_entry_t));
    if (!e) { free(pkt); return; }
    e->pkt = pkt; e->len = len; e->next = NULL;
    xSemaphoreTake(g.outq_mtx, portMAX_DELAY);
    if (g.outq_tail) g.outq_tail->next = e; else g.outq_head = e;
    g.outq_tail = e;
    xSemaphoreGive(g.outq_mtx);
}

static outq_entry_t *outq_drain(void) {
    xSemaphoreTake(g.outq_mtx, portMAX_DELAY);
    outq_entry_t *e = g.outq_head;
    g.outq_head = NULL;
    g.outq_tail = NULL;
    xSemaphoreGive(g.outq_mtx);
    return e;
}

static int master_channel_id(void) {
    if (g.mix_count <= 0) return -1;
    if (g.mix_idx < 0 || g.mix_idx >= g.mix_count) return -1;
    return g.mix_offset + g.mix_idx;
}

static void sync_master_id_to_app_state(void) {
    app_state_master_set_id(master_channel_id());
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

// MS's OSC bridge takes a string arg for cfg.name. ,s is OSC's string type.
static void enqueue_set_str(const char *dotted, const char *s) {
    char addr[100];
    snprintf(addr, sizeof(addr), "/con/v/%s", dotted);
    size_t alen = strlen(addr) + 1;
    size_t apad = osc_padded(alen);
    size_t tlen = 3;            // ",s\0"
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

// ────────────────────────────────────────────────────────────────────────────
// Inbound dispatch (mirrors handle_broadcast in app_ms_client_ws.c)
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

// Apply one /con/[nv]/<dotted> ,f <value> update. The OSC bridge pushes
// every alias separately (e.g. levelData.0.lvl AND levelData.0.level AND
// mix.sends.0.lvl all fire on a single fader move) -- we only act on the
// canonical levelData.<mix>.* entries to avoid 3x the LVGL work.
static void handle_inbound(const char *addr, const char *types,
                           const uint8_t *args, size_t args_len) {
    char fmt;
    const char *dotted;
    if (strncmp(addr, "/con/n/", 7) == 0) { fmt = 'n'; dotted = addr + 7; }
    else if (strncmp(addr, "/con/v/", 7) == 0) { fmt = 'v'; dotted = addr + 7; }
    else return;

    // Any inbound /con/[nv]/<path> clears a matching expectation. Acts as
    // both fulfilment (prime GET reply) and liveness signal (watchdog
    // probe reply, or a coincidental change broadcast that proves MS is
    // alive on this path).
    expect_match(dotted);

    int id = -1;
    const char *suf = parse_ch_path(dotted, &id);
    if (!suf) return;

    int  idx       = app_state_idx_for_id(id);
    bool is_master = (id == master_channel_id());

    // cfg.name -- strings ride ,s. Use the existing OSC-string-after-tags
    // pointer convention.
    if (strcmp(suf, "cfg.name") == 0 && types && types[0] == 's') {
        const char *name = (const char *)args;
        if (idx >= 0)        app_state_set_name(idx, name, true);
        else if (is_master)  app_state_master_set_name(name, true);
        if (g.mix_count > 0 && id >= g.mix_offset && id < g.mix_offset + g.mix_count) {
            int slot = id - g.mix_offset;
            strncpy(g.mix_names[slot], name, sizeof(g.mix_names[slot]) - 1);
            g.mix_names[slot][sizeof(g.mix_names[slot]) - 1] = 0;
            notify_subscribers();
        }
        return;
    }

    float vf;
    bool vbool;
    if (!osc_first_scalar(types, args, args_len, &vf, &vbool)) return;

    char level_prefix[32];
    snprintf(level_prefix, sizeof(level_prefix), "levelData.%d.", g.mix_idx);
    size_t plen = strlen(level_prefix);
    if (strncmp(suf, level_prefix, plen) == 0) {
        const char *tail = suf + plen;
        if (idx >= 0 && strcmp(tail, "lvl") == 0) {
            // norm vs val differs by /con/n vs /con/v address. We accept
            // whatever matches the active level format and ignore the
            // other -- saves a redundant LVGL update per pair.
            if (fmt == 'v' && g.level_format == APP_LEVEL_FORMAT_DB) {
                app_state_set_level_db(idx, vf, true);
            } else if (fmt == 'n' && g.level_format != APP_LEVEL_FORMAT_DB) {
                app_state_set_level(idx, vf, true);
            }
        } else if (idx >= 0 && strcmp(tail, "on") == 0) {
            // MS "on" = audible = NOT muted.
            bool on = vbool ? (vf > 0.5f) : (vf > 0.5f);
            app_state_set_mute(idx, !on, true);
        }
    } else if (is_master) {
        if (strcmp(suf, "mix.lvl") == 0) {
            if (fmt == 'v' && g.level_format == APP_LEVEL_FORMAT_DB) {
                app_state_master_set_level_db(vf, true);
            } else if (fmt == 'n' && g.level_format != APP_LEVEL_FORMAT_DB) {
                app_state_master_set_level(vf, true);
            }
        } else if (strcmp(suf, "mix.on") == 0) {
            app_state_master_set_mute(!(vf > 0.5f), true);
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// HTTP init (clones app_ms_client_ws.c -- different mgr, same wire pattern)
// ────────────────────────────────────────────────────────────────────────────

static void info_evt(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
    (void)fn_data;
    if (ev == MG_EV_CONNECT) {
        mg_printf(c, "GET /console/information HTTP/1.0\r\nHost: %s\r\n\r\n",
                  app_config_ms_host());
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        char *body = (char *)malloc(hm->body.len + 1);
        if (body) {
            memcpy(body, hm->body.ptr, hm->body.len);
            body[hm->body.len] = 0;
            cJSON *root = cJSON_Parse(body);
            free(body);
            if (root) {
                cJSON *jts = cJSON_GetObjectItem(root, "channelTypes");
                if (cJSON_IsArray(jts)) {
                    cJSON *t;
                    cJSON_ArrayForEach(t, jts) {
                        cJSON *jname = cJSON_GetObjectItem(t, "name");
                        cJSON *joff  = cJSON_GetObjectItem(t, "offset");
                        cJSON *jcnt  = cJSON_GetObjectItem(t, "count");
                        if (cJSON_IsString(jname) && strcmp(jname->valuestring, "Mix") == 0 &&
                            cJSON_IsNumber(joff) && cJSON_IsNumber(jcnt)) {
                            g.mix_offset        = (int)joff->valuedouble;
                            g.mix_count         = (int)jcnt->valuedouble;
                            g.mix_list_received = true;
                            g.info_fetched      = true;
                            ESP_LOGI(TAG, "mix layout: offset=%d count=%d",
                                     g.mix_offset, g.mix_count);
                            notify_subscribers();
                        }
                    }
                }
                cJSON_Delete(root);
            }
        }
        c->is_draining = 1;
    } else if (ev == MG_EV_ERROR) {
        ESP_LOGW(TAG, "info GET error: %s", (const char *)ev_data);
    }
}

static void offline_evt(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
    (void)fn_data;
    if (ev == MG_EV_CONNECT) {
        const char *body = "{\"consoleId\":3,\"modelId\":1,\"model\":\"Si Expression\"}";
        mg_printf(c,
                  "POST /app/mixers/offline HTTP/1.0\r\n"
                  "Host: %s\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: %u\r\n\r\n%s",
                  app_config_ms_host(),
                  (unsigned)strlen(body), body);
    } else if (ev == MG_EV_HTTP_MSG) {
        c->is_draining = 1;
    } else if (ev == MG_EV_ERROR) {
        ESP_LOGD(TAG, "offline-attach error: %s", (const char *)ev_data);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// LwIP raw udp_recv callback. Runs in TCP/IP task context for every
// datagram delivered to our PCB -- before any per-socket recv mbox is
// involved. Cheap memcmps + a bitmap lookup decide keep vs. drop; kept
// packets get COPIED into the ring (ownership of the pbuf can't cross
// into the worker without holding the core lock the whole way) and the
// worker dispatches in its own context.
//
// Constraints: must be tight. No malloc, no logging, no ESP_LOG (which
// allocates), no app_state calls. This task has a small stack.
// ────────────────────────────────────────────────────────────────────────────

static void udp_filter_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *src, u16_t src_port) {
    (void)arg; (void)pcb; (void)src; (void)src_port;
    if (!p) return;

    if (p->tot_len < 8 || p->tot_len > OSC_RX_MAX_PKT) {
        g.rx_dropped++;
        pbuf_free(p);
        return;
    }

    // Pull the first ~12 bytes contiguously so memcmp + the channel-id
    // parse can run without walking pbuf chains. For our payload sizes a
    // single pbuf is the common case anyway, but be defensive.
    uint8_t head[16];
    uint16_t head_len = p->tot_len < sizeof(head) ? p->tot_len : (uint16_t)sizeof(head);
    if (pbuf_copy_partial(p, head, head_len, 0) != head_len) {
        g.rx_dropped++;
        pbuf_free(p);
        return;
    }

    // Cheapest reject: must be /con/[nv]/ch.<n>. -- everything else from
    // the OSC firehose (fx/dca/cfg.* outside cfg.name/route.* etc) dies
    // here without touching the worker queue.
    if (head_len < 12) { g.rx_dropped++; pbuf_free(p); return; }
    if (memcmp(head, "/con/", 5) != 0) { g.rx_dropped++; pbuf_free(p); return; }
    if (head[5] != 'n' && head[5] != 'v') { g.rx_dropped++; pbuf_free(p); return; }
    if (head[6] != '/') { g.rx_dropped++; pbuf_free(p); return; }
    if (memcmp(head + 7, "ch.", 3) != 0) { g.rx_dropped++; pbuf_free(p); return; }

    // Parse channel id from "ch.<digits>." -- bounded to 4 digits, which
    // covers any console (max ~80 channels seen in MS).
    int id = 0;
    int idx = 10;
    while (idx < head_len && head[idx] >= '0' && head[idx] <= '9' && idx < 14) {
        id = id * 10 + (head[idx] - '0');
        ++idx;
    }
    if (idx == 10 || idx >= head_len || head[idx] != '.') {
        g.rx_dropped++; pbuf_free(p); return;
    }
    if (!filter_test(id)) {
        g.rx_dropped++; pbuf_free(p); return;
    }

    // Kept. Copy into a queue entry and push.
    osc_rx_entry_t entry;
    entry.len = p->tot_len;
    if (pbuf_copy_partial(p, entry.pkt, entry.len, 0) != entry.len) {
        g.rx_dropped++; pbuf_free(p); return;
    }
    pbuf_free(p);
    // udp_recv runs in the TCP/IP task (a regular FreeRTOS task), not an
    // ISR. xQueueSend with 0 timeout is the right call -- if the worker
    // is behind, drop rather than block the network stack.
    if (xQueueSend(g.rx_q, &entry, 0) == pdTRUE) {
        g.rx_kept++;
    } else {
        g.rx_dropped++;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Worker task
// ────────────────────────────────────────────────────────────────────────────

static void compose_urls(void) {
    snprintf(g.udp_host,    sizeof(g.udp_host),    "%s", app_config_ms_host());
    g.udp_port = (int)app_config_ms_osc_port();
    snprintf(g.http_base,   sizeof(g.http_base),   "http://%s:%u", app_config_ms_host(), (unsigned)app_config_ms_port());
    snprintf(g.info_url,    sizeof(g.info_url),    "%s/console/information",  g.http_base);
    snprintf(g.offline_url, sizeof(g.offline_url), "%s/app/mixers/offline",   g.http_base);
}

// Raw LwIP send. Use udp_sendto rather than udp_connect+udp_send so the
// PCB stays unfiltered on inbound -- otherwise LwIP's UDP_FLAGS_CONNECTED
// filter rejects any reply whose source addr/port doesn't match the
// connected pair byte-for-byte (and we've seen that drop legitimate
// replies in the wild).
static err_t udp_send_bytes(const uint8_t *bytes, size_t len) {
    if (!g.pcb || !g.pcb_remote_ok) return ERR_CONN;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) return ERR_MEM;
    memcpy(p->payload, bytes, len);
    LOCK_TCPIP_CORE();
    err_t e = udp_sendto(g.pcb, p, &g.remote_ip, g.remote_port);
    UNLOCK_TCPIP_CORE();
    pbuf_free(p);
    return e;
}

static void send_heartbeat(void) {
    if (!g.pcb_open) return;
    // The trailing char selects the broadcast flavor MS will push at us:
    // /hi/n -> /con/n broadcasts, /hi/v -> /con/v. handle_inbound filters
    // by active level_format and would discard the wrong-flavor stream;
    // the tablet must subscribe in the same flavor it expects to receive.
    const char *path = (g.level_format == APP_LEVEL_FORMAT_DB) ? "/hi/v" : "/hi/n";
    uint8_t pkt[16];
    size_t n = osc_build(pkt, sizeof(pkt), path, NULL, NULL, 0);
    if (n) {
        err_t e = udp_send_bytes(pkt, n);
        if (e != ERR_OK) ESP_LOGW(TAG, "heartbeat send err=%d", e);
    }
}

// Bring up the raw UDP PCB. Resolves the host (dotted-IP first; falls back
// to getaddrinfo so hostnames like "mixingstation.local" work). Then binds
// an ephemeral local port and registers the recv callback. Returns true
// on success; the connection state is reflected in g.pcb_open.
static bool udp_pcb_up(void) {
    if (g.pcb) return g.pcb_open;
    if (!ipaddr_aton(g.udp_host, &g.remote_ip)) {
        // Hostname -- resolve via lwIP's getaddrinfo. Synchronous; uses
        // the DHCP-supplied DNS server configured at WiFi association.
        // Block here is acceptable -- this only runs on connect/reconnect,
        // not on the hot path.
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
        struct addrinfo *res = NULL;
        int rc = getaddrinfo(g.udp_host, NULL, &hints, &res);
        if (rc != 0 || !res) {
            ESP_LOGW(TAG, "udp: DNS lookup '%s' failed (rc=%d)", g.udp_host, rc);
            if (res) freeaddrinfo(res);
            return false;
        }
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        ip_addr_set_ip4_u32(&g.remote_ip, sin->sin_addr.s_addr);
        uint32_t a = ntohl(sin->sin_addr.s_addr);
        ESP_LOGI(TAG, "udp: '%s' -> %u.%u.%u.%u",
                 g.udp_host,
                 (unsigned)((a >> 24) & 0xff), (unsigned)((a >> 16) & 0xff),
                 (unsigned)((a >> 8)  & 0xff), (unsigned)(a & 0xff));
        freeaddrinfo(res);
    }
    g.remote_port = (uint16_t)g.udp_port;

    LOCK_TCPIP_CORE();
    g.pcb = udp_new();
    if (!g.pcb) { UNLOCK_TCPIP_CORE(); return false; }

    err_t e = udp_bind(g.pcb, IP_ANY_TYPE, 0);  // ephemeral local
    if (e != ERR_OK) {
        udp_remove(g.pcb);
        g.pcb = NULL;
        UNLOCK_TCPIP_CORE();
        ESP_LOGW(TAG, "udp_bind err=%d", e);
        return false;
    }
    udp_recv(g.pcb, udp_filter_recv, NULL);
    UNLOCK_TCPIP_CORE();

    g.pcb_open      = true;
    g.pcb_remote_ok = true;
    ESP_LOGI(TAG, "udp pcb up: %s:%d", g.udp_host, g.udp_port);
    set_state(APP_MS_STATE_CONNECTED);
    return true;
}

static void udp_pcb_down(void) {
    if (!g.pcb) return;
    LOCK_TCPIP_CORE();
    udp_recv(g.pcb, NULL, NULL);
    udp_remove(g.pcb);
    UNLOCK_TCPIP_CORE();
    g.pcb           = NULL;
    g.pcb_open      = false;
    g.pcb_remote_ok = false;
    g.primed        = false;
    g.prime_idx     = 0;
    osc_expect_clear(&g.expect);
    set_state(APP_MS_STATE_DISCONNECTED);
}

// Per docs, "A OSC packet without any parameters is used to request the
// current value" -- MS replies on the sender's source port with the same
// /con/[nv]/<path> ,<type> <value> envelope used for change broadcasts.
// Heartbeat-subscribe alone never delivers the initial state of unchanged
// parameters, so on cold start the UI shows "ch 1" / "Mix 1" placeholders
// and 0 levels until something happens to change. We GET the cfg.name for
// every tracked channel, the master, and every mix bus once the layout is
// known. Levels/mute are intentionally left to the next change broadcast
// (parity with the WS path's level subscribe -- the slider visual being
// briefly at 0 is less surprising than the wrong scribble-strip name).
//
// Sends are paced one per poll iteration. Bursting all 27 in a single
// drain overflowed LwIP's UDP recv queue (default 6 deep) before mongoose
// could service MG_EV_READ at the 25 ms poll cadence -- ~half the replies
// were dropped. Spreading them across polls lets each reply land before
// the next request goes out.
static void enqueue_get(const char *dotted, char fmt /* 'n' or 'v' */) {
    char addr[100];
    snprintf(addr, sizeof(addr), "/con/%c/%s", fmt, dotted);
    uint8_t *pkt = (uint8_t *)malloc(128);
    if (!pkt) return;
    size_t n = osc_build(pkt, 128, addr, NULL, NULL, 0);
    if (!n) { free(pkt); return; }
    // Track for retry. Prime GETs use 2 retries -- enough to absorb the
    // typical UDP-loss rate observed on hardware (initial probe showed
    // ~5/12 misses on a single burst; pacing brought that to a few per
    // 50, retry brings it to ~zero).
    expect_register(dotted, fmt, 2, 0);
    outq_push(pkt, n);
}

// One prime path per call; caller drives via prime_idx in the worker loop.
// Returns false when nothing remains to enqueue.
//
// Layout per tracked channel: cfg.name, levelData.<m>.lvl, levelData.<m>.on
// Layout for master (if present): cfg.name, mix.lvl, mix.on
// Layout for each mix bus: cfg.name
//
// Level paths use /con/v/ in DB mode and /con/n/ in NORM mode -- handle_inbound
// only applies the format that matches the active level_format pref, and the
// other would just be discarded if MS replied to both.
static bool prime_step(void) {
    int idx       = g.prime_idx;
    int tracked   = (int)app_state_count();
    int mid       = master_channel_id();
    int has_master = (mid >= 0) ? 1 : 0;
    int mixes     = (g.mix_count < MAX_MIX_BUSES ? g.mix_count : MAX_MIX_BUSES);
    char fmt      = (g.level_format == APP_LEVEL_FORMAT_DB) ? 'v' : 'n';

    const int per_ch       = 3;
    int track_total        = tracked * per_ch;
    int master_total       = has_master * 3;
    int total              = track_total + master_total + mixes;
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
                snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.lvl", id, g.mix_idx);
                enqueue_get(dotted, fmt);
                break;
            case 2:
                snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.on", id, g.mix_idx);
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
        snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", g.mix_offset + slot);
        enqueue_get(dotted, 'n');
    }
    g.prime_idx++;
    if (g.prime_idx >= total) {
        g.prime_total = total;
        g.primed      = true;
        return false;
    }
    return true;
}

// Drain inbound queue. Each entry was already validated by the recv
// filter (address pattern + tracked-channel bitmap), so dispatch is
// straight to handle_inbound -- no further bouncer.
static void drain_rx(void) {
    osc_rx_entry_t entry;
    int max = 16;  // bound work per poll so we don't starve outbound
    while (max-- > 0 && xQueueReceive(g.rx_q, &entry, 0) == pdTRUE) {
        const char *addr; const char *types;
        const uint8_t *args; size_t args_len;
        if (osc_split(entry.pkt, entry.len, &addr, &types, &args, &args_len)) {
            handle_inbound(addr, types, args, args_len);
        }
    }
}

static void osc_task(void *unused) {
    (void)unused;
    mg_mgr_init(&g.mgr);
    set_state(APP_MS_STATE_CONNECTING);

    compose_urls();

    // Boot HTTP fetches: offline-attach (best-effort) and /console/information
    // (source of truth for mix layout). Mongoose handles HTTP only now.
    mg_http_connect(&g.mgr, g.offline_url, offline_evt, NULL);
    mg_http_connect(&g.mgr, g.info_url,    info_evt,    NULL);

    udp_pcb_up();

    while (g.running) {
        mg_mgr_poll(&g.mgr, MGR_POLL_MS);

        // Drain inbound first so the queue doesn't sit full while we send.
        drain_rx();

        if (g.pcb_open) {
            outq_entry_t *e = outq_drain();
            while (e) {
                udp_send_bytes(e->pkt, e->len);
                outq_entry_t *next = e->next;
                free(e->pkt);
                free(e);
                e = next;
            }
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (g.pcb_open && now - g.last_heartbeat_ms > HEARTBEAT_MS) {
            send_heartbeat();
            g.last_heartbeat_ms = now;
        }

        // Name + level prime, one path per poll iteration. Pacing keeps
        // MS replies arriving at a rate the recv pipeline can absorb
        // (also helps when the LwIP filter is in early state and the
        // bitmap is still being seeded).
        if (g.pcb_open && !g.primed && g.mix_count > 0) {
            // Make sure the filter bitmap is current before priming so
            // replies for tracked channels aren't dropped at the filter.
            filter_rebuild();
            prime_step();
        }

        // Periodic watchdog: register a GET expectation for ch.0.cfg.name
        // every 10 s. expect_tick will retry up to OSC_WATCHDOG_RETRIES
        // and then fail-flag the slot, which we catch below.
        if (g.pcb_open && g.primed && now - g.last_watchdog_ms > OSC_WATCHDOG_INTERVAL_MS) {
            g.last_watchdog_ms = now;
            int probe_id = (app_state_count() > 0) ? app_state_id_for_idx(0) : 0;
            if (probe_id < 0) probe_id = 0;
            char path[OSC_EXPECT_PATH_MAX];
            snprintf(path, sizeof(path), "ch.%d.cfg.name", probe_id);
            if (expect_register(path, 'n', OSC_WATCHDOG_RETRIES, OSC_EXPECT_FLAG_WATCHDOG)) {
                char addr[80];
                snprintf(addr, sizeof(addr), "/con/n/%s", path);
                uint8_t pkt[128];
                size_t n = osc_build(pkt, sizeof(pkt), addr, NULL, NULL, 0);
                if (n) udp_send_bytes(pkt, n);
            }
        }

        // Sweep expectations every ~250 ms. Cheaper to skip ticks than to
        // run the table walk on every 25 ms poll.
        if (now - g.last_expect_tick_ms > 250) {
            g.last_expect_tick_ms = now;
            int wd_failed = expect_tick();
            if (wd_failed > 0 && g.pcb_open) {
                ESP_LOGW(TAG, "watchdog: %d failure(s) -- forcing reconnect", wd_failed);
                udp_pcb_down();
            }
        }

        if (!g.pcb_open) {
            if (now - g.last_init_ms > RECONNECT_RETRY_MS) {
                g.last_init_ms = now;
                set_state(APP_MS_STATE_CONNECTING);
                if (!g.info_fetched) {
                    mg_http_connect(&g.mgr, g.info_url, info_evt, NULL);
                }
                udp_pcb_up();
            }
        }
    }

    udp_pcb_down();
    mg_mgr_free(&g.mgr);
    if (g.exit_sig) xSemaphoreGive(g.exit_sig);
    vTaskDelete(NULL);
}

// ────────────────────────────────────────────────────────────────────────────
// ms_client_iface_t implementations
// ────────────────────────────────────────────────────────────────────────────

static bool osc_start(void) {
    if (g.task) return true;
    g.outq_mtx = xSemaphoreCreateMutex();
    g.exit_sig = xSemaphoreCreateBinary();
    g.rx_q     = xQueueCreate(OSC_RX_QUEUE_DEPTH, sizeof(osc_rx_entry_t));
    if (!g.rx_q) {
        ESP_LOGE(TAG, "rx queue create failed");
        return false;
    }
    // Reset prime/info state so a restart re-fetches /console/information
    // and re-primes against the (potentially new) tracked-channel list.
    g.info_fetched = false;
    g.primed       = false;
    g.prime_idx    = 0;
    osc_expect_init(&g.expect, g.expect_slots, OSC_EXPECT_SIZE,
                    OSC_EXPECT_TIMEOUT_MS, osc_expect_retry_send, NULL);
    g.running  = true;
    g.state    = APP_MS_STATE_BOOT;
    g.level_format = app_prefs_get_level_format();
    if (xTaskCreatePinnedToCore(osc_task, "ms_osc", WORKER_STACK, NULL, WORKER_PRIO, &g.task, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return false;
    }
    return true;
}

static void osc_stop(void) {
    g.running = false;
    // Bounded join. Same rationale as ws_stop: the picker live-apply
    // path restarts the worker immediately, and the new task can't be
    // allowed to race the old one on the global g (mgr, queues, pcb).
    if (g.exit_sig) {
        if (xSemaphoreTake(g.exit_sig, pdMS_TO_TICKS(1500)) != pdTRUE) {
            ESP_LOGW(TAG, "osc_stop: worker join timed out");
        }
        vSemaphoreDelete(g.exit_sig);
        g.exit_sig = NULL;
    }
    g.task = NULL;
    // Drain + free outq, drop the mutex. Anything still queued was for
    // the prior session and would emit on the wrong filter/level.
    if (g.outq_mtx) {
        outq_entry_t *e = outq_drain();
        while (e) {
            outq_entry_t *next = e->next;
            free(e->pkt);
            free(e);
            e = next;
        }
        vSemaphoreDelete(g.outq_mtx);
        g.outq_mtx = NULL;
    }
    if (g.rx_q) {
        vQueueDelete(g.rx_q);
        g.rx_q = NULL;
    }
    set_state(APP_MS_STATE_BOOT);
}

static void osc_set_level(int id, float level) {
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.lvl", id, g.mix_idx);
    if (g.level_format == APP_LEVEL_FORMAT_DB) {
        enqueue_set_val_db(dotted, app_position_to_db(level));
    } else {
        enqueue_set_norm(dotted, level);
    }
}

static void osc_set_mute(int id, bool mute) {
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.on", id, g.mix_idx);
    enqueue_set_bool(dotted, !mute);
}

static void osc_set_name(int id, const char *name) {
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", id);
    enqueue_set_str(dotted, name);
}

static void osc_set_master_level(float level) {
    int mid = master_channel_id();
    if (mid < 0) return;
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.lvl", mid);
    enqueue_set_norm(dotted, level);
}

static void osc_set_master_mute(bool mute) {
    int mid = master_channel_id();
    if (mid < 0) return;
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.on", mid);
    enqueue_set_bool(dotted, !mute);
}

static app_ms_state_t osc_get_state(void) { return g.state; }
static const char    *osc_get_host (void) { return app_config_ms_host(); }
static int            osc_get_port (void) { return (int)app_config_ms_osc_port(); }

static void osc_register_on_change(app_ms_on_change_t cb, void *ctx) {
    if (!cb) return;
    if (g.subscriber_count >= sizeof(g.subscribers) / sizeof(g.subscribers[0])) return;
    g.subscribers[g.subscriber_count].cb  = cb;
    g.subscribers[g.subscriber_count].ctx = ctx;
    g.subscriber_count++;
}

static int  osc_get_mix(void)   { return g.mix_idx; }

static void osc_set_mix(int idx) {
    g.mix_idx = idx;
    sync_master_id_to_app_state();
    // Heartbeat-subscribe means there is nothing to "re-subscribe" -- the
    // next /con/n/<...lvl> push for the new mix index lands automatically.
    // But the filter's master-id slot needs to follow the new mix and
    // re-prime so the new mix's levels arrive right away.
    filter_rebuild();
    g.primed   = false;
    g.prime_idx = 0;
}

static void osc_set_mix_layout(int offset, int count) {
    g.mix_offset = offset;
    g.mix_count  = count;
    sync_master_id_to_app_state();
    if (count > 0) {
        g.mix_list_received = true;
        notify_subscribers();
    }
    filter_rebuild();
}

static const char *osc_get_mix_name(int idx) {
    if (idx < 0 || idx >= g.mix_count || idx >= MAX_MIX_BUSES) return NULL;
    if (g.mix_names[idx][0] == 0) return NULL;
    return g.mix_names[idx];
}

static bool osc_is_mix_routed     (int idx)     { (void)idx; return true; }   // TODO: P11 mixTargets
static void osc_fetch_mix_routing (void)        {}                            // TODO: P11
static bool osc_is_mix_list_ready (void)        { return g.mix_list_received; }
static void osc_resubscribe       (void)        {
    // Heartbeat covers the subscribe; refresh the filter (the tracked-id
    // bitmap may have changed) and re-prime so missing initial values
    // arrive again.
    filter_rebuild();
    g.primed   = false;
    g.prime_idx = 0;
}
static void osc_reconnect         (void)        {
    udp_pcb_down();
    // PCB will come back up on the next loop iteration via udp_pcb_up.
}

static void osc_fetch_all_strip_names(int total)         { (void)total; /* TODO */ }
static const char *osc_get_strip_name(int id) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "CH %02d", id + 1);
    return buf;
}

static void osc_fetch_channel_routability(int total)     { (void)total; /* TODO */ }
static bool osc_is_channel_routable      (int id)        { (void)id; return true; }

static void osc_set_meter_enabled(bool on)               { (void)on; /* TODO */ }

// OSC graceful shutdown is asymmetric vs WS: there is no /unsubscribe
// path on the OSC bridge -- MS's heartbeat-subscribe model just stops
// pushing to a UDP source after ~5 s of no inbound /hi packets. The
// equivalent of "tell MS we're going" is "stop heartbeating and let the
// timeout drop us"; UDP has no connection state to clean up. We still
// drain any pending outbound packets so a final SET (e.g. user toggled
// mute right before reboot) doesn't get dropped in the worker queue.
// The WS-vs-OSC asymmetry is intentional: documenting it here so a
// future reader doesn't try to add a non-existent unsubscribe path.
static void osc_shutdown_graceful(void)
{
    if (!g.task) return;

    // Drain whatever's queued so the last user action lands. ~250 ms
    // budget; outbound is one UDP send per poll, queue is typically
    // single-digit entries.
    for (int i = 0; i < 10; ++i) {
        bool empty;
        xSemaphoreTake(g.outq_mtx, portMAX_DELAY);
        empty = (g.outq_head == NULL);
        xSemaphoreGive(g.outq_mtx);
        if (empty) break;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    // Heartbeat dies naturally when the worker stops. MS's UDP-source
    // entry expires server-side; nothing more to do.
}

static void osc_set_level_format(app_level_format_t f) {
    if (g.level_format == f) return;
    g.level_format = f;
    // Heartbeat-subscribe only delivers values on CHANGE -- channels that
    // haven't moved since boot never land a /con/v (or /con/n) broadcast.
    // handle_inbound also filters by active format, so the inbound stream
    // we DO have is now wrong-format and gets discarded. Re-prime so the
    // worker fetches every tracked channel's lvl in the new format.
    // Cheap (~30 paths, ~750 ms paced), and harmless if MS happens to
    // also push the same values via change broadcasts in the meantime.
    g.primed   = false;
    g.prime_idx = 0;
    osc_expect_clear(&g.expect);
    // Force a fresh heartbeat in the new flavor on the next poll
    // iteration so MS switches its broadcast format right away. Without
    // this, we'd wait up to HEARTBEAT_MS for MS to flip to /con/v
    // broadcasts, leaving the UI seeing nothing for a few seconds while
    // discarding the stale-flavor stream.
    g.last_heartbeat_ms = 0;
}

static const ms_client_iface_t s_iface = {
    .start                       = osc_start,
    .set_level                   = osc_set_level,
    .set_mute                    = osc_set_mute,
    .set_name                    = osc_set_name,
    .stop                        = osc_stop,
    .get_state                   = osc_get_state,
    .get_host                    = osc_get_host,
    .get_port                    = osc_get_port,
    .register_on_change          = osc_register_on_change,
    .get_mix                     = osc_get_mix,
    .set_mix                     = osc_set_mix,
    .set_mix_layout              = osc_set_mix_layout,
    .get_mix_name                = osc_get_mix_name,
    .is_mix_routed               = osc_is_mix_routed,
    .fetch_mix_routing           = osc_fetch_mix_routing,
    .is_mix_list_ready           = osc_is_mix_list_ready,
    .resubscribe                 = osc_resubscribe,
    .reconnect                   = osc_reconnect,
    .set_master_level            = osc_set_master_level,
    .set_master_mute             = osc_set_master_mute,
    .fetch_all_strip_names       = osc_fetch_all_strip_names,
    .get_strip_name              = osc_get_strip_name,
    .fetch_channel_routability   = osc_fetch_channel_routability,
    .is_channel_routable         = osc_is_channel_routable,
    .set_meter_enabled           = osc_set_meter_enabled,
    .set_level_format            = osc_set_level_format,
    .shutdown_graceful           = osc_shutdown_graceful,
};

const ms_client_iface_t *app_ms_client_osc(void) { return &s_iface; }
