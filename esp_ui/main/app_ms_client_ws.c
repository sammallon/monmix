#include "app_config.h"
#include "app_logd.h"
#include "app_ms_client.h"
#include "app_state.h"

#include "esp_attr.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ms_ws";

// Mixing Station protocol — verified against the offline test instance
// (see reference_mixing_station_protocol memory, repro_ms_probe*.py).
//
//   Single WS to ws://<host>:<port>/  carries subscribe + set as HTTP-style
//   JSON envelopes. Subscribe immediately emits the current value, then
//   re-emits on every change. Set snaps to the mixer's quantized step and
//   broadcasts the snapped value back over any active subscription.
//
//   We subscribe per-tracked-channel rather than wildcard — fewer messages
//   on stage WiFi, and it scales naturally to per-musician channel
//   selection later.
//
//   "Mix 1" in the MS UI = MIX index 0. Runtime-selectable via ws_set_mix;
//   defaults to 0 on boot.
#define WS_GET_PREFIX          "/console/data/get/"
#define WS_GET_PREFIX_LEN      (sizeof(WS_GET_PREFIX) - 1)

static int s_mix_bus_idx;

// Mix-bus layout — set by app_main from /console/information after WiFi
// associates. Mix scribble-strip names live at ch.<offset+i>.cfg.name and
// arrive via the same broadcast path as channel names. Names array goes
// to PSRAM (EXT_RAM_BSS_ATTR) — internal SRAM is tight enough that the
// 768 bytes of names alone can prevent the FreeRTOS timer task from
// allocating its stack at boot.
#define MAX_MIX_BUSES 24
static int  s_mix_offset;
static int  s_mix_count;
EXT_RAM_BSS_ATTR static char s_mix_names[MAX_MIX_BUSES][32];

// P11: per-mix routing flag from ch.<mix_offset+i>.info.isActive. Default
// true so a stale state never accidentally hides every mix before the
// REST fetch (or initial WS broadcast) populates it.
static bool s_mix_routed[MAX_MIX_BUSES] = { [0 ... MAX_MIX_BUSES - 1] = true };

static esp_websocket_client_handle_t s_ws;
static app_ms_state_t                s_state = APP_MS_STATE_BOOT;

// Reconnect-watchdog state. Per soak v4 logs, esp_websocket_client can wedge
// in a state where every internal retry fires EVENT_DISCONNECTED + EVENT_ERROR
// every ~15 s but never EVENT_CONNECTED, indefinitely. The watchdog detects
// this by tracking how long we've been in a non-CONNECTED state and forcibly
// destroys+recreates the client when it crosses the threshold.
//
// 60 s is a comfortable bound — esp_websocket_client's own reconnect timer is
// 5 s, so a healthy client should successfully reconnect within ~10 s. 60 s
// means we wait for ≥6 internal retries before escalating, avoiding spurious
// recreates during a brief network blip.
//
#define WS_STUCK_THRESHOLD_US (60LL * 1000 * 1000)

static int64_t  s_state_entered_us;
static uint32_t s_watchdog_recreations;

#define MAX_SUBSCRIBERS 4
static struct {
    app_ms_on_change_t cb;
    void              *ctx;
} s_subscribers[MAX_SUBSCRIBERS];
static size_t s_subscriber_count;

// P8: outstanding-SET tracker per channel. When a SET is emitted we stamp
// the time + value + retry-budget; when the matching server-snap echo for
// the same `lvl` path arrives we clear it. The poll watchdog scans for
// entries past the 500 ms deadline, fires a REST GET, and routes the
// returned value through app_state (so the existing dirty-sweep redraws).
// If REST fails too, we re-send the lost SET once before giving up.
//
// 500 ms is well above MS's normal echo latency (~10-50 ms in soak logs)
// and well below the threshold a user would notice as a stuck slider.
#define POLL_DEADLINE_US     (500LL * 1000)
#define POLL_SCAN_PERIOD_MS  100
#define POLL_HTTP_TIMEOUT_MS 1500
#define POLL_HTTP_BUF_LEN    256
#define POLL_RESEND_BUDGET   1

static int64_t s_pending_set_at_us[APP_CONFIG_MAX_CHANNELS];
static float   s_pending_set_value[APP_CONFIG_MAX_CHANNELS];
static int     s_pending_set_mix  [APP_CONFIG_MAX_CHANNELS];
static uint8_t s_pending_resends  [APP_CONFIG_MAX_CHANNELS];

// Shared sink for the small REST GETs (poll watchdog + P11 routing fetch).
typedef struct {
    char   buf[POLL_HTTP_BUF_LEN];
    size_t len;
} poll_sink_t;

static esp_err_t poll_http_event(esp_http_client_event_t *evt)
{
    poll_sink_t *sink = (poll_sink_t *) evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && sink && evt->data && evt->data_len > 0) {
        if (sink->len + (size_t) evt->data_len < sizeof(sink->buf)) {
            memcpy(sink->buf + sink->len, evt->data, evt->data_len);
            sink->len += evt->data_len;
        }
    }
    return ESP_OK;
}

static void notify_subscribers(void)
{
    for (size_t i = 0; i < s_subscriber_count; ++i) {
        if (s_subscribers[i].cb) s_subscribers[i].cb(s_subscribers[i].ctx);
    }
}

static void set_state(app_ms_state_t s)
{
    if (s_state == s) return;
    bool was_connected = (s_state == APP_MS_STATE_CONNECTED);
    bool was_boot      = (s_state == APP_MS_STATE_BOOT);
    s_state = s;
    // Only reset the watchdog age timer when we actually CROSS the
    // CONNECTED boundary (or on the first transition out of BOOT). The
    // soak v4 + #49 test data shows the state oscillates between
    // CONNECTING / DISCONNECTED / ERROR every ~15 s when the underlying
    // socket is dead — resetting on every bounce means the timer never
    // ages, so the watchdog never fires. Aging is what we actually want
    // to detect: "how long has it been since we last saw a successful
    // connection".
    if (s == APP_MS_STATE_CONNECTED || was_connected || was_boot) {
        s_state_entered_us = esp_timer_get_time();
    }
    notify_subscribers();
}

static bool ws_start(void);
static void ws_set_level(int ms_channel_id, float level);
static void ws_set_mute (int ms_channel_id, bool mute);
static void ws_set_name (int ms_channel_id, const char *name);
static void ws_set_master_level(float level);
static void ws_set_master_mute (bool mute);
static void ws_stop(void);
static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data);
static void send_envelope(const char *method, const char *path, const char *body_json);
static void subscribe_path(const char *dotted, const char *format);
static void on_connected_subscribe_all(void);
static void handle_broadcast(const char *json, size_t len);

static app_ms_state_t ws_get_state(void)             { return s_state; }
static const char    *ws_get_host(void)              { return app_config_ms_host(); }
static int            ws_get_port(void)              { return (int) app_config_ms_port(); }
static void           ws_register_on_change(app_ms_on_change_t cb, void *ctx)
{
    if (!cb || s_subscriber_count >= MAX_SUBSCRIBERS) return;
    s_subscribers[s_subscriber_count].cb  = cb;
    s_subscribers[s_subscriber_count].ctx = ctx;
    s_subscriber_count++;
}

static int ws_get_mix(void) { return s_mix_bus_idx; }

static void ws_set_mix_layout(int offset, int count)
{
    if (offset < 0)             offset = 0;
    if (count  < 0)             count  = 0;
    if (count  > MAX_MIX_BUSES) count  = MAX_MIX_BUSES;
    s_mix_offset = offset;
    s_mix_count  = count;
    // The actual ch.<offset+i>.cfg.name subscriptions happen in
    // on_connected_subscribe_all so they fire on first connect AND
    // every reconnect. app_main typically calls this before ws_start
    // — the layout just needs to be remembered; the WS event handler
    // does the rest.
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        on_connected_subscribe_all();
    }
}

static const char *ws_get_mix_name(int mix_idx)
{
    if (mix_idx < 0 || mix_idx >= s_mix_count) return NULL;
    if (s_mix_names[mix_idx][0] == '\0')        return NULL;
    return s_mix_names[mix_idx];
}

static bool ws_is_mix_routed(int mix_idx)
{
    if (mix_idx < 0 || mix_idx >= MAX_MIX_BUSES) return false;
    return s_mix_routed[mix_idx];
}

// P11: blocking REST GET of /console/mixTargets — the profile-aware list
// of mix buses the current MS user view exposes. Returns true on 200 +
// parseable body. Replaces the earlier per-mix info.isActive sweep,
// which on this Si console always returns true regardless of profile.
//
// Response is up to a few KB on a full-profile fetch (14 mix targets +
// 6 matrix + LR + Mono), so the per-poll 256-byte sink is too small;
// use a heap-allocated 4 KB buffer scoped to this call.
#define MIX_TARGETS_BUF_LEN 16384  // full-profile response on Si is ~6.5 KB; headroom
typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
} mt_sink_t;

static esp_err_t mt_http_event(esp_http_client_event_t *evt)
{
    mt_sink_t *sink = (mt_sink_t *) evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && sink && sink->buf) {
        size_t room = sink->cap - 1 - sink->len;
        if (room > 0) {
            size_t n = (size_t) evt->data_len < room ? (size_t) evt->data_len : room;
            memcpy(sink->buf + sink->len, evt->data, n);
            sink->len += n;
        }
    }
    return ESP_OK;
}

static bool fetch_mix_targets(bool routed_out[MAX_MIX_BUSES])
{
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%u/console/mixTargets",
             app_config_ms_host(), (unsigned) app_config_ms_port());

    mt_sink_t sink = { .buf = malloc(MIX_TARGETS_BUF_LEN),
                       .cap = MIX_TARGETS_BUF_LEN, .len = 0 };
    if (!sink.buf) return false;

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = mt_http_event,
        .user_data     = &sink,
        .timeout_ms    = POLL_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { free(sink.buf); return false; }

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK || status != 200 || sink.len == 0) {
        free(sink.buf);
        return false;
    }
    sink.buf[sink.len] = '\0';

    cJSON *root = cJSON_Parse(sink.buf);
    free(sink.buf);
    if (!root) return false;
    cJSON *targets = cJSON_GetObjectItem(root, "targets");
    bool ok = cJSON_IsArray(targets);
    if (ok) {
        memset(routed_out, 0, sizeof(bool) * MAX_MIX_BUSES);
        cJSON *t;
        cJSON_ArrayForEach(t, targets) {
            cJSON *jid = cJSON_GetObjectItem(t, "id");
            if (cJSON_IsNumber(jid)) {
                int mix_idx = (int) jid->valuedouble;
                if (mix_idx >= 0 && mix_idx < MAX_MIX_BUSES) {
                    routed_out[mix_idx] = true;
                }
            }
        }
    }
    cJSON_Delete(root);
    return ok;
}

// P11: one-shot fetch of the profile-filtered mix list. Called from
// app_main right after set_mix_layout so saved-index validation sees
// the routed mask before WS subscribes wire up. mixTargets reflects
// the active MS profile -- e.g., switching to a "mix master" profile
// reduces the list to just the assigned mix.
static void ws_fetch_mix_routing(void)
{
    if (s_mix_count <= 0) return;
    bool fresh[MAX_MIX_BUSES] = {0};
    if (!fetch_mix_targets(fresh)) {
        ESP_LOGW(TAG, "mixTargets fetch failed; assuming all routed");
        return;  // keep prior mask (defaults to all-true on first boot)
    }
    int routed = 0;
    for (int i = 0; i < s_mix_count && i < MAX_MIX_BUSES; ++i) {
        s_mix_routed[i] = fresh[i];
        if (fresh[i]) routed++;
    }
    ESP_LOGI(TAG, "mix routing: %d/%d in profile", routed, s_mix_count);
    notify_subscribers();
}

static bool ws_create_and_start(void);  // fwd

static void ws_reconnect(void)
{
    APP_LOGD_I("ms_ws", "reconnect requested (config change)");
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    ws_create_and_start();
    s_state_entered_us = esp_timer_get_time();
}

static void ws_resubscribe(void)
{
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        on_connected_subscribe_all();
    }
}

static void ws_set_mix(int mix_idx)
{
    if (mix_idx < 0)               return;
    if (s_mix_bus_idx == mix_idx)  return;
    s_mix_bus_idx = mix_idx;
    ESP_LOGI(TAG, "mix bus -> %d (Mix %d)", mix_idx, mix_idx + 1);
    // Re-subscribe per-channel under the new mix index. MS dedupes on
    // path, so the old mix's subscriptions remain active server-side and
    // continue broadcasting; handle_broadcast filters those by current
    // s_mix_bus_idx and silently drops them. True unsubscribe support is
    // a follow-up — the leak is bounded (N channels per mix selected this
    // session) and resets on WS reconnect.
    ws_resubscribe();
    notify_subscribers();
}

static const ms_client_iface_t s_iface = {
    .start              = ws_start,
    .set_level          = ws_set_level,
    .set_mute           = ws_set_mute,
    .set_name           = ws_set_name,
    .stop               = ws_stop,
    .get_state          = ws_get_state,
    .get_host           = ws_get_host,
    .get_port           = ws_get_port,
    .register_on_change = ws_register_on_change,
    .get_mix            = ws_get_mix,
    .set_mix            = ws_set_mix,
    .set_mix_layout     = ws_set_mix_layout,
    .get_mix_name       = ws_get_mix_name,
    .is_mix_routed      = ws_is_mix_routed,
    .fetch_mix_routing  = ws_fetch_mix_routing,
    .resubscribe        = ws_resubscribe,
    .reconnect          = ws_reconnect,
    .set_master_level   = ws_set_master_level,
    .set_master_mute    = ws_set_master_mute,
};

const ms_client_iface_t *app_ms_client_ws(void)
{
    return &s_iface;
}

// Build + start the websocket client. Shared by ws_start (initial bring-up)
// and the watchdog (forced recreate after a stuck disconnect). Caller owns
// the prior s_ws lifecycle if any.
static bool ws_create_and_start(void)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "ws://%s:%u/",
             app_config_ms_host(), (unsigned) app_config_ms_port());

    esp_websocket_client_config_t cfg = {
        .uri                  = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
        // Two layers of liveness probing:
        // 1) TCP keepalive: catches half-open sockets where the kernel
        //    on the far side is gone but no RST was ever delivered. With
        //    idle=10 / interval=5 / count=3 the kernel declares the
        //    socket dead ~25 s after traffic stops.
        // 2) WS ping/pong: catches the case where the TCP socket is
        //    fine but the server-side WS handler is wedged. The client
        //    sends PING every 10 s; if no PONG arrives within 20 s, the
        //    client disconnects (which our watchdog then notices).
        // Either path winds up firing EVENT_DISCONNECTED, which starts
        // the watchdog's 60 s recovery clock. Without these, half-open
        // detection is at the mercy of TCP retransmit timeout (often
        // minutes) — observed during the host's Modern Standby in soak
        // v4 where the device stayed "connected" long after the network
        // pipe was effectively dead.
        .keep_alive_enable    = true,
        .keep_alive_idle      = 10,
        .keep_alive_interval  = 5,
        .keep_alive_count     = 3,
        .ping_interval_sec    = 10,
        .pingpong_timeout_sec = 20,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "ws init failed");
        return false;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);

    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ws start failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "ws starting -> %s", uri);
    set_state(APP_MS_STATE_CONNECTING);
    return true;
}

// Watchdog escalation — invoked when a non-CONNECTED state has persisted
// past WS_STUCK_THRESHOLD_US. Tears the client down and recreates it from
// scratch. Soak v4 showed esp_websocket_client can sit in an infinite
// internal retry loop where every retry fires DISCONNECTED+ERROR but never
// CONNECTED; only a fresh client recovers.
static void ws_force_recreate(void)
{
    int64_t age_ms = (esp_timer_get_time() - s_state_entered_us) / 1000;
    APP_LOGD_W("ms_ws", "watchdog: recreating client (state=%d age=%lldms count=%u)",
               (int) s_state, (long long) age_ms,
               (unsigned)(s_watchdog_recreations + 1));
    ESP_LOGW(TAG, "watchdog: recreating client (state=%d age=%lldms)",
             (int) s_state, (long long) age_ms);

    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_watchdog_recreations++;

    // Force a WiFi re-association through the C6. Field testing showed
    // that a fresh esp_websocket_client + new TCP connect alone doesn't
    // recover the wedged state -- only a full chip reset did. The most
    // likely culprit is ESP-Hosted's per-association socket bookkeeping
    // on the C6 side; bouncing the STA association re-handshakes with
    // the AP through the C6, which clears that state without needing a
    // full reset. The wifi event handler in app_wifi.c will set the UI
    // back to CONNECTING, then CONNECTED once the IP is reassigned. The
    // watchdog blocks for the full reassociate (~3-5 s typical) so the
    // new WS client below is created against a freshly-up wifi.
    APP_LOGD_W("ms_ws", "watchdog: forcing wifi re-associate before WS recreate");
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_connect();
    // Wait for the wifi handler to flip back to CONNECTED (got_ip event)
    // before we try the WS connect, otherwise the new client will fire a
    // burst of TCP_TRANSPORT errors against a still-disassociated radio.
    for (int i = 0; i < 60; ++i) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ws_create_and_start();
    // Give the freshly-created client a full WS_STUCK_THRESHOLD_US window
    // to actually establish a connection before we'd recreate again. Without
    // this, the watchdog fires every 5 s once the threshold is crossed (the
    // CONNECTING state set by ws_create_and_start doesn't reset the age
    // timer per set_state's "only on CONNECTED-boundary" rule). Saw 24
    // recreates in a row during #49 because of this — each one usable but
    // wasteful.
    s_state_entered_us = esp_timer_get_time();
}

static void ws_watchdog_task(void *arg)
{
    (void) arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        // CONNECTED is the success state; BOOT means we haven't tried yet.
        // CONNECTING or DISCONNECTED or ERROR for >threshold means stuck.
        if (s_state == APP_MS_STATE_CONNECTED || s_state == APP_MS_STATE_BOOT)
            continue;
        int64_t age_us = esp_timer_get_time() - s_state_entered_us;
        if (age_us > WS_STUCK_THRESHOLD_US) {
            ws_force_recreate();
        }
    }
}

// P8: REST GET the current `lvl` for one channel/mix and parse the value.
// Returns true + writes *out_level on 200 OK with a numeric body.value.
static bool poll_fetch_level(int ms_channel_id, int mix_idx, float *out_level)
{
    char url[128];
    snprintf(url, sizeof(url),
             "http://%s:%u/console/data/get/ch.%d.levelData.%d.lvl/norm",
             app_config_ms_host(), (unsigned) app_config_ms_port(),
             ms_channel_id, mix_idx);

    poll_sink_t sink = {0};
    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = poll_http_event,
        .user_data     = &sink,
        .timeout_ms    = POLL_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200 || sink.len == 0) {
        APP_LOGD_W("ms_ws", "poll GET failed ch=%d mix=%d err=%s status=%d",
                   ms_channel_id, mix_idx, esp_err_to_name(err), status);
        return false;
    }
    sink.buf[sink.len < sizeof(sink.buf) ? sink.len : sizeof(sink.buf) - 1] = '\0';

    cJSON *root = cJSON_Parse(sink.buf);
    if (!root) return false;
    cJSON *jv = cJSON_GetObjectItem(root, "value");
    bool ok = cJSON_IsNumber(jv);
    if (ok) *out_level = (float) jv->valuedouble;
    cJSON_Delete(root);
    return ok;
}

// P8: scan the outstanding-SET tracker, escalate any past-deadline entry
// via REST poll. On REST success the corrected value flows back through
// app_state_set_level (notify=true) → existing dirty-flag sweep redraws
// the slider. On REST failure, re-send the lost SET once. Either path
// clears the tracker so we don't loop on the same slot.
static void poll_watchdog_scan(void)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    int64_t now = esp_timer_get_time();
    size_t total = app_state_count();
    for (size_t i = 0; i < total && i < APP_CONFIG_MAX_CHANNELS; ++i) {
        int64_t at = s_pending_set_at_us[i];
        if (at == 0) continue;
        if (now - at < POLL_DEADLINE_US) continue;

        int ch_id = app_state_id_for_idx(i);
        int mix   = s_pending_set_mix[i];
        float sent = s_pending_set_value[i];
        // Clear early so a new SET arriving mid-scan doesn't get clobbered
        // by the value we read here. If the poll/resend fails to land we
        // accept the gap — the next user touch re-arms.
        s_pending_set_at_us[i] = 0;

        if (ch_id < 0) continue;

        float board = 0.0f;
        if (poll_fetch_level(ch_id, mix, &board)) {
            ESP_LOGI(TAG, "ch %d: poll resync (sent=%.3f -> board=%.3f)",
                     ch_id, (double) sent, (double) board);
            APP_LOGD_I("ms_ws", "poll resync ch=%d mix=%d sent=%.3f board=%.3f",
                       ch_id, mix, (double) sent, (double) board);
            // Server-snap is source of truth — route through app_state with
            // notify=true so the dirty-sweep redraws the slider, exactly
            // like a normal WS broadcast would.
            app_state_set_level(i, board, true);
        } else if (s_pending_resends[i] < POLL_RESEND_BUDGET) {
            // Tertiary fallback: re-send the SET. Bounded to one retry to
            // avoid pinning the WS task on a stuck connection.
            uint8_t next_try = s_pending_resends[i] + 1;
            ESP_LOGW(TAG, "ch %d: poll failed, re-sending SET %.3f (try %u)",
                     ch_id, (double) sent, (unsigned) next_try);
            APP_LOGD_W("ms_ws", "poll failed ch=%d resend=%.3f try=%u",
                       ch_id, (double) sent, (unsigned) next_try);
            ws_set_level(ch_id, sent);
            // ws_set_level rearms the tracker with retries=0 — restore the
            // budget counter so we honor POLL_RESEND_BUDGET across the
            // retry window instead of looping forever.
            s_pending_resends[i] = next_try;
        } else {
            ESP_LOGW(TAG, "ch %d: poll + resend exhausted, giving up", ch_id);
            APP_LOGD_W("ms_ws", "poll exhausted ch=%d", ch_id);
        }
    }
}

// Separate task from ws_watchdog_task because the cadences differ by 50x
// (5 s vs 100 ms) and the poll task issues blocking HTTP calls. Stack is
// sized for esp_http_client + cJSON parse on the small response body.
static void poll_watchdog_task(void *arg)
{
    (void) arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_SCAN_PERIOD_MS));
        poll_watchdog_scan();
    }
}

static bool ws_start(void)
{
    if (!ws_create_and_start()) return false;

    // Spin up the reconnect watchdog once. ws_start is called once from
    // app_main; if that ever changes we'd need a one-shot guard here.
    xTaskCreate(ws_watchdog_task,   "ms_ws_wdt",   4096, NULL, 5, NULL);
    // Poll watchdog: catches dropped/missing SET echoes and resyncs the UI
    // to the board's actual value (or re-sends the SET). 5 KB stack covers
    // esp_http_client + cJSON_Parse on the ~80-byte response body.
    xTaskCreate(poll_watchdog_task, "ms_ws_poll",  5120, NULL, 4, NULL);
    return true;
}

static void ws_set_level(int ms_channel_id, float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "set_level dropped: ws not connected");
        return;
    }

    char path[80];
    snprintf(path, sizeof(path),
             "/console/data/set/ch.%d.levelData.%d.lvl/norm",
             ms_channel_id, s_mix_bus_idx);

    char body[48];
    snprintf(body, sizeof(body), "{\"value\":%.6f}", (double)level);

    send_envelope("POST", path, body);

    // Arm the outstanding-SET tracker. Cleared in handle_broadcast on the
    // matching `lvl` echo, escalated by the poll watchdog otherwise. Stamp
    // by slot index, not channel id, so reordered slots stay aligned with
    // the broadcast handler's lookup. Reset retry budget on every fresh
    // user-driven SET so a re-send earlier in the session doesn't poison
    // a later legit drop.
    int idx = app_state_idx_for_id(ms_channel_id);
    if (idx >= 0 && idx < APP_CONFIG_MAX_CHANNELS) {
        s_pending_set_at_us[idx] = esp_timer_get_time();
        s_pending_set_value[idx] = level;
        s_pending_set_mix  [idx] = s_mix_bus_idx;
        s_pending_resends  [idx] = 0;
    }
}

// Master = the mix-bus's own channel id (s_mix_offset + s_mix_bus_idx).
// Returns -1 when the mix layout hasn't been set yet (boot before
// /console/information arrives).
static int master_channel_id(void)
{
    if (s_mix_count <= 0)              return -1;
    if (s_mix_bus_idx >= s_mix_count)  return -1;
    return s_mix_offset + s_mix_bus_idx;
}

static void ws_set_master_level(float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    int id = master_channel_id();
    if (id < 0) return;

    char path[64];
    snprintf(path, sizeof(path),
             "/console/data/set/ch.%d.mix.lvl/norm", id);
    char body[48];
    snprintf(body, sizeof(body), "{\"value\":%.6f}", (double)level);
    send_envelope("POST", path, body);
}

static void ws_set_master_mute(bool mute)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    int id = master_channel_id();
    if (id < 0) return;

    char path[64];
    snprintf(path, sizeof(path),
             "/console/data/set/ch.%d.mix.on/val", id);
    // MS `.on` true = audible; we expose `mute` as the user-facing bool
    // (true = silenced) so flip on the wire — same convention as input
    // strips, see ws_set_mute.
    const char *body = mute ? "{\"value\":false}" : "{\"value\":true}";
    send_envelope("POST", path, body);
}

static void ws_set_mute(int ms_channel_id, bool mute)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "set_mute dropped: ws not connected");
        return;
    }

    char path[80];
    snprintf(path, sizeof(path),
             "/console/data/set/ch.%d.levelData.%d.on/val",
             ms_channel_id, s_mix_bus_idx);

    // MS `.on` is a bool: true = audible (NOT muted), false = muted. We
    // expose `mute` to the rest of the firmware as the user-facing boolean
    // (true = "this channel is silenced"), so flip it on the wire.
    const char *body = mute ? "{\"value\":false}" : "{\"value\":true}";
    send_envelope("POST", path, body);
}

static void ws_set_name(int ms_channel_id, const char *name)
{
    if (!name) return;
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "set_name dropped: ws not connected");
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), "/console/data/set/ch.%d.cfg.name/val", ms_channel_id);

    // Build the JSON body via cJSON so the name string is escaped properly
    // (quotes, backslashes, control chars) — a hand-rolled snprintf would
    // miscompose any name with a quote in it.
    cJSON *body = cJSON_CreateObject();
    if (!body) return;
    cJSON_AddStringToObject(body, "value", name);
    char *body_text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_text) return;

    send_envelope("POST", path, body_text);
    free(body_text);
}

static void ws_stop(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
}

static void send_envelope(const char *method, const char *path, const char *body_json)
{
    char frame[256];
    int n = snprintf(frame, sizeof(frame),
                     "{\"method\":\"%s\",\"path\":\"%s\",\"body\":%s}",
                     method, path, body_json);
    if (n <= 0 || n >= (int)sizeof(frame)) {
        ESP_LOGE(TAG, "envelope too large for path %s", path);
        APP_LOGD_E("ms_ws", "envelope too large path=%s", path);
        return;
    }
    APP_LOGD_T("ms_ws", "tx %s %s", method, path);
    esp_websocket_client_send_text(s_ws, frame, n, portMAX_DELAY);
}

static void subscribe_path(const char *dotted, const char *format)
{
    char body[80];
    snprintf(body, sizeof(body),
             "{\"path\":\"%s\",\"format\":\"%s\"}", dotted, format);
    send_envelope("POST", "/console/data/subscribe", body);
}

static void on_connected_subscribe_all(void)
{
    // For each tracked channel, subscribe to its fader (norm 0..1), scribble
    // strip name, and mute state. The initial-value broadcasts populate
    // app_state before the user touches anything.
    for (size_t i = 0; i < app_state_count(); ++i) {
        int ch_id = app_state_id_for_idx(i);
        if (ch_id < 0) continue;

        char dotted[48];
        snprintf(dotted, sizeof(dotted),
                 "ch.%d.levelData.%d.lvl", ch_id, s_mix_bus_idx);
        subscribe_path(dotted, "norm");

        // Same level node, different alias + format → MS sends dB. We need
        // both because norm drives the slider linearly and dB drives the
        // user-facing readout (with non-linear MS-specific mapping that we
        // can't compute locally).
        snprintf(dotted, sizeof(dotted),
                 "ch.%d.levelData.%d.level", ch_id, s_mix_bus_idx);
        subscribe_path(dotted, "val");

        snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", ch_id);
        subscribe_path(dotted, "val");

        snprintf(dotted, sizeof(dotted),
                 "ch.%d.levelData.%d.on", ch_id, s_mix_bus_idx);
        subscribe_path(dotted, "val");
    }

    // Mix-bus scribble-strip names — subscribed here, not in
    // ws_set_mix_layout, because app_main calls set_mix_layout BEFORE
    // ws_start (the layout is known from /console/information well
    // before WS opens). Doing it here means the names also get
    // re-subscribed on every reconnect for free.
    for (int i = 0; i < s_mix_count; ++i) {
        char dotted[32];
        snprintf(dotted, sizeof(dotted), "ch.%d.cfg.name", s_mix_offset + i);
        subscribe_path(dotted, "val");
    }
    // P11: profile-filtered routed mask comes from /console/mixTargets,
    // not from per-channel info.isActive (which on the Si Expression
    // returns true for all 20 buses regardless of MS profile). Re-fetch
    // it on every connect so a profile switch in MS picks up after a
    // reconnect without a device reboot.
    bool fresh[MAX_MIX_BUSES] = {0};
    if (fetch_mix_targets(fresh)) {
        int routed = 0;
        for (int i = 0; i < s_mix_count && i < MAX_MIX_BUSES; ++i) {
            s_mix_routed[i] = fresh[i];
            if (fresh[i]) routed++;
        }
        ESP_LOGI(TAG, "subscribe-time routing: %d/%d in profile", routed, s_mix_count);
        notify_subscribers();
    }

    // Master strip — the mix bus's own output. Path shape differs from
    // input strips: `ch.<N>.mix.lvl` (norm only — there's no `level`
    // alias on the master, see repro_ms_master_fader.py findings) and
    // `ch.<N>.mix.on`. Master `cfg.name` subscription is already covered
    // by the mix-name loop above. Re-aimed at the new id on every
    // set_mix via ws_resubscribe → here.
    int master_id = master_channel_id();
    if (master_id >= 0) {
        char dotted[32];
        snprintf(dotted, sizeof(dotted), "ch.%d.mix.lvl", master_id);
        subscribe_path(dotted, "norm");
        snprintf(dotted, sizeof(dotted), "ch.%d.mix.on", master_id);
        subscribe_path(dotted, "val");
        // Hand the id to app_state so the master-state's mute/level
        // notifications carry the right channel context. Clears stale
        // values on a mix change.
        app_state_master_set_id(master_id);
    }

    ESP_LOGI(TAG, "subscribed %d channels + %d mix names + master(ch.%d)",
             (int) app_state_count(), s_mix_count, master_id);
}

static void handle_broadcast(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;

    cJSON *jpath = cJSON_GetObjectItem(root, "path");
    cJSON *jbody = cJSON_GetObjectItem(root, "body");
    cJSON *jerr  = cJSON_GetObjectItem(root, "error");

    if (cJSON_IsString(jerr)) {
        ESP_LOGW(TAG, "ms error: %s", jerr->valuestring);
        cJSON_Delete(root);
        return;
    }

    if (!cJSON_IsString(jpath) || !cJSON_IsObject(jbody)) {
        cJSON_Delete(root);
        return;
    }

    // Strip the "/console/data/get/" prefix. Anything that doesn't carry it
    // isn't a value broadcast we care about.
    const char *p = jpath->valuestring;
    if (strncmp(p, WS_GET_PREFIX, WS_GET_PREFIX_LEN) != 0) {
        cJSON_Delete(root);
        return;
    }
    const char *dotted = p + WS_GET_PREFIX_LEN;

    cJSON *jvalue = cJSON_GetObjectItem(jbody, "value");

    // Single-pass dispatch on the trailing key. sscanf with `== N` checks
    // ONLY confirm that the %d conversions filled — the rest of the format
    // string can still mismatch and sscanf returns the same count, so a
    // path like `levelData.0.level` would falsely match
    // `levelData.%d.lvl`. Matching the prefix once and switching on the
    // suffix avoids that whole class of bug.
    int  ch = 0, mix = 0;
    char suffix[16] = {0};
    if (sscanf(dotted, "ch.%d.levelData.%d.%15s", &ch, &mix, suffix) == 3 &&
        mix == s_mix_bus_idx) {
        int idx = app_state_idx_for_id(ch);
        if (idx >= 0) {
            if (strcmp(suffix, "lvl") == 0 && cJSON_IsNumber(jvalue)) {
                // Echo arrived — clear the outstanding-SET tracker for
                // this slot so the poll watchdog leaves it alone.
                if (idx < APP_CONFIG_MAX_CHANNELS) {
                    s_pending_set_at_us[idx] = 0;
                }
                app_state_set_level((size_t)idx, (float)jvalue->valuedouble, true);
            } else if (strcmp(suffix, "level") == 0 && cJSON_IsNumber(jvalue)) {
                app_state_set_level_db((size_t)idx, (float)jvalue->valuedouble, true);
            } else if (strcmp(suffix, "on") == 0 && cJSON_IsBool(jvalue)) {
                // MS `.on` true = audible, false = muted. Flip to our
                // user-facing boolean (true = "this channel is silenced").
                bool ms_on = cJSON_IsTrue(jvalue);
                app_state_set_mute((size_t)idx, !ms_on, true);
            }
        }
        cJSON_Delete(root);
        return;
    }

    if (sscanf(dotted, "ch.%d.cfg.name", &ch) == 1 && cJSON_IsString(jvalue)) {
        int idx = app_state_idx_for_id(ch);
        if (idx >= 0) {
            app_state_set_name((size_t)idx, jvalue->valuestring, true);
        }
        // Mix scribble-strip names use the same path; if this ch falls in
        // the mix range, cache the name for the selector popup.
        if (s_mix_count > 0 && ch >= s_mix_offset &&
            ch < s_mix_offset + s_mix_count) {
            int mix_idx = ch - s_mix_offset;
            strncpy(s_mix_names[mix_idx], jvalue->valuestring,
                    sizeof(s_mix_names[mix_idx]) - 1);
            s_mix_names[mix_idx][sizeof(s_mix_names[mix_idx]) - 1] = '\0';
            notify_subscribers();
        }
        // Master strip name follows the active mix's cfg.name. The cache
        // above keeps the picker labels current; this branch keeps the
        // visible master strip's name in sync.
        if (ch == master_channel_id()) {
            app_state_master_set_name(jvalue->valuestring, true);
        }
    }

    // Master strip values: ch.<N>.mix.<lvl|on>. Filter by current master id
    // — old subs from a prior mix can still fire (true unsubscribe is a
    // follow-up, same caveat as the per-channel re-subscribe note in
    // ws_set_mix). lvl arrives as norm only; master has no `level` alias
    // (verified in repro_ms_master_fader.py) so dB-format readout falls
    // back to the slider percent in app_ui.
    if (sscanf(dotted, "ch.%d.mix.%15s", &ch, suffix) == 2 &&
        ch == master_channel_id()) {
        if (strcmp(suffix, "lvl") == 0 && cJSON_IsNumber(jvalue)) {
            app_state_master_set_level((float)jvalue->valuedouble, true);
        } else if (strcmp(suffix, "on") == 0 && cJSON_IsBool(jvalue)) {
            bool ms_on = cJSON_IsTrue(jvalue);
            app_state_master_set_mute(!ms_on, true);
        }
    }

    cJSON_Delete(root);
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *evt = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected");
        APP_LOGD_I("ms_ws", "connected to %s:%u",
                   app_config_ms_host(), (unsigned) app_config_ms_port());
        set_state(APP_MS_STATE_CONNECTED);
        on_connected_subscribe_all();
        break;
    }

    case WEBSOCKET_EVENT_DATA:
        if (evt->op_code == 0x1 /* text */ && evt->data_len > 0) {
            APP_LOGD_T("ms_ws", "rx %.*s",
                       (int) (evt->data_len > 90 ? 90 : evt->data_len),
                       evt->data_ptr);
            handle_broadcast(evt->data_ptr, (size_t)evt->data_len);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        APP_LOGD_W("ms_ws", "disconnected");
        set_state(APP_MS_STATE_DISCONNECTED);
        break;

    case WEBSOCKET_EVENT_ERROR:
        // Surface the underlying errno + handshake status so post-mortem can
        // tell us whether the recreate's connect attempt failed at TCP
        // (ECONNREFUSED / EHOSTUNREACH / ETIMEDOUT etc.), at HTTP upgrade
        // (non-101 status), or somewhere else. Without this we only see the
        // event fired with no context.
        ESP_LOGE(TAG, "error");
        if (evt) {
            APP_LOGD_E("ms_ws", "error event "
                       "type=%d sock_errno=%d handshake_status=%d "
                       "tls_err=%d tls_stack=%d",
                       (int) evt->error_handle.error_type,
                       (int) evt->error_handle.esp_transport_sock_errno,
                       (int) evt->error_handle.esp_ws_handshake_status_code,
                       (int) evt->error_handle.esp_tls_last_esp_err,
                       (int) evt->error_handle.esp_tls_stack_err);
        } else {
            APP_LOGD_E("ms_ws", "error event (no data)");
        }
        set_state(APP_MS_STATE_ERROR);
        break;

    default:
        break;
    }
}
