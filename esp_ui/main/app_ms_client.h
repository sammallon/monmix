#pragma once

#include <stdbool.h>

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
} ms_client_iface_t;

const ms_client_iface_t *app_ms_client_ws(void);
