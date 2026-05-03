// Mongoose-backed Mixing Station client. Replaces the prior
// esp_websocket_client + esp_http_client implementation. Same wire
// protocol and same ms_client_iface_t surface, so esp_ui_main.c and
// app_ui.c are unchanged. Same library the PC sim uses
// (pc_sim/ms_client_real.c), so protocol-level bugs reproduce on both
// sides.
//
// Architecture:
//   - Single mg_mgr running on a dedicated FreeRTOS task (ws_task) that
//     pinned-polls every 25 ms. Mongoose's event loop is single-threaded;
//     the task owns the mgr and its connections.
//   - Outbound JSON sends from non-mongoose tasks (LVGL UI fader drag,
//     console commands) push pre-rendered envelopes onto a mutex-guarded
//     queue. The worker drains it once the WS is open.
//   - Inbound WS broadcasts are parsed under cJSON (same parser
//     app_ms_info.c + app_prefs.c use, kept for tablet schema
//     consistency) and routed straight to app_state_set_*; the per-state
//     observers in app_ui.c bridge into LVGL via lv_async_call.
//
// Deferred from the prior implementation (intentional, listed so a
// future pass can pick them up):
//   * P11 /console/mixTargets fetch    — ws_fetch_mix_routing no-op;
//     ws_is_mix_routed returns true for all indices.
//   * P8 outstanding-SET tracker       — relies on echo broadcasts. In
//     practice MS echoes within ~10-50 ms; if a network blip drops one,
//     the UI's local optimistic state is still correct, the next
//     broadcast resyncs.
//   * 60 s reconnect watchdog          — mongoose handles socket-level
//     close detection; simple 2 s retry replaces the destroy-and-recreate
//     state machine.
//
// All deferred features are behind public iface methods that no-op
// rather than return ERR -- the UI checks for NULL fn-ptrs so even
// dropping them would be safe, but matching the iface keeps the rest of
// the firmware unchanged.

#include "app_config.h"
#include "app_logd.h"
#include "app_ms_client.h"
#include "app_ms_info.h"
#include "app_prefs.h"
#include "app_state.h"
#include "app_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

#include "mongoose.h"

static const char *TAG = "ms_ws";

#define WS_GET_PREFIX            "/console/data/get/"
#define WS_GET_PREFIX_LEN        (sizeof(WS_GET_PREFIX) - 1)
#define MAX_MIX_BUSES            24
// Si Expression 2 has 80 channels; cap covers it plus headroom for larger
// consoles. 128 * 32 = 4 KB BSS, acceptable for a one-time prime + live cache.
#define MAX_STRIP_NAMES          128
#define WORKER_STACK             8192
#define WORKER_PRIO              5
#define MGR_POLL_MS              25
#define RECONNECT_RETRY_MS       2000

// #30 metering: single /console/metering2 subscription. Sub id is arbitrary
// but must match between subscribe + the inbound `/console/metering2/<id>`
// path. 100 ms = 10 Hz, the rate the original prototype settled on (visible
// movement on the bar, low enough WS load to leave the fader path room).
#define METERING_SUB_ID          1
#define METERING_INTERVAL_MS     100

// /app/state heartbeat. MS doesn't expose state under the data tree so it
// can't be subscribed -- /console/data/subscribe only takes paths under
// `muteGroups` / `ch.<n>.*`. Polling is the only option. 5 s gives a
// reasonable detection latency for the "console powered on" transition
// without thrashing the MS HTTP server (the user's real-world cycle is
// console-off-for-a-week, not seconds).
#define HEARTBEAT_INTERVAL_MS    5000
#define HEARTBEAT_STACK          4096
#define HEARTBEAT_PRIO           4

// ────────────────────────────────────────────────────────────────────────────
// State
// ────────────────────────────────────────────────────────────────────────────

typedef struct outq_entry {
    char               *json;
    struct outq_entry  *next;
} outq_entry_t;

static struct {
    SemaphoreHandle_t       outq_mtx;
    outq_entry_t           *outq_head;
    outq_entry_t           *outq_tail;

    TaskHandle_t            task;
    volatile bool           running;

    struct mg_mgr           mgr;
    struct mg_connection   *ws_conn;
    bool                    ws_open;
    bool                    subscribed;
    uint32_t                last_reconnect_ms;

    char                    ws_url[160];
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

    // P3: cached scribble-strip names indexed by raw MS channel id. Primed
    // by ws_fetch_all_strip_names (REST sweep before ws_start) and kept
    // live by the cfg.name broadcast handler. Empty string at [id] means
    // "name unknown for that id" -- ws_get_strip_name returns NULL so the
    // picker falls back to "CH NN".
    char                    strip_names[MAX_STRIP_NAMES][32];

    // W6.1: routability mask. true = ch.<id>.levelData.0.lvl exists (input
    // strip — can be routed to a mix), false = 404 (mix/matrix/main bus
    // strip, self-routes only). Defaults to true so a never-fetched state
    // doesn't accidentally hide every row in the picker. Probed once by
    // ws_fetch_channel_routability before ws_start; channel TYPE doesn't
    // change without an MS profile swap (which restarts the WS), so the
    // mask is stable for the session.
    bool                    routable[MAX_STRIP_NAMES];
    bool                    routability_known;
    int                     total_channels;  // from fetch_channel_routability(total)

    // #30 metering. UI-driven flag (set_meter_enabled toggles it), persisted
    // across reconnects so the on-connect handler resubscribes if the user
    // had meter mode on. Param ids parallel the order we sent in subscribe
    // so the binary frame decoder can map int16 positions back to channel
    // slots. Master rides on the same subscribe (last entry, marked via
    // meter_is_master[]) so its meter follows the active bus without a
    // second roundtrip.
    bool                    meter_subscribed;
    int                     meter_param_ids[APP_CONFIG_MAX_CHANNELS + 1];
    bool                    meter_is_master[APP_CONFIG_MAX_CHANNELS + 1];
    int                     meter_param_count;

    // /app/state heartbeat. console_attached is the cached result of the
    // last poll; transitions from false->true trigger notify_subscribers
    // so esp_ui_main can run the deferred MS-info setup, and the UI can
    // recolor the MS icon. Owned and updated only by hb_task.
    TaskHandle_t            hb_task;
    bool                    console_attached;

    struct {
        app_ms_on_change_t cb;
        void              *ctx;
    } subscribers[4];
    size_t                  subscriber_count;
} g;

// ────────────────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────────────────

static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

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

static void outq_push(const char *json) {
    if (!g.outq_mtx || !json) return;
    outq_entry_t *e = (outq_entry_t *)malloc(sizeof *e);
    if (!e) return;
    e->json = dup_cstr(json);
    e->next = NULL;
    if (!e->json) { free(e); return; }
    xSemaphoreTake(g.outq_mtx, portMAX_DELAY);
    if (g.outq_tail) g.outq_tail->next = e; else g.outq_head = e;
    g.outq_tail = e;
    xSemaphoreGive(g.outq_mtx);
}

static outq_entry_t *outq_drain(void) {
    if (!g.outq_mtx) return NULL;
    xSemaphoreTake(g.outq_mtx, portMAX_DELAY);
    outq_entry_t *h = g.outq_head;
    g.outq_head = g.outq_tail = NULL;
    xSemaphoreGive(g.outq_mtx);
    return h;
}

// ────────────────────────────────────────────────────────────────────────────
// Outbound: subscribe + set
// ────────────────────────────────────────────────────────────────────────────

static void send_subscribe(const char *dotted, const char *fmt) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"POST\",\"path\":\"/console/data/subscribe\","
             "\"body\":{\"path\":\"%s\",\"format\":\"%s\"}}",
             dotted, fmt);
    outq_push(buf);
}

// Per the OpenAPI: /console/data/unsubscribe takes the same {path,format}
// body as subscribe and "the path must match 1:1 the path used for the
// subscription." Used on mix-change to drop the previous mix's per-channel
// subscriptions so MS stops broadcasting them, instead of bouncing the WS.
static void send_unsubscribe(const char *dotted, const char *fmt) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"POST\",\"path\":\"/console/data/unsubscribe\","
             "\"body\":{\"path\":\"%s\",\"format\":\"%s\"}}",
             dotted, fmt);
    outq_push(buf);
}

static void unsubscribe_channel(int ch_id, int mix_idx) {
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.lvl", ch_id, mix_idx);
    send_unsubscribe(dotted, g.level_format == APP_LEVEL_FORMAT_DB ? "val" : "norm");
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.on", ch_id, mix_idx);
    send_unsubscribe(dotted, "val");
    // cfg.name is per-channel (not mix-specific) -- leave it subscribed.
}

static void unsubscribe_master(int master_id) {
    if (master_id < 0) return;
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.lvl", master_id);
    send_unsubscribe(dotted, g.level_format == APP_LEVEL_FORMAT_DB ? "val" : "norm");
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.on", master_id);
    send_unsubscribe(dotted, "val");
}

static void send_set_norm(const char *dotted, float v) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"POST\",\"path\":\"/console/data/set/%s/norm\","
             "\"body\":{\"value\":%.6f}}",
             dotted, v);
    outq_push(buf);
}

static void send_set_val_db(const char *dotted, float db) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"POST\",\"path\":\"/console/data/set/%s/val\","
             "\"body\":{\"value\":%.4f}}",
             dotted, db);
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
             dotted, s ? s : "");
    outq_push(buf);
}

// Master strip's MS channel id is the mix-bus's own channel id, which
// is mix_offset + selected_mix_idx. Computed (not cached) because the
// layout changes at runtime via set_mix_layout and the selected mix
// changes via set_mix; reading the live values keeps subscribe / SET /
// broadcast-routing paths in sync without a separate notification.
// Returns -1 when the mix layout hasn't been received yet (boot before
// /console/information).
static int master_channel_id(void) {
    if (g.mix_count <= 0)                          return -1;
    if (g.mix_idx < 0 || g.mix_idx >= g.mix_count) return -1;
    return g.mix_offset + g.mix_idx;
}

// Whenever the master id changes (mix layout arrived, or user picked a
// different mix), push it into app_state so app_ui's master strip + the
// inbound broadcast handler's id-comparison see the right value.
// Idempotent: app_state_master_set_id no-ops on unchanged id.
static void sync_master_id_to_app_state(void) {
    app_state_master_set_id(master_channel_id());
}

static void subscribe_channel(int ch_id, int mix_idx) {
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", ch_id);
    send_subscribe(dotted, "val");
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.lvl", ch_id, mix_idx);
    send_subscribe(dotted, g.level_format == APP_LEVEL_FORMAT_DB ? "val" : "norm");
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.on", ch_id, mix_idx);
    send_subscribe(dotted, "val");
}

static void subscribe_master(int master_id) {
    if (master_id < 0) return;
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", master_id);
    send_subscribe(dotted, "val");
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.lvl", master_id);
    send_subscribe(dotted, g.level_format == APP_LEVEL_FORMAT_DB ? "val" : "norm");
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.on", master_id);
    send_subscribe(dotted, "val");
}

static void subscribe_mix_names(void) {
    for (int i = 0; i < g.mix_count && i < MAX_MIX_BUSES; ++i) {
        char dotted[80];
        snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", g.mix_offset + i);
        send_subscribe(dotted, "val");
    }
}

// Subscribe cfg.name for every routable channel so the picker reflects
// live renames mid-session, not just at boot. MS dedupes by (path,
// format) server-side so re-subscribing names already covered by
// subscribe_channel/subscribe_master is a no-op. Called from
// subscribe_all (per-connect) AND from ws_fetch_channel_routability
// (covers the deferred-info path where routability arrives after WS
// open). Skipped if routability hasn't been probed yet -- the boot-time
// REST sweep has already primed the cache for the picker's first-open
// case, so a couple of seconds without live updates is fine.
static void subscribe_all_routable_names(void) {
    if (!g.routability_known) return;
    int max = g.total_channels;
    if (max > MAX_STRIP_NAMES) max = MAX_STRIP_NAMES;
    int n = 0;
    for (int id = 0; id < max; ++id) {
        if (!g.routable[id]) continue;
        char dotted[80];
        snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", id);
        send_subscribe(dotted, "val");
        n++;
    }
    ESP_LOGI(TAG, "subscribed cfg.name for %d routable channels", n);
}

// #30: metering subscribe. Body is bigger than the 256 B stack frame in
// send_subscribe (per-channel param objects scale with channel count), so
// build via cJSON and outq_push the unformatted string directly. Subscribe
// order is "tracked input channels, then master" -- the parser uses the
// same ordering to map int16 positions back to slots. Re-fired on every
// connect / mix change while the UI's meter pref is on; mix change
// matters because the master's MS channel id rolls with the active bus.
static void meter_send_subscribe(void) {
    g.meter_param_count = 0;
    cJSON *root   = cJSON_CreateObject();
    cJSON *body   = cJSON_CreateObject();
    cJSON *params = cJSON_CreateArray();
    if (!root || !body || !params) {
        if (root)   cJSON_Delete(root);
        if (body)   cJSON_Delete(body);
        if (params) cJSON_Delete(params);
        return;
    }
    size_t cap = sizeof(g.meter_param_ids) / sizeof(g.meter_param_ids[0]);
    for (size_t i = 0; i < app_state_count() && g.meter_param_count < cap; ++i) {
        int ch_id = app_state_id_for_idx(i);
        if (ch_id < 0) continue;
        g.meter_param_ids[g.meter_param_count]  = ch_id;
        g.meter_is_master[g.meter_param_count]  = false;
        g.meter_param_count++;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "index", ch_id);
        cJSON_AddNumberToObject(p, "type",  0);  // mono peak
        cJSON_AddItemToArray(params, p);
    }
    int mid = master_channel_id();
    if (mid >= 0 && g.meter_param_count < cap) {
        g.meter_param_ids[g.meter_param_count] = mid;
        g.meter_is_master[g.meter_param_count] = true;
        g.meter_param_count++;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "index", mid);
        cJSON_AddNumberToObject(p, "type",  0);
        cJSON_AddItemToArray(params, p);
    }
    cJSON_AddBoolToObject  (body, "binary",   true);
    cJSON_AddNumberToObject(body, "interval", METERING_INTERVAL_MS);
    cJSON_AddNumberToObject(body, "id",       METERING_SUB_ID);
    cJSON_AddItemToObject  (body, "params",   params);  // takes ownership

    cJSON_AddStringToObject(root, "method", "POST");
    cJSON_AddStringToObject(root, "path",   "/console/metering2/subscribe");
    cJSON_AddItemToObject  (root, "body",   body);

    char *frame = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!frame) return;
    outq_push(frame);
    free(frame);
    ESP_LOGI(TAG, "meter subscribe: %d entries @ %d ms",
             g.meter_param_count, METERING_INTERVAL_MS);
}

static void meter_send_unsubscribe(void) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"method\":\"POST\",\"path\":\"/console/metering/unsubscribe\","
             "\"body\":{\"id\":%d}}", METERING_SUB_ID);
    outq_push(buf);
    g.meter_param_count = 0;
}

static void subscribe_all(void) {
    size_t n = app_state_count();
    for (size_t i = 0; i < n; ++i) {
        int id = app_state_id_for_idx(i);
        if (id >= 0) subscribe_channel(id, g.mix_idx);
    }
    subscribe_master(master_channel_id());
    subscribe_mix_names();
    // Names for every routable channel so the picker stays current when
    // a non-tracked input gets renamed on the console mid-session. No-op
    // until routability has been probed; the deferred-info path covers
    // the case where routability lands after WS open by re-firing this
    // subscribe directly from ws_fetch_channel_routability.
    subscribe_all_routable_names();
    // Re-arm metering if the user had it on at last shutdown OR before this
    // (re)connect. MS treats a fresh subscribe as an in-place replacement of
    // the params list, so this also handles the "channel set changed via
    // picker" path -- the new id list lands on the next subscribe_all.
    if (g.meter_subscribed) meter_send_subscribe();
    g.subscribed = true;
}

// ────────────────────────────────────────────────────────────────────────────
// Inbound: route a "/console/data/get/<dotted>" broadcast
// ────────────────────────────────────────────────────────────────────────────

// #30: pad a non-padded base64 string in place so mbedtls accepts it. MS
// emits trailing '=' padding stripped (RFC 4648 5.) — re-add up to 2.
static size_t pad_b64(char *buf, size_t in_len, size_t buf_cap) {
    size_t pad = (4 - (in_len % 4)) % 4;
    if (in_len + pad + 1 > buf_cap) return 0;
    for (size_t i = 0; i < pad; ++i) buf[in_len + i] = '=';
    buf[in_len + pad] = '\0';
    return in_len + pad;
}

// Decode one /console/metering2/<id> broadcast body. Filters by id so a
// stale subscription from a prior mix-change resub is ignored. Maps
// payload position -> app_state slot via g.meter_param_ids.
static void handle_metering(int sub_id, cJSON *jbody) {
    if (sub_id != METERING_SUB_ID) return;
    if (g.meter_param_count <= 0) return;

    cJSON *jb = cJSON_GetObjectItem(jbody, "b");
    if (!cJSON_IsString(jb) || !jb->valuestring) return;
    const char *b64 = jb->valuestring;
    size_t in_len = strlen(b64);
    if (in_len == 0) return;

    char padded[APP_CONFIG_MAX_CHANNELS * 4 + 8];
    if (in_len + 4 > sizeof(padded)) return;
    memcpy(padded, b64, in_len);
    size_t padded_len = pad_b64(padded, in_len, sizeof(padded));
    if (padded_len == 0) return;

    unsigned char raw[APP_CONFIG_MAX_CHANNELS * 2 + 4];
    size_t        raw_len = 0;
    int rc = mbedtls_base64_decode(raw, sizeof(raw), &raw_len,
                                   (const unsigned char *) padded, padded_len);
    if (rc != 0) return;

    int values = (int) (raw_len / 2);
    if (values > g.meter_param_count) values = g.meter_param_count;
    for (int i = 0; i < values; ++i) {
        // big-endian int16, scale=100. Si silence floor is -90 dB.
        int16_t v = (int16_t) ((raw[2 * i] << 8) | raw[2 * i + 1]);
        float db  = (float) v / 100.0f;
        if (g.meter_is_master[i]) {
            app_state_master_set_meter_db(db, true);
        } else {
            int idx = app_state_idx_for_id(g.meter_param_ids[i]);
            if (idx >= 0) app_state_set_meter_db((size_t) idx, db, true);
        }
    }
}

// Parse "ch.<n>.<rest>" into ch_id + suffix. Returns NULL on no-match.
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

static void handle_broadcast(const char *json, size_t len) {
    char *copy = (char *)malloc(len + 1);
    if (!copy) return;
    memcpy(copy, json, len);
    copy[len] = 0;

    cJSON *root = cJSON_Parse(copy);
    free(copy);
    if (!root) return;

    cJSON *jpath = cJSON_GetObjectItem(root, "path");
    cJSON *jbody = cJSON_GetObjectItem(root, "body");
    if (!cJSON_IsString(jpath) || !cJSON_IsObject(jbody)) {
        cJSON_Delete(root);
        return;
    }
    const char *path = jpath->valuestring;

    // #30: metering broadcasts arrive on `/console/metering2/<id>`, NOT
    // under the `/console/data/get/` prefix. Route them off here before
    // the prefix check rejects.
    {
        int sub_id;
        if (sscanf(path, "/console/metering2/%d", &sub_id) == 1) {
            handle_metering(sub_id, jbody);
            cJSON_Delete(root);
            return;
        }
    }

    if (strncmp(path, WS_GET_PREFIX, WS_GET_PREFIX_LEN) != 0) {
        cJSON_Delete(root);
        return;
    }
    const char *dotted = path + WS_GET_PREFIX_LEN;

    int id = -1;
    const char *suf = parse_ch_path(dotted, &id);
    if (!suf) { cJSON_Delete(root); return; }

    cJSON *jvalue = cJSON_GetObjectItem(jbody, "value");
    int idx        = app_state_idx_for_id(id);
    bool is_master = (id == master_channel_id());

    if (strcmp(suf, "cfg.name") == 0 && cJSON_IsString(jvalue)) {
        const char *name = jvalue->valuestring;
        if (idx >= 0)       app_state_set_name(idx, name, true);
        else if (is_master) app_state_master_set_name(name, true);
        // Strip-name cache for the picker. Tracked + master + mix + every
        // other channel id all flow through this branch, so caching here
        // means the picker reflects live renames without a re-sweep.
        if (id >= 0 && id < MAX_STRIP_NAMES) {
            strncpy(g.strip_names[id], name, sizeof(g.strip_names[id]) - 1);
            g.strip_names[id][sizeof(g.strip_names[id]) - 1] = 0;
        }
        // Mix bus name (ch.<mix_offset+i>.cfg.name)
        if (g.mix_count > 0 && id >= g.mix_offset && id < g.mix_offset + g.mix_count) {
            int slot = id - g.mix_offset;
            strncpy(g.mix_names[slot], name, sizeof(g.mix_names[slot]) - 1);
            g.mix_names[slot][sizeof(g.mix_names[slot]) - 1] = 0;
            notify_subscribers();
        }
        cJSON_Delete(root);
        return;
    }

    char level_prefix[32];
    snprintf(level_prefix, sizeof(level_prefix), "levelData.%d.", g.mix_idx);
    size_t plen = strlen(level_prefix);
    if (strncmp(suf, level_prefix, plen) == 0) {
        const char *tail = suf + plen;
        if (idx >= 0 && cJSON_IsNumber(jvalue) && strcmp(tail, "lvl") == 0) {
            // Could be norm or val depending on subscribe format. Push to
            // the appropriate app_state setter.
            if (g.level_format == APP_LEVEL_FORMAT_DB) {
                app_state_set_level_db(idx, (float)jvalue->valuedouble, true);
            } else {
                app_state_set_level(idx, (float)jvalue->valuedouble, true);
            }
        } else if (idx >= 0 && cJSON_IsBool(jvalue) && strcmp(tail, "on") == 0) {
            // MS "on" = audible = NOT muted.
            bool on = cJSON_IsTrue(jvalue);
            app_state_set_mute(idx, !on, true);
        }
    } else if (is_master) {
        if (cJSON_IsNumber(jvalue) && strcmp(suf, "mix.lvl") == 0) {
            if (g.level_format == APP_LEVEL_FORMAT_DB) {
                app_state_master_set_level_db((float)jvalue->valuedouble, true);
            } else {
                app_state_master_set_level((float)jvalue->valuedouble, true);
            }
        } else if (cJSON_IsBool(jvalue) && strcmp(suf, "mix.on") == 0) {
            app_state_master_set_mute(!cJSON_IsTrue(jvalue), true);
        }
    }

    cJSON_Delete(root);
}

// ────────────────────────────────────────────────────────────────────────────
// HTTP: /console/information for mix layout, /app/mixers/offline to attach
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
        // Discard the response body; we only care that the call was made.
        c->is_draining = 1;
    } else if (ev == MG_EV_ERROR) {
        ESP_LOGD(TAG, "offline-attach error: %s", (const char *)ev_data);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// WS event handler
// ────────────────────────────────────────────────────────────────────────────

static void ws_evt(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
    (void)c; (void)fn_data;
    if (ev == MG_EV_OPEN) {
        // TCP up; nothing to do until WS handshake completes.
    } else if (ev == MG_EV_ERROR) {
        ESP_LOGW(TAG, "ws error: %s", (const char *)ev_data);
        set_state(APP_MS_STATE_ERROR);
    } else if (ev == MG_EV_WS_OPEN) {
        ESP_LOGI(TAG, "ws open to %s", g.ws_url);
        g.ws_open = true;
        set_state(APP_MS_STATE_CONNECTED);
        if (!g.subscribed) subscribe_all();
        if (g.mix_count > 0) g.mix_list_received = true;
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *m = (struct mg_ws_message *)ev_data;
        handle_broadcast(m->data.ptr, m->data.len);
    } else if (ev == MG_EV_CLOSE) {
        ESP_LOGW(TAG, "ws closed");
        g.ws_open    = false;
        g.ws_conn    = NULL;
        g.subscribed = false;
        g.mix_list_received = false;
        set_state(APP_MS_STATE_DISCONNECTED);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Worker task — owns the mg_mgr and drives reconnect
// ────────────────────────────────────────────────────────────────────────────

static void compose_urls(void) {
    snprintf(g.ws_url,      sizeof(g.ws_url),      "ws://%s:%u/", app_config_ms_host(), (unsigned)app_config_ms_port());
    snprintf(g.http_base,   sizeof(g.http_base),   "http://%s:%u", app_config_ms_host(), (unsigned)app_config_ms_port());
    snprintf(g.info_url,    sizeof(g.info_url),    "%s/console/information",  g.http_base);
    snprintf(g.offline_url, sizeof(g.offline_url), "%s/app/mixers/offline",   g.http_base);
}

static void ws_task(void *unused) {
    (void)unused;
    mg_mgr_init(&g.mgr);
    set_state(APP_MS_STATE_CONNECTING);

    compose_urls();

    // One-shot: best-effort offline-mixer attach, then info fetch. If MS
    // is already in offline mode either call is harmless; the offline
    // POST returns the same console object whether or not it changed
    // anything.
    mg_http_connect(&g.mgr, g.offline_url, offline_evt, NULL);
    mg_http_connect(&g.mgr, g.info_url,    info_evt,    NULL);

    g.ws_conn = mg_ws_connect(&g.mgr, g.ws_url, ws_evt, NULL, NULL);

    uint32_t last_ping_ms = 0;
    while (g.running) {
        mg_mgr_poll(&g.mgr, MGR_POLL_MS);

        if (g.ws_open && g.ws_conn) {
            outq_entry_t *e = outq_drain();
            while (e) {
                mg_ws_send(g.ws_conn, e->json, strlen(e->json), WEBSOCKET_OP_TEXT);
                outq_entry_t *next = e->next;
                free(e->json);
                free(e);
                e = next;
            }
        }

        // Periodic WS PING. Without this MS closes idle sockets after
        // ~25 s and the WS cycles open/closed forever even with no user
        // activity. mongoose auto-pongs received PINGs but doesn't send
        // them on its own, so we drive the keepalive ourselves. 15 s
        // gives a comfortable margin under any reasonable server-side
        // idle timeout.
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (g.ws_open && g.ws_conn && now - last_ping_ms > 15000) {
            mg_ws_send(g.ws_conn, NULL, 0, WEBSOCKET_OP_PING);
            last_ping_ms = now;
        }

        if (!g.ws_conn) {
            if (now - g.last_reconnect_ms > RECONNECT_RETRY_MS) {
                g.last_reconnect_ms = now;
                set_state(APP_MS_STATE_CONNECTING);
                mg_http_connect(&g.mgr, g.info_url, info_evt, NULL);
                g.ws_conn = mg_ws_connect(&g.mgr, g.ws_url, ws_evt, NULL, NULL);
            }
        }
    }

    mg_mgr_free(&g.mgr);
    vTaskDelete(NULL);
}

// /app/state heartbeat task. Runs alongside the ws_task and polls the
// console-attached flag every HEARTBEAT_INTERVAL_MS. Lives separately
// because esp_http_client_perform is blocking and we don't want it
// inside the mongoose poll loop. Lightweight: ~one HTTP request every
// 5 s, ~4 KB stack.
static void hb_task(void *arg)
{
    (void) arg;
    while (g.running) {
        bool was = g.console_attached;
        bool now = app_ms_info_console_ready(app_config_ms_host(),
                                             (int) app_config_ms_port());
        if (now != was) {
            g.console_attached = now;
            ESP_LOGI(TAG, "console_attached %s -> %s",
                     was ? "true" : "false", now ? "true" : "false");
            notify_subscribers();
        }
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    }
    vTaskDelete(NULL);
}

// ────────────────────────────────────────────────────────────────────────────
// ms_client_iface_t implementations
// ────────────────────────────────────────────────────────────────────────────

static bool ws_is_console_attached(void) { return g.console_attached; }

static bool ws_start(void) {
    if (g.task) return true;
    g.outq_mtx = xSemaphoreCreateMutex();
    g.running  = true;
    g.state    = APP_MS_STATE_BOOT;
    g.subscriber_count = g.subscriber_count;  // keep prior registrations
    // Seed level_format from prefs so the first subscribe goes out in the
    // right shape. Without this, a tablet booting with level=db saved in
    // NVS subscribes in "norm", the broadcasts land in ch.level, and the
    // UI's apply_pending reads ch.level_db (never populated) -> -inf dB.
    g.level_format = app_prefs_get_level_format();
    if (xTaskCreatePinnedToCore(ws_task, "ms_ws", WORKER_STACK, NULL, WORKER_PRIO, &g.task, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return false;
    }
    if (xTaskCreatePinnedToCore(hb_task, "ms_hb", HEARTBEAT_STACK, NULL, HEARTBEAT_PRIO, &g.hb_task, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGW(TAG, "hb task xTaskCreate failed");
        // Non-fatal -- WS path still works, just no console-attached signal
        // and the deferred MS-info setup will only fire on first WS connect.
    }
    return true;
}

static void ws_stop(void) {
    g.running = false;
    // The task tears itself down on the next loop iteration. We don't
    // join: xTaskCreate's deletion is async, and we don't need to wait
    // here -- nothing on the caller's side touches the mgr.
    g.task = NULL;
    set_state(APP_MS_STATE_BOOT);
}

static void ws_set_level(int id, float level) {
    // app_ui.c always hands us a NORM position (0..1) regardless of
    // the active level format pref. In DB mode we need to convert to
    // dB before sending down the /val path -- otherwise MS receives
    // a "0.5 dB" SET when the slider was at 50% (which the audio-
    // taper maps to ~-13 dB), snaps to ~+0.5 dB, broadcasts back, and
    // the slider visibly jumps to ~76% on release. Symptom report
    // was "fader slides but doesn't move MS, then snaps back on
    // release" -- this is the conversion that was missing.
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.lvl", id, g.mix_idx);
    if (g.level_format == APP_LEVEL_FORMAT_DB) {
        send_set_val_db(dotted, app_position_to_db(level));
    } else {
        send_set_norm(dotted, level);
    }
}

static void ws_set_mute(int id, bool mute) {
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.levelData.%d.on", id, g.mix_idx);
    send_set_bool(dotted, !mute);
}

static void ws_set_name(int id, const char *name) {
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", id);
    send_set_str(dotted, name);
}

static void ws_set_master_level(float level) {
    int mid = master_channel_id();
    if (mid < 0) return;
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.lvl", mid);
    // Always /norm (see ws_set_level for rationale).
    send_set_norm(dotted, level);
}

static void ws_set_master_mute(bool mute) {
    int mid = master_channel_id();
    if (mid < 0) return;
    char dotted[80];
    snprintf(dotted, sizeof(dotted), "ch.%d.mix.on", mid);
    send_set_bool(dotted, !mute);
}

static app_ms_state_t ws_get_state(void) { return g.state; }
static const char    *ws_get_host (void) { return app_config_ms_host(); }
static int            ws_get_port (void) { return (int)app_config_ms_port(); }

static void ws_register_on_change(app_ms_on_change_t cb, void *ctx) {
    if (!cb) return;
    if (g.subscriber_count >= sizeof(g.subscribers) / sizeof(g.subscribers[0])) return;
    g.subscribers[g.subscriber_count].cb  = cb;
    g.subscribers[g.subscriber_count].ctx = ctx;
    g.subscriber_count++;
}

static int  ws_get_mix(void)   { return g.mix_idx; }

static void ws_set_mix(int idx) {
    int old_mix       = g.mix_idx;
    int old_master_id = master_channel_id();

    g.mix_idx    = idx;
    g.subscribed = false;
    sync_master_id_to_app_state();

    if (!g.ws_open) return;

    ESP_LOGI(TAG, "mix bus -> %d (Mix %d)", idx, idx + 1);

    // /console/data/unsubscribe lets us drop the old mix's per-channel
    // and old-master paths cleanly without bouncing the WS. cfg.name +
    // mix-name subscriptions aren't mix-specific so they stay live.
    if (old_mix != idx) {
        size_t n = app_state_count();
        for (size_t i = 0; i < n; ++i) {
            int ch = app_state_id_for_idx(i);
            if (ch >= 0) unsubscribe_channel(ch, old_mix);
        }
    }
    if (old_master_id != master_channel_id()) {
        unsubscribe_master(old_master_id);
    }

    // Re-subscribe under the new mix. subscribe_all also re-fires
    // metering if it was on; the master id is part of the metering
    // params so it must be rebuilt with the new id.
    subscribe_all();
}

static void ws_set_mix_layout(int offset, int count) {
    g.mix_offset = offset;
    g.mix_count  = count;
    sync_master_id_to_app_state();
    if (count > 0) {
        g.mix_list_received = true;
        if (g.ws_open) subscribe_mix_names();
        notify_subscribers();
    }
}

static const char *ws_get_mix_name(int idx) {
    if (idx < 0 || idx >= g.mix_count || idx >= MAX_MIX_BUSES) return NULL;
    if (g.mix_names[idx][0] == 0) return NULL;
    return g.mix_names[idx];
}

static bool ws_is_mix_routed     (int idx)     { (void)idx; return true; }   // TODO: P11 mixTargets
static void ws_fetch_mix_routing (void)        {}                            // TODO: P11
static bool ws_is_mix_list_ready (void)        { return g.mix_list_received; }
static void ws_resubscribe       (void)        { if (g.ws_open) { g.subscribed = false; subscribe_all(); } }
static void ws_reconnect         (void)        {
    if (g.ws_conn) g.ws_conn->is_draining = 1;
}

// P3 strip-name sweep. esp_http_client one-shot per channel; ~30 ms each
// over typical stage WiFi -> ~2.4 s for an 80-channel Si Expression. Runs
// from app_main BEFORE ws_start so the worker task / mongoose mgr aren't up
// yet, no contention. The WS cfg.name broadcast handler keeps the cache
// live afterwards.
typedef struct { char *buf; size_t cap; size_t len; } strip_name_sink_t;

static esp_err_t strip_name_http_event(esp_http_client_event_t *evt)
{
    strip_name_sink_t *sink = (strip_name_sink_t *) evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && sink && evt->data && evt->data_len > 0) {
        if (sink->len + (size_t) evt->data_len < sink->cap) {
            memcpy(sink->buf + sink->len, evt->data, evt->data_len);
            sink->len += evt->data_len;
        }
    }
    return ESP_OK;
}

static bool fetch_strip_name(const char *host, int port, int id,
                             char *out, size_t out_size)
{
    char url[160];
    snprintf(url, sizeof(url),
             "http://%s:%d/console/data/get/ch.%d.cfg.name/val",
             host, port, id);

    char buf[192];
    strip_name_sink_t sink = { .buf = buf, .cap = sizeof(buf), .len = 0 };
    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = strip_name_http_event,
        .user_data     = &sink,
        .timeout_ms    = 1500,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
    esp_err_t err = esp_http_client_perform(cli);
    int status   = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK || status != 200) return false;

    buf[sink.len < sink.cap ? sink.len : sink.cap - 1] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;
    cJSON *jv = cJSON_GetObjectItem(root, "value");
    bool ok = false;
    if (cJSON_IsString(jv) && jv->valuestring) {
        strncpy(out, jv->valuestring, out_size - 1);
        out[out_size - 1] = 0;
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static void ws_fetch_all_strip_names(int total)
{
    if (total <= 0) return;
    if (total > MAX_STRIP_NAMES) total = MAX_STRIP_NAMES;
    const char *host = app_config_ms_host();
    int         port = (int) app_config_ms_port();
    int ok = 0;
    for (int id = 0; id < total; ++id) {
        if (fetch_strip_name(host, port, id,
                             g.strip_names[id], sizeof(g.strip_names[id]))) {
            ok++;
        }
    }
    ESP_LOGI(TAG, "P3: name sweep %d/%d", ok, total);
}

static const char *ws_get_strip_name(int id)
{
    if (id < 0 || id >= MAX_STRIP_NAMES)  return NULL;
    if (g.strip_names[id][0] == '\0')      return NULL;
    return g.strip_names[id];
}

// W6.1: probe ch.<n>.levelData.0.lvl/norm with a HEAD-equivalent REST GET
// for each channel id. 200 means the path exists (input/aux strip with a
// per-mix send), 404 means the path doesn't exist (mix/matrix/main bus --
// these self-route via mix.lvl, levelData.<m>.lvl is undefined for them).
// Same blocking-sweep + esp_http_client pattern as the strip-name sweep.
static bool probe_routable(const char *host, int port, int id)
{
    char url[160];
    snprintf(url, sizeof(url),
             "http://%s:%d/console/data/get/ch.%d.levelData.0.lvl/norm",
             host, port, id);
    esp_http_client_config_t cfg = {
        .url        = url,
        .timeout_ms = 1500,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return true;  // best-effort default: don't hide on failure
    esp_err_t err = esp_http_client_perform(cli);
    int status   = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK) return true;  // network blip -- don't hide
    return status == 200;
}

// Forward decl for the cross-call below.
static void subscribe_all_routable_names(void);

static void ws_fetch_channel_routability(int total)
{
    if (total <= 0) return;
    if (total > MAX_STRIP_NAMES) total = MAX_STRIP_NAMES;
    const char *host = app_config_ms_host();
    int         port = (int) app_config_ms_port();
    int routable_count = 0;
    for (int id = 0; id < total; ++id) {
        g.routable[id] = probe_routable(host, port, id);
        if (g.routable[id]) routable_count++;
    }
    g.total_channels    = total;
    g.routability_known = true;
    ESP_LOGI(TAG, "W6.1: routability %d/%d", routable_count, total);
    // If WS is already up by the time routability lands (deferred MS-info
    // path: console attached after WS open), subscribe the full routable
    // name set right now so the picker stays live without waiting for
    // the next mix change to re-fire subscribe_all.
    if (g.ws_open) subscribe_all_routable_names();
}

static bool ws_is_channel_routable(int id)
{
    if (!g.routability_known)         return true;  // pre-fetch: assume yes
    if (id < 0 || id >= MAX_STRIP_NAMES) return true;
    return g.routable[id];
}

static void ws_set_meter_enabled(bool on)
{
    bool was = g.meter_subscribed;
    g.meter_subscribed = on;
    if (g.ws_open) {
        if (on)        meter_send_subscribe();
        else if (was)  meter_send_unsubscribe();
    }
    // Reset cached meter values to the no-sample sentinel on a mode flip
    // so stale readings don't briefly flash on the bar before the first
    // new broadcast lands. Both directions: turning OFF should clear too.
    if (on != was) {
        for (size_t i = 0; i < app_state_count(); ++i) {
            app_state_set_meter_db(i, -200.0f, true);
        }
    }
}

static void ws_set_level_format(app_level_format_t f) {
    if (g.level_format == f) return;
    g.level_format = f;
    g.subscribed   = false;
    if (g.ws_open) subscribe_all();
}

static const ms_client_iface_t s_iface = {
    .start                       = ws_start,
    .set_level                   = ws_set_level,
    .set_mute                    = ws_set_mute,
    .set_name                    = ws_set_name,
    .stop                        = ws_stop,
    .get_state                   = ws_get_state,
    .get_host                    = ws_get_host,
    .get_port                    = ws_get_port,
    .register_on_change          = ws_register_on_change,
    .get_mix                     = ws_get_mix,
    .set_mix                     = ws_set_mix,
    .set_mix_layout              = ws_set_mix_layout,
    .get_mix_name                = ws_get_mix_name,
    .is_mix_routed               = ws_is_mix_routed,
    .fetch_mix_routing           = ws_fetch_mix_routing,
    .is_mix_list_ready           = ws_is_mix_list_ready,
    .resubscribe                 = ws_resubscribe,
    .reconnect                   = ws_reconnect,
    .set_master_level            = ws_set_master_level,
    .set_master_mute             = ws_set_master_mute,
    .fetch_all_strip_names       = ws_fetch_all_strip_names,
    .get_strip_name              = ws_get_strip_name,
    .fetch_channel_routability   = ws_fetch_channel_routability,
    .is_channel_routable         = ws_is_channel_routable,
    .set_meter_enabled           = ws_set_meter_enabled,
    .set_level_format            = ws_set_level_format,
    .is_console_attached         = ws_is_console_attached,
};

const ms_client_iface_t *app_ms_client_ws(void) { return &s_iface; }
