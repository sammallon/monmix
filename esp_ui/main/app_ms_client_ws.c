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
//   * P3 all-strip-names sweep         — ws_fetch_all_strip_names is a
//     no-op; the UI's picker falls back to "CH NN" until WS broadcasts
//     populate names.
//   * P11 /console/mixTargets fetch    — ws_fetch_mix_routing no-op;
//     ws_is_mix_routed returns true for all indices.
//   * W6.1 channel routability fetch   — ws_fetch_channel_routability
//     no-op; ws_is_channel_routable returns true for all ids.
//   * #30 metering subscription        — ws_set_meter_enabled no-op;
//     UI handles missing meter samples (signal_indicator=meter renders
//     empty bars).
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
#include "app_prefs.h"
#include "app_state.h"

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
#include "freertos/semphr.h"

#include "mongoose.h"

static const char *TAG = "ms_ws";

#define WS_GET_PREFIX            "/console/data/get/"
#define WS_GET_PREFIX_LEN        (sizeof(WS_GET_PREFIX) - 1)
#define MAX_MIX_BUSES            24
#define WORKER_STACK             8192
#define WORKER_PRIO              5
#define MGR_POLL_MS              25
#define RECONNECT_RETRY_MS       2000

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

static void subscribe_all(void) {
    size_t n = app_state_count();
    for (size_t i = 0; i < n; ++i) {
        int id = app_state_id_for_idx(i);
        if (id >= 0) subscribe_channel(id, g.mix_idx);
    }
    subscribe_master(master_channel_id());
    subscribe_mix_names();
    g.subscribed = true;
}

// ────────────────────────────────────────────────────────────────────────────
// Inbound: route a "/console/data/get/<dotted>" broadcast
// ────────────────────────────────────────────────────────────────────────────

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

// ────────────────────────────────────────────────────────────────────────────
// ms_client_iface_t implementations
// ────────────────────────────────────────────────────────────────────────────

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
    g.mix_idx     = idx;
    g.subscribed  = false;
    sync_master_id_to_app_state();
    if (g.ws_open) subscribe_all();
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

static void ws_fetch_all_strip_names(int total)         { (void)total; /* TODO: P3 sweep */ }
static const char *ws_get_strip_name(int id) {
    // Fallback to "CH NN" until the WS broadcast for ch.<id>.cfg.name lands.
    static char buf[16];
    snprintf(buf, sizeof(buf), "CH %02d", id + 1);
    return buf;
}

static void ws_fetch_channel_routability(int total)     { (void)total; /* TODO: W6.1 */ }
static bool ws_is_channel_routable      (int id)        { (void)id; return true; }

static void ws_set_meter_enabled(bool on)               { (void)on; /* TODO: #30 metering */ }

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
};

const ms_client_iface_t *app_ms_client_ws(void) { return &s_iface; }
