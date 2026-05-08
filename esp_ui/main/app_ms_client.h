#pragma once

#include <math.h>
#include <stdbool.h>

#include "app_prefs.h"

// MS lvl/val range on the Si Expression 2 (verified by repro_ms_master_fader.py).
#define APP_DB_MIN   (-138.0f)
#define APP_DB_MAX   (10.0f)
#define APP_DB_RANGE (APP_DB_MAX - APP_DB_MIN)

// DB-mode slider taper: 4th-root curve so unity gain (0 dB) lands at
// ~75% slider position and useful headroom (-15 dB..+10 dB) gets the
// top quarter of travel. Linear-in-dB compressed everything useful
// into the top 11% (since 0 dB sits at (138/148)=93% on a linear ramp);
// pow 0.25 expands that out to roughly an audio-taper feel.
//
//   pos -> db: db = APP_DB_MIN + APP_DB_RANGE * pos^0.25
//   db -> pos: pos = ((db - APP_DB_MIN) / APP_DB_RANGE)^4
//
// Sample points (k=0.25):
//   pos 0.10 -> -54 dB        pos 0.50 -> -13 dB
//   pos 0.25 -> -33 dB        pos 0.75 ->   0 dB
//   pos 0.90 ->  +6 dB        pos 1.00 -> +10 dB
//
// Implemented with sqrtf (forward = 4th root via two sqrts) and a
// 4-mul (inverse) -- avoids powf and stays out of libm hot paths.
static inline float app_db_to_position(float db)
{
    if (db <= APP_DB_MIN) return 0.0f;
    if (db >= APP_DB_MAX) return 1.0f;
    float linear = (db - APP_DB_MIN) / APP_DB_RANGE;
    return linear * linear * linear * linear;
}

static inline float app_position_to_db(float pos)
{
    if (pos <= 0.0f) return APP_DB_MIN;
    if (pos >= 1.0f) return APP_DB_MAX;
    return APP_DB_MIN + APP_DB_RANGE * sqrtf(sqrtf(pos));
}

typedef enum {
    APP_MS_STATE_BOOT = 0,    // before start()
    APP_MS_STATE_CONNECTING,  // start() returned, no connect event yet
    APP_MS_STATE_CONNECTED,
    APP_MS_STATE_DISCONNECTED,
    APP_MS_STATE_ERROR,
} app_ms_state_t;

typedef void (*app_ms_on_change_t)(void *ctx);

// Backend-agnostic Mixing Station client interface.
//   M1: app_ms_client_ws.c   (REST + WebSocket)
//   M5: app_ms_client_osc.c  (OSC over UDP, configurable alternative)
typedef struct {
    bool (*start)(void);
    void (*set_level)(int ms_channel_id, float level);
    void (*set_mute) (int ms_channel_id, bool mute);
    void (*set_name) (int ms_channel_id, const char *name);
    void (*stop)(void);

    app_ms_state_t (*get_state)(void);
    const char    *(*get_host)(void);
    int            (*get_port)(void);
    void           (*register_on_change)(app_ms_on_change_t cb, void *ctx);

    // Mix bus selection. The fader strips control sends to this mix; on
    // set_mix the client re-subscribes every tracked channel against the
    // new mix index. Si Expression 2: 0..13 = Mix 1..14.
    int  (*get_mix)(void);
    void (*set_mix)(int mix_idx);

    // Set the mix-bus layout from /console/information's Mix channelType.
    // Tells the client where in the ch.<n>.* namespace the mix strips live
    // (offset = first mix's channel id, count = how many) so it can
    // subscribe to their cfg.name scribble strips.
    void (*set_mix_layout)(int offset, int count);

    // Mix scribble-strip name. Returns NULL if the layout hasn't been set
    // or the name hasn't broadcast yet. Caller should fall back to
    // "Mix <idx+1>" when NULL.
    const char *(*get_mix_name)(int mix_idx);

    // P11: routed-mix filter. MS exposes ch.<mix_offset+i>.info.isActive
    // (bool) per mix bus; only the routed ones should appear in the
    // selector. Returns true if the layout hasn't been populated yet so
    // callers don't accidentally hide everything before discovery
    // completes -- esp_ui_main forces a synchronous REST fetch before
    // boot-time validation runs.
    bool (*is_mix_routed)(int mix_idx);

    // Synchronously REST-fetch the routed (info.isActive) mask for every
    // mix bus in the current layout. Called once from app_main between
    // set_mix_layout and the saved-index validation so the boot path
    // doesn't see an empty mask. The WS subscribe path keeps it live
    // afterwards.
    void (*fetch_mix_routing)(void);

    // P5: true once the mix-bus layout is known (count > 0) AND we have an
    // active WS connection. Cleared on WEBSOCKET_EVENT_DISCONNECTED so the
    // UI can fall back to "loading" until the next connect re-establishes
    // the list. The mix-indicator visibility uses this as one half of its
    // gate (the other being get_state() == CONNECTED).
    bool (*is_mix_list_ready)(void);

    // True once MS has reported `state == "connected"` on /app/state. This
    // is *separate* from get_state() — the WS connects to MS itself, but
    // MS may still be trying to attach to its physical console (common
    // scenario: console gets powered off after service and back on the
    // following week). While the WS is up but the console isn't, queries
    // like /console/information return default-shaped data and broadcasts
    // never fire, so the boot-time setup must wait on this flag.
    // Polled via /app/state heartbeat at HEARTBEAT_INTERVAL_MS; flips
    // notify_subscribers so the UI's MS icon can show a distinct
    // "MS up, console offline" state. Defaults to false until the first
    // heartbeat lands.
    bool (*is_console_attached)(void);

    // Re-subscribe every tracked channel under the current mix bus. Used
    // by the discovery flow after reseeding app_state. No-op when the WS
    // isn't connected.
    void (*resubscribe)(void);

    // Live-apply a host/port change: stop + destroy the current client,
    // start a fresh one against whatever app_config_ms_host/port currently
    // returns. Called from the MS-settings UI after the user saves new
    // values. Does NOT touch wifi -- the watchdog's wifi-reassoc path is
    // for stuck connections, not for normal config changes.
    void (*reconnect)(void);

    // Master fader (mix-bus output) SET path. Master uses `ch.<N>.mix.lvl`
    // and `ch.<N>.mix.on`, NOT the per-channel `levelData.<m>.*` paths.
    // Channel id derives from current mix bus + mix offset; client owns
    // the mapping so callers don't have to track the layout.
    void (*set_master_level)(float level);
    void (*set_master_mute) (bool mute);

    // P3: prime the all-strip-name cache by REST-sweeping ch.<n>.cfg.name
    // for every id in 0..total-1. The picker overlay wants names for ALL
    // strips on the connected console, not just those tracked in
    // app_state. Blocking; ~30 ms per GET on stage WiFi -> ~2.4 s for an
    // 80-channel Si Expression. Called once after set_mix_layout from
    // app_main; the WS broadcast handler keeps the cache live thereafter.
    void (*fetch_all_strip_names)(int total);

    // P3: read a cached scribble-strip name by raw MS channel id (NOT
    // the app_state slot index). Returns NULL when MS hasn't broadcast
    // a name for this id yet -- caller should fall back to "CH NN".
    const char *(*get_strip_name)(int ms_channel_id);

    // W6.1: REST-probe ch.<n>.levelData.0.lvl for every channel id in
    // 0..total-1; 200 -> routable to a mix, 404 -> not (mix/matrix/main
    // self-routes). Same scale + cadence as fetch_all_strip_names; called
    // once after set_mix_layout, before ws_start. Cache stays valid for
    // the session (channel TYPE doesn't change without an MS profile
    // swap, which restarts the WS anyway).
    void (*fetch_channel_routability)(int total);

    // W6.1: getter for the cached routability bit. Defaults to true so a
    // never-fetched state never accidentally hides everything.
    bool (*is_channel_routable)(int ms_channel_id);

    // #30: real metering subscription. on=true subscribes the current
    // tracked channel set to /console/metering2 at ~10 Hz; on=false
    // unsubscribes. Idempotent. Called by the UI when the user changes
    // signal_indicator pref to/from APP_SIGNAL_INDICATOR_METER, and on
    // every reconnect/resubscribe so the subscription survives WS
    // bounce. Routes incoming dB values through app_state_set_meter_db.
    void (*set_meter_enabled)(bool on);

    // Tell the client which format the UI is currently displaying. Used
    // to gate the dB-only subscriptions: in NORM mode only `lvl/norm` is
    // subscribed (input strips and master); in DB mode the input strips
    // also subscribe `level/val` and the master arms a debounced REST
    // fetch for `mix.lvl/val` (master has no `level` alias to subscribe
    // alongside `lvl`, see repro_ms_subscribe_format.py). Idempotent;
    // called on every level-format pref change.
    void (*set_level_format)(app_level_format_t f);

    // Send /console/data/unsubscribe for every active subscription, then
    // close the WS with a proper close frame (1000 NORMAL). Blocks up to
    // ~500 ms for the close ACK. Safe to call when not connected (no-op).
    // Distinct from stop(): stop() tears the worker down without server
    // notice, shutdown_graceful() informs MS first so it doesn't leak the
    // subscription state and an abruptly-closed connection. Called from
    // the reboot path (so MS doesn't have to clean up after every flash
    // cycle) and from pc_sim's exit handlers.
    void (*shutdown_graceful)(void);
} ms_client_iface_t;

// Helper: route an esp_restart() through the active client's
// shutdown_graceful first, with a short delay for log flush. Defined in
// app_console.c (the only TU that already pulls in esp_system + the
// active-client accessor). Use this instead of calling esp_restart()
// directly so MS isn't left holding a stale WS connection.
void app_reboot_graceful(void);

const ms_client_iface_t *app_ms_client_ws(void);
const ms_client_iface_t *app_ms_client_osc(void);
