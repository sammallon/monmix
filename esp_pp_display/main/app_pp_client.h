#pragma once

#include <stdbool.h>

typedef enum {
    APP_PP_CONN_DISCONNECTED = 0,
    APP_PP_CONN_CONNECTING,
    APP_PP_CONN_CONNECTED,
    APP_PP_CONN_RECONNECTING,
} app_pp_conn_state_t;

typedef void (*app_pp_on_change_t)(void *ctx);

// Function-pointer iface so future backends (HTTP REST shim, OSC if PP
// ever adds one) can slot in without changing callers. Today there's
// exactly one implementation (TCP socket on port 63306).
typedef struct {
    // Lifecycle. start() spawns the connection task and returns
    // immediately; reconnect / backoff is internal. stop() is a no-op
    // in the current impl -- the PP client runs from boot to reboot.
    void                  (*start)(void);
    app_pp_conn_state_t   (*get_state)(void);
    void                  (*register_on_change)(app_pp_on_change_t cb, void *ctx);

    // Outbound writes. All return true if the envelope was sent over a
    // live socket; false if disconnected or send failed. There's no
    // built-in retry -- callers that need durability should re-check
    // get_state() and call again later. The state subscriber callbacks
    // will fire when PP echoes the change back, which is the source of
    // truth for the UI.
    bool                  (*stage_message_put)(const char *msg);
    bool                  (*stage_message_clear)(void);
    bool                  (*trigger_next)(void);
    bool                  (*trigger_previous)(void);

    // Debug: tear down the socket and reconnect+resubscribe. Useful for
    // exercising the reconnect path from the REPL.
    bool                  (*resubscribe)(void);
} app_pp_client_iface_t;

// Singleton accessor. Pointer is stable across the process lifetime.
const app_pp_client_iface_t *app_pp_client_tcp(void);
