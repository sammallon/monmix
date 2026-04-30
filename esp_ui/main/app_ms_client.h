#pragma once

#include <stdbool.h>

// Backend-agnostic Mixing Station client interface.
//   M1: app_ms_client_ws.c   (REST + WebSocket)
//   M5: app_ms_client_osc.c  (OSC over UDP, configurable alternative)
typedef struct {
    bool (*start)(void);
    void (*set_level)(int ms_channel_id, float level);
    void (*stop)(void);
} ms_client_iface_t;

const ms_client_iface_t *app_ms_client_ws(void);
