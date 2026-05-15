// ProPresenter client iface — backend-agnostic, mirroring the
// app_ms_client_iface_t pattern in esp_ui/main/app_ms_client.h.
//
// Skeleton-round surface only. The chunked /v1/status/updates subscriber
// and the trigger-next / trigger-previous REST calls land in Round 2
// (real backend, talks to a live ProPresenter). For now there's only the
// mock backend in pc_sim/mocks/mock_app_pp_client.c, which pumps canned
// slide payloads on a timer so the UI animates.

#ifndef APP_PP_CLIENT_H
#define APP_PP_CLIENT_H

#include <stdbool.h>

typedef enum {
    APP_PP_CONN_BOOT = 0,
    APP_PP_CONN_CONNECTING,
    APP_PP_CONN_CONNECTED,
    APP_PP_CONN_DISCONNECTED,
    APP_PP_CONN_ERROR,
} app_pp_conn_state_t;

typedef void (*app_pp_on_change_t)(void *ctx);

typedef struct {
    bool (*start)(void);
    void (*stop)(void);

    // Slide advance. Wired to the on-screen Prev/Next buttons AND to the
    // tap-on-next-slide preview. In the real backend these will POST to
    // /v1/trigger/previous and /v1/trigger/next.
    void (*trigger_next)(void);
    void (*trigger_previous)(void);

    // Drop + recreate. Called from the PP-config panel after the user
    // changes host/port and applies. Mock impl just resets its sim state
    // and notifies; real backend tears the chunked stream down and
    // reconnects.
    void (*reconnect)(void);

    app_pp_conn_state_t (*get_state)(void);
    const char         *(*get_host)(void);
    int                 (*get_port)(void);

    void (*register_on_change)(app_pp_on_change_t cb, void *ctx);
} pp_client_iface_t;

// Currently always returns the mock backend. The real backend lands in
// Round 2 and selects between this and a real_client based on a runtime
// flag (analogous to the --ms-host gate in esp_ui's pc_main).
const pp_client_iface_t *app_pp_client(void);

#endif // APP_PP_CLIENT_H
