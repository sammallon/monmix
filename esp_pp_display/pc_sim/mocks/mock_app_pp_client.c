// Mock app_pp_client. Lets pc_main inject fake "connected" state, drive
// state transitions for the icon, and pretend that stage_message_put /
// trigger_* succeed (we just echo the result back into app_pp_state).
#include "app_pp_client.h"
#include "app_pp_state.h"

#include <stdio.h>
#include <string.h>

#define MAX_OBS 4

typedef struct { app_pp_on_change_t cb; void *ctx; } obs_t;
static obs_t s_obs[MAX_OBS];
static size_t s_obs_count;
static app_pp_conn_state_t s_state = APP_PP_CONN_CONNECTED;

static void mock_start(void) {}

static app_pp_conn_state_t mock_get_state(void) { return s_state; }

static void mock_register_on_change(app_pp_on_change_t cb, void *ctx)
{
    if (!cb) return;
    if (s_obs_count < MAX_OBS) {
        s_obs[s_obs_count].cb  = cb;
        s_obs[s_obs_count].ctx = ctx;
        s_obs_count++;
    }
}

void mock_pp_set_conn_state(app_pp_conn_state_t s)
{
    if (s == s_state) return;
    s_state = s;
    for (size_t i = 0; i < s_obs_count; ++i) s_obs[i].cb(s_obs[i].ctx);
}

static bool mock_stage_message_put(const char *msg)
{
    app_pp_state_set_stage_message(msg ? msg : "");
    return true;
}

static bool mock_stage_message_clear(void)
{
    app_pp_state_set_stage_message("");
    return true;
}

static bool mock_trigger_next(void)     { return true; }
static bool mock_trigger_previous(void) { return true; }
static bool mock_resubscribe(void)      { return true; }

static const app_pp_client_iface_t IFACE = {
    .start               = mock_start,
    .get_state           = mock_get_state,
    .register_on_change  = mock_register_on_change,
    .stage_message_put   = mock_stage_message_put,
    .stage_message_clear = mock_stage_message_clear,
    .trigger_next        = mock_trigger_next,
    .trigger_previous    = mock_trigger_previous,
    .resubscribe         = mock_resubscribe,
};

const app_pp_client_iface_t *app_pp_client_tcp(void) { return &IFACE; }
