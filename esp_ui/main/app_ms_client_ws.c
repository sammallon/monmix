#include "app_config.h"
#include "app_logd.h"
#include "app_ms_client.h"
#include "app_ms_info.h"
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
#include "mbedtls/base64.h"

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

// P5: true once we have a non-empty mix-bus layout AND a live WS connection.
// Cleared on disconnect so the next reconnect rebuilds it (and on the
// reconnect-while-MS-was-unreachable-at-boot case, on_ws_event re-fetches
// /console/information to populate s_mix_count before flipping this flag).
// The UI's mix-indicator visibility AND-gates this with the WS connected
// state — see mix_indicator_apply_visibility in app_ui.c.
static volatile bool s_mix_list_received;

// P11: per-mix routing flag from ch.<mix_offset+i>.info.isActive. Default
// true so a stale state never accidentally hides every mix before the
// REST fetch (or initial WS broadcast) populates it.
static bool s_mix_routed[MAX_MIX_BUSES] = { [0 ... MAX_MIX_BUSES - 1] = true };

// P3: cache of every strip's scribble-strip name. The channel picker
// overlay shows ALL strips on the connected console, not just the ones
// tracked in app_state, so we need a name slot per MS channel id rather
// than per app_state index. Sized to match APP_UI_MAX_PICKER_ROWS (128)
// -- this header isn't included here to keep the layering one-way, so
// the constant is duplicated. Si Expression reports 80; 128 covers all
// reasonable consoles. PSRAM-backed: ~4 KB.
#define MS_MAX_STRIP_NAMES 128
EXT_RAM_BSS_ATTR static char s_all_strip_names[MS_MAX_STRIP_NAMES][32];

// W6.1: per-channel routability mask. ch.<n>.levelData.0.lvl returns 200
// for input/aux/stereo channels (routable to a mix bus) and 404 for
// mix/matrix/main strips (can't be routed onto a mix bus -- mixes can't
// contain themselves or other mixes). Default true so a never-fetched
// state shows everything rather than hiding everything.
static bool s_channel_routable[MS_MAX_STRIP_NAMES] = {0};
static bool s_channel_routable_inited = false;

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

// Cached level-format pref. Drives which format we subscribe `lvl` and
// `mix.lvl` paths in -- one subscription per channel, no dual-sub, no
// REST polling. NORM gets `lvl/norm` (audio-tapered slider matching MS);
// DB gets `lvl/val` (linear-in-dB slider, dB readout direct from sub).
// Initialized from prefs in ws_start; updated by ws_set_level_format on
// user toggle, which re-subscribes every tracked channel + master.
static app_level_format_t s_level_format = APP_LEVEL_FORMAT_NORM;

// #30: realtime metering. Single /console/metering2 subscription that
// covers every tracked channel. Server paces at the requested interval
// (verified 10 Hz with INTERVAL_MS=100 against the offline MS instance,
// see tools/repro_ms_metering.py). Type=0 = mono peak, one int16 per
// channel in the same order as params[]. Decoded values land in
// app_state via app_state_set_meter_db; the UI redraws via the
// existing dirty-flag sweep.
//
// One subscription id covers all tracked channels — keeps the wire
// traffic to ~10 frames/sec total instead of N times that.
#define METERING_SUB_ID       1
#define METERING_INTERVAL_MS  100
// MAX_PARAMS bounded by APP_CONFIG_MAX_CHANNELS so the params[] array
// never overruns.  Slot order matches app_state slot order at subscribe
// time; decode unpacks into the same slots.
static bool s_meter_subscribed;
static int  s_meter_param_ids[APP_CONFIG_MAX_CHANNELS];   // ms_channel_id per slot
static int  s_meter_param_count;

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
static void ws_set_level_format(app_level_format_t f);
static int  master_channel_id  (void);
static void ws_set_meter_enabled(bool on);
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
    // P5: if WS is already connected when the layout lands (boot ordering
    // or a re-fetch after the user updates MS host/port), flip the
    // received flag now and notify so the UI's mix-indicator gate can
    // refresh. Otherwise wait for WEBSOCKET_EVENT_CONNECTED.
    if (count > 0 && s_ws && esp_websocket_client_is_connected(s_ws)) {
        s_mix_list_received = true;
        notify_subscribers();
    } else if (count == 0) {
        // Caller cleared the layout (rare). Hide the gate.
        s_mix_list_received = false;
        notify_subscribers();
    }
    // The actual ch.<offset+i>.cfg.name subscriptions happen in
    // on_connected_subscribe_all so they fire on first connect AND
    // every reconnect. app_main typically calls this before ws_start
    // — the layout just needs to be remembered; the WS event handler
    // does the rest.
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        on_connected_subscribe_all();
    }
}

static bool ws_is_mix_list_ready(void)
{
    return s_mix_list_received;
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

// P3: blocking REST GET of one ch.<n>.cfg.name. Writes into the all-strip
// cache slot directly on success. Failures are silent -- the slot stays
// empty and the picker falls back to "CH NN" until the WS broadcast (or
// a later sweep) populates it.
static void fetch_one_strip_name(int ch_id)
{
    if (ch_id < 0 || ch_id >= MS_MAX_STRIP_NAMES) return;

    char url[128];
    snprintf(url, sizeof(url),
             "http://%s:%u/console/data/get/ch.%d.cfg.name/val",
             app_config_ms_host(), (unsigned) app_config_ms_port(), ch_id);

    poll_sink_t sink = {0};
    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = poll_http_event,
        .user_data     = &sink,
        .timeout_ms    = POLL_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return;

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK || status != 200 || sink.len == 0) return;
    sink.buf[sink.len < sizeof(sink.buf) ? sink.len : sizeof(sink.buf) - 1] = '\0';

    cJSON *root = cJSON_Parse(sink.buf);
    if (!root) return;
    cJSON *jv = cJSON_GetObjectItem(root, "value");
    if (cJSON_IsString(jv) && jv->valuestring) {
        strncpy(s_all_strip_names[ch_id], jv->valuestring,
                sizeof(s_all_strip_names[ch_id]) - 1);
        s_all_strip_names[ch_id][sizeof(s_all_strip_names[ch_id]) - 1] = '\0';
    }
    cJSON_Delete(root);
}

// P3: one-shot REST sweep of every strip's name. Called from app_main once
// after set_mix_layout, before ws_start, so the channel picker shows
// MS-side names from first open. WS subscribe path keeps the cache live
// for the tracked channels; renames on un-tracked strips are picked up
// at next reboot's sweep (acceptable -- the picker is rarely opened and
// MS rename is rare).
static void ws_fetch_all_strip_names(int total)
{
    if (total < 0) return;
    if (total > MS_MAX_STRIP_NAMES) total = MS_MAX_STRIP_NAMES;
    int populated = 0;
    for (int i = 0; i < total; ++i) {
        fetch_one_strip_name(i);
        if (s_all_strip_names[i][0]) populated++;
    }
    ESP_LOGI(TAG, "fetched %d/%d strip names", populated, total);
    notify_subscribers();
}

static const char *ws_get_strip_name(int ms_channel_id)
{
    if (ms_channel_id < 0 || ms_channel_id >= MS_MAX_STRIP_NAMES) return NULL;
    if (s_all_strip_names[ms_channel_id][0] == '\0') return NULL;
    return s_all_strip_names[ms_channel_id];
}

// W6.1: probe one channel for routability via REST. 200 -> routable,
// 404 -> not (mix/matrix/main). 404 is a normal negative answer here, not
// an error -- treat it as the signal, log louder cases only.
static bool fetch_one_channel_routable(int ch_id)
{
    char url[128];
    snprintf(url, sizeof(url),
             "http://%s:%u/console/data/get/ch.%d.levelData.0.lvl/val",
             app_config_ms_host(), (unsigned) app_config_ms_port(), ch_id);

    poll_sink_t sink = {0};
    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = poll_http_event,
        .user_data     = &sink,
        .timeout_ms    = POLL_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return true;  // network blip -> don't hide the channel

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK) return true;  // transient: keep visible
    if (status == 404) return false;
    return true;  // 200 (or anything else non-404): treat as routable
}

static void ws_fetch_channel_routability(int total)
{
    if (total < 0) return;
    if (total > MS_MAX_STRIP_NAMES) total = MS_MAX_STRIP_NAMES;
    int routable = 0;
    for (int i = 0; i < total; ++i) {
        s_channel_routable[i] = fetch_one_channel_routable(i);
        if (s_channel_routable[i]) routable++;
    }
    s_channel_routable_inited = true;
    ESP_LOGI(TAG, "channel routability: %d/%d routable to mix", routable, total);
    notify_subscribers();
}

static bool ws_is_channel_routable(int ms_channel_id)
{
    if (ms_channel_id < 0 || ms_channel_id >= MS_MAX_STRIP_NAMES) return true;
    if (!s_channel_routable_inited) return true;
    return s_channel_routable[ms_channel_id];
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
    .is_mix_list_ready  = ws_is_mix_list_ready,
    .resubscribe        = ws_resubscribe,
    .reconnect          = ws_reconnect,
    .set_master_level      = ws_set_master_level,
    .set_master_mute       = ws_set_master_mute,
    .fetch_all_strip_names = ws_fetch_all_strip_names,
    .get_strip_name        = ws_get_strip_name,
    .fetch_channel_routability = ws_fetch_channel_routability,
    .is_channel_routable       = ws_is_channel_routable,
    .set_meter_enabled     = ws_set_meter_enabled,
    .set_level_format      = ws_set_level_format,
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

// P8: REST GET the current `lvl` for one channel/mix and parse the value
// as slider position 0..1. Format-aware: in DB mode we GET /val (dB) and
// convert, in NORM mode we GET /norm directly. Returns true on 200 OK
// with a numeric body.value.
static bool poll_fetch_level(int ms_channel_id, int mix_idx, float *out_position)
{
    const char *fmt = (s_level_format == APP_LEVEL_FORMAT_DB) ? "val" : "norm";
    char url[128];
    snprintf(url, sizeof(url),
             "http://%s:%u/console/data/get/ch.%d.levelData.%d.lvl/%s",
             app_config_ms_host(), (unsigned) app_config_ms_port(),
             ms_channel_id, mix_idx, fmt);

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
    if (ok) {
        float v = (float) jv->valuedouble;
        if (s_level_format == APP_LEVEL_FORMAT_DB) {
            *out_position = app_db_to_position(v);
        } else {
            *out_position = v;
        }
    }
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
            // like a normal WS broadcast would. board is slider position
            // 0..1; in DB mode convert to dB before storing in level_db.
            if (s_level_format == APP_LEVEL_FORMAT_DB) {
                float db = app_position_to_db(board);
                app_state_set_level_db(i, db, true);
            } else {
                app_state_set_level(i, board, true);
            }
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

    // Snapshot the current level-format so the first on_connected_subscribe_all
    // picks the right set of subscriptions. Live toggles afterward go
    // through ws_set_level_format.
    s_level_format = app_prefs_get_level_format();

    // Spin up the reconnect watchdog once. ws_start is called once from
    // app_main; if that ever changes we'd need a one-shot guard here.
    xTaskCreate(ws_watchdog_task,   "ms_ws_wdt",   4096, NULL, 5, NULL);
    // Poll watchdog: catches dropped/missing SET echoes and resyncs the UI
    // to the board's actual value (or re-sends the SET). 5 KB stack covers
    // esp_http_client + cJSON_Parse on the ~80-byte response body.
    xTaskCreate(poll_watchdog_task, "ms_ws_poll",  5120, NULL, 4, NULL);
    return true;
}

// Re-subscribe every fader (input + master) under the new format. Per
// repro_ms_subscribe_format.py, MS treats subscriptions as path-keyed:
// re-subscribing the same path with a different format REPLACES the
// previous one. So we just sub the new format and the old one drops
// implicitly. MS also pushes the current value immediately on subscribe,
// so the slider/readout refresh without waiting for a move.
//
// Pending-set values are cleared because they were stamped against the
// old format's units (norm vs dB); a stale resend after a toggle would
// send the wrong wire format.
static void ws_set_level_format(app_level_format_t f)
{
    if (s_level_format == f) return;
    s_level_format = f;

    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        // Format will be picked up by the next on_connected_subscribe_all.
        return;
    }

    const char *level_fmt = (f == APP_LEVEL_FORMAT_DB) ? "val" : "norm";

    for (size_t i = 0; i < app_state_count(); ++i) {
        int ch_id = app_state_id_for_idx(i);
        if (ch_id < 0) continue;
        char dotted[48];
        snprintf(dotted, sizeof(dotted),
                 "ch.%d.levelData.%d.lvl", ch_id, s_mix_bus_idx);
        subscribe_path(dotted, level_fmt);
        if (i < APP_CONFIG_MAX_CHANNELS) {
            s_pending_set_at_us[i] = 0;
            s_pending_resends [i] = 0;
        }
    }

    int master_id = master_channel_id();
    if (master_id >= 0) {
        char dotted[32];
        snprintf(dotted, sizeof(dotted), "ch.%d.mix.lvl", master_id);
        subscribe_path(dotted, level_fmt);
    }
}

// Caller passes the slider position 0..1. The wire format depends on the
// current level-format pref: NORM sends norm directly via /lvl/norm; DB
// converts to dB via the linear-in-dB mapping and sends via /lvl/val.
static void ws_set_level(int ms_channel_id, float position)
{
    if (position < 0.0f) position = 0.0f;
    if (position > 1.0f) position = 1.0f;

    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "set_level dropped: ws not connected");
        return;
    }

    char path[80];
    char body[48];
    if (s_level_format == APP_LEVEL_FORMAT_DB) {
        float db = app_position_to_db(position);
        snprintf(path, sizeof(path),
                 "/console/data/set/ch.%d.levelData.%d.lvl/val",
                 ms_channel_id, s_mix_bus_idx);
        snprintf(body, sizeof(body), "{\"value\":%.4f}", (double)db);
    } else {
        snprintf(path, sizeof(path),
                 "/console/data/set/ch.%d.levelData.%d.lvl/norm",
                 ms_channel_id, s_mix_bus_idx);
        snprintf(body, sizeof(body), "{\"value\":%.6f}", (double)position);
    }
    send_envelope("POST", path, body);

    // Arm the outstanding-SET tracker. Cleared in handle_broadcast on the
    // matching `lvl` echo, escalated by the poll watchdog otherwise. Stamp
    // by slot index, not channel id, so reordered slots stay aligned with
    // the broadcast handler's lookup. Reset retry budget on every fresh
    // user-driven SET so a re-send earlier in the session doesn't poison
    // a later legit drop. We stash the slider position (0..1) so the
    // resync re-send maps back through the current format.
    int idx = app_state_idx_for_id(ms_channel_id);
    if (idx >= 0 && idx < APP_CONFIG_MAX_CHANNELS) {
        s_pending_set_at_us[idx] = esp_timer_get_time();
        s_pending_set_value[idx] = position;
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

// Caller passes slider position 0..1. Same format-aware conversion as
// ws_set_level -- the master rides `mix.lvl` which accepts both norm and
// val format selectors.
static void ws_set_master_level(float position)
{
    if (position < 0.0f) position = 0.0f;
    if (position > 1.0f) position = 1.0f;
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    int id = master_channel_id();
    if (id < 0) return;

    char path[64];
    char body[48];
    if (s_level_format == APP_LEVEL_FORMAT_DB) {
        float db = app_position_to_db(position);
        snprintf(path, sizeof(path),
                 "/console/data/set/ch.%d.mix.lvl/val", id);
        snprintf(body, sizeof(body), "{\"value\":%.4f}", (double)db);
    } else {
        snprintf(path, sizeof(path),
                 "/console/data/set/ch.%d.mix.lvl/norm", id);
        snprintf(body, sizeof(body), "{\"value\":%.6f}", (double)position);
    }
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


// #30: send metering2 subscribe/unsubscribe. Body is bigger than the
// stack-allocated frame in send_envelope (256 B) for >8 channels, so
// we build it via cJSON and send_text directly. params order matches
// app_state slot order so decode can index by position.
static void meter_send_subscribe(void)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;

    s_meter_param_count = 0;
    cJSON *root   = cJSON_CreateObject();
    cJSON *body   = cJSON_CreateObject();
    cJSON *params = cJSON_CreateArray();
    if (!root || !body || !params) {
        if (root)   cJSON_Delete(root);
        if (body)   cJSON_Delete(body);
        if (params) cJSON_Delete(params);
        return;
    }
    for (size_t i = 0; i < app_state_count() && i < APP_CONFIG_MAX_CHANNELS; ++i) {
        int ch_id = app_state_id_for_idx(i);
        if (ch_id < 0) continue;
        s_meter_param_ids[s_meter_param_count++] = ch_id;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "index", ch_id);
        cJSON_AddNumberToObject(p, "type",  0);  // mono peak; verified
        cJSON_AddItemToArray(params, p);
    }
    cJSON_AddBoolToObject  (body, "binary",   true);
    cJSON_AddNumberToObject(body, "interval", METERING_INTERVAL_MS);
    cJSON_AddNumberToObject(body, "id",       METERING_SUB_ID);
    cJSON_AddItemToObject  (body, "params",   params);  // takes ownership

    cJSON_AddStringToObject(root, "method", "POST");
    cJSON_AddStringToObject(root, "path",   "/console/metering2/subscribe");
    cJSON_AddItemToObject  (root, "body",   body);      // takes ownership

    char *frame = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!frame) return;
    APP_LOGD_I("ms_ws", "meter subscribe: %d channels @ %d ms",
               s_meter_param_count, METERING_INTERVAL_MS);
    esp_websocket_client_send_text(s_ws, frame, strlen(frame), portMAX_DELAY);
    free(frame);
}

static void meter_send_unsubscribe(void)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    char body[24];
    snprintf(body, sizeof(body), "{\"id\":%d}", METERING_SUB_ID);
    send_envelope("POST", "/console/metering/unsubscribe", body);
    s_meter_param_count = 0;
}

static void ws_set_meter_enabled(bool on)
{
    bool was = s_meter_subscribed;
    s_meter_subscribed = on;
    if (!on && was) {
        meter_send_unsubscribe();
    }
    if (on) {
        meter_send_subscribe();
    }
    // Always reset cached meter values to the "no sample" sentinel on a
    // mode flip — clears stale fill that would briefly flash on the bar
    // before the first new broadcast arrives, both directions.
    if (on != was) {
        for (size_t i = 0; i < app_state_count(); ++i) {
            app_state_set_meter_db(i, -200.0f, true);
        }
    }
}

// Pads a non-padded base64 string in place so mbedtls accepts it. MS
// emits the trailing '=' padding stripped (RFC 4648 5.) — we re-add up
// to 2 '='s. Output buffer must have room for in_len + 3.
static size_t pad_b64(char *buf, size_t in_len, size_t buf_cap)
{
    size_t pad = (4 - (in_len % 4)) % 4;
    if (in_len + pad + 1 > buf_cap) return 0;
    for (size_t i = 0; i < pad; ++i) buf[in_len + i] = '=';
    buf[in_len + pad] = '\0';
    return in_len + pad;
}

// Decode one /console/metering2/<id> broadcast body. Filters by id so a
// stale subscription (after a mix-change resub or test scaffolding) is
// ignored. Maps payload position -> app_state slot via s_meter_param_ids.
static void handle_metering(int sub_id, cJSON *jbody)
{
    if (sub_id != METERING_SUB_ID) return;
    if (s_meter_param_count <= 0) return;

    cJSON *jb = cJSON_GetObjectItem(jbody, "b");
    if (!cJSON_IsString(jb) || !jb->valuestring) return;
    const char *b64 = jb->valuestring;
    size_t in_len = strlen(b64);
    if (in_len == 0) return;

    // Pad in a stack copy. Bound max size: APP_CONFIG_MAX_CHANNELS *
    // 2 bytes (mono peak per channel) -> 48B raw -> ~64B base64.
    char padded[96];
    if (in_len + 4 > sizeof(padded)) return;
    memcpy(padded, b64, in_len);
    size_t padded_len = pad_b64(padded, in_len, sizeof(padded));
    if (padded_len == 0) return;

    unsigned char raw[APP_CONFIG_MAX_CHANNELS * 2 + 4];
    size_t        raw_len = 0;
    int rc = mbedtls_base64_decode(raw, sizeof(raw), &raw_len,
                                   (const unsigned char *) padded, padded_len);
    if (rc != 0) {
        APP_LOGD_W("ms_ws", "meter b64 decode rc=%d", rc);
        return;
    }
    int values = (int) (raw_len / 2);
    if (values > s_meter_param_count) values = s_meter_param_count;

    for (int i = 0; i < values; ++i) {
        int ch_id = s_meter_param_ids[i];
        int idx   = app_state_idx_for_id(ch_id);
        if (idx < 0) continue;
        // big-endian int16, scale=100. -90.0 = silence floor on Si.
        int16_t v = (int16_t) ((raw[2 * i] << 8) | raw[2 * i + 1]);
        float db  = (float) v / 100.0f;
        app_state_set_meter_db((size_t) idx, db, true);
    }
}

static void on_connected_subscribe_all(void)
{
    // For each tracked channel, subscribe to its fader, scribble-strip
    // name, and mute state. The fader is subscribed in EITHER norm or val
    // format -- never both. NORM mode gets the audio-tapered MS norm
    // (slider position == norm); DB mode gets dB and the slider position
    // is derived linearly across APP_DB_MIN..APP_DB_MAX. Toggling the
    // level-format pref calls ws_set_level_format which re-subscribes
    // every channel under the new format.
    const char *level_fmt = (s_level_format == APP_LEVEL_FORMAT_DB) ? "val" : "norm";
    for (size_t i = 0; i < app_state_count(); ++i) {
        int ch_id = app_state_id_for_idx(i);
        if (ch_id < 0) continue;

        char dotted[48];
        snprintf(dotted, sizeof(dotted),
                 "ch.%d.levelData.%d.lvl", ch_id, s_mix_bus_idx);
        subscribe_path(dotted, level_fmt);

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
    // input strips: `ch.<N>.mix.lvl` (no `level` alias -- see
    // repro_ms_master_fader.py) and `ch.<N>.mix.on`. Same format-aware
    // single-sub pattern as inputs: NORM gets norm, DB gets val. Master
    // `cfg.name` is already subscribed by the mix-name loop above.
    // Re-aimed at the new id on every set_mix via ws_resubscribe → here.
    int master_id = master_channel_id();
    if (master_id >= 0) {
        char dotted[32];
        snprintf(dotted, sizeof(dotted), "ch.%d.mix.lvl", master_id);
        subscribe_path(dotted, level_fmt);
        snprintf(dotted, sizeof(dotted), "ch.%d.mix.on", master_id);
        subscribe_path(dotted, "val");
        // Hand the id to app_state so the master-state's mute/level
        // notifications carry the right channel context. Clears stale
        // values on a mix change.
        app_state_master_set_id(master_id);
    }

    ESP_LOGI(TAG, "subscribed %d channels + %d mix names + master(ch.%d)",
             (int) app_state_count(), s_mix_count, master_id);

    // #30: re-arm metering subscription if the user had it on across the
    // reconnect (or set_mix re-subscribes the channel set under a new
    // mix). Always rebuild rather than try to dedupe — MS dedupes by id
    // and accepts a fresh /console/metering2/subscribe as an in-place
    // replacement of the params list.
    if (s_meter_subscribed) {
        meter_send_subscribe();
    }
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

    const char *p = jpath->valuestring;

    // #30: metering2 broadcasts arrive on /console/metering2/<id>, NOT
    // /console/data/get/, so route them off here before the data-prefix
    // check below.
    {
        int sub_id;
        if (sscanf(p, "/console/metering2/%d", &sub_id) == 1) {
            handle_metering(sub_id, jbody);
            cJSON_Delete(root);
            return;
        }
    }

    // Strip the "/console/data/get/" prefix. Anything that doesn't carry it
    // isn't a value broadcast we care about.
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
                // Format-aware: in DB mode the broadcast carries dB; in
                // NORM mode it carries norm. Update the matching state
                // field; the UI's apply_pending picks the right one based
                // on the current format pref.
                if (s_level_format == APP_LEVEL_FORMAT_DB) {
                    app_state_set_level_db((size_t)idx, (float)jvalue->valuedouble, true);
                } else {
                    app_state_set_level((size_t)idx, (float)jvalue->valuedouble, true);
                }
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
        // P3: always update the all-strip cache so the channel picker
        // overlay reflects the latest names regardless of whether this
        // channel happens to be tracked in app_state.
        if (ch >= 0 && ch < MS_MAX_STRIP_NAMES) {
            strncpy(s_all_strip_names[ch], jvalue->valuestring,
                    sizeof(s_all_strip_names[ch]) - 1);
            s_all_strip_names[ch][sizeof(s_all_strip_names[ch]) - 1] = '\0';
        }
        // Mix scribble-strip names use the same path; if this ch falls in
        // the mix range, cache the name for the selector popup.
        if (s_mix_count > 0 && ch >= s_mix_offset &&
            ch < s_mix_offset + s_mix_count) {
            int mix_idx = ch - s_mix_offset;
            strncpy(s_mix_names[mix_idx], jvalue->valuestring,
                    sizeof(s_mix_names[mix_idx]) - 1);
            s_mix_names[mix_idx][sizeof(s_mix_names[mix_idx]) - 1] = '\0';
        }
        // Notify on every name broadcast -- the chpick overlay refresh
        // hangs off this notification too. mix_picker_refresh_labels and
        // chpick_refresh_labels are both gated on visibility.
        notify_subscribers();
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
    // ws_set_mix). Format-aware like input strips: DB mode subs `mix.lvl`
    // as val (dB), NORM mode as norm.
    if (sscanf(dotted, "ch.%d.mix.%15s", &ch, suffix) == 2 &&
        ch == master_channel_id()) {
        if (strcmp(suffix, "lvl") == 0 && cJSON_IsNumber(jvalue)) {
            if (s_level_format == APP_LEVEL_FORMAT_DB) {
                app_state_master_set_level_db((float)jvalue->valuedouble, true);
            } else {
                app_state_master_set_level((float)jvalue->valuedouble, true);
            }
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
        // P5: if the boot path skipped /console/information (MS unreachable
        // at app_main time), the mix layout is empty and the indicator
        // would stay hidden forever. Re-fetch here so the FIRST WS connect
        // — including the one after a config-change reboot when the host
        // came up just slightly later than the device — populates the
        // layout before we touch the visibility gate. Cheap (~50 ms) and
        // idempotent on subsequent reconnects.
        if (s_mix_count == 0) {
            app_ms_info_t info;
            if (app_ms_info_fetch(app_config_ms_host(),
                                  app_config_ms_port(), &info) &&
                info.mix_count > 0) {
                ws_set_mix_layout(info.mix_offset, info.mix_count);
                ESP_LOGI(TAG, "post-connect refetch: mix_count=%d", info.mix_count);
            }
        }
        // Layout-known case: flip the received flag now (set_state already
        // notified for the connect transition; this fires the second
        // notify so the UI sees BOTH bits set in one apply sweep).
        if (s_mix_count > 0 && !s_mix_list_received) {
            s_mix_list_received = true;
        }
        set_state(APP_MS_STATE_CONNECTED);
        on_connected_subscribe_all();
        notify_subscribers();
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
        // P5: clear the mix-list-received flag so the UI's apply-sweep
        // hides the indicator until the next CONNECTED event re-establishes
        // it. Without this, a wedged-then-recreated WS could leave the
        // button visible while the underlying subscriptions were torn down.
        s_mix_list_received = false;
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
