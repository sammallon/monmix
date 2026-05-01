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
    void (*stop)(void);

    app_ms_state_t (*get_state)(void);
    const char    *(*get_host)(void);
    int            (*get_port)(void);
    void           (*register_on_change)(app_ms_on_change_t cb, void *ctx);
} ms_client_iface_t;

const ms_client_iface_t *app_ms_client_ws(void);
