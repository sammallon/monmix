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

    // Re-subscribe every tracked channel under the current mix bus. Used
    // by the discovery flow after reseeding app_state. No-op when the WS
    // isn't connected.
    void (*resubscribe)(void);
} ms_client_iface_t;

const ms_client_iface_t *app_ms_client_ws(void);
