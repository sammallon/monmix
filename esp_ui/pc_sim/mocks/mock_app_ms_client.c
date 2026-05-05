// Mock MS client. Sits in APP_MS_STATE_CONNECTED from the moment start()
// is called and emits a single on-change notification so the UI updates
// its status badge. Setters log; getters return placeholder values.
//
// This is exactly the surface needed to repro the mute-button panic:
// the click handler's gate 1 (ms_ok) needs get_state() == CONNECTED, and
// then gate 2 (s_mute_enabled=false) routes to toast_show() — which is
// where the bug lives.
#include "app_ms_client.h"

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "app_state.h"

static app_ms_state_t s_state = APP_MS_STATE_BOOT;
static app_ms_on_change_t s_cb;
static void *s_cb_ctx;
static int s_mix_idx     = 0;
static int s_mix_offset  = 60;     // matches Si Expression 2 layout
static int s_mix_count   = 14;
static char s_strip_name_buf[64];

static void fire_change(void) { if (s_cb) s_cb(s_cb_ctx); }

static bool m_start(void) {
    s_state = APP_MS_STATE_CONNECTED;
    fprintf(stdout, "[mock_ms] start() -> CONNECTED\n");
    fire_change();
    return true;
}
static void m_stop(void) {
    s_state = APP_MS_STATE_DISCONNECTED;
    fire_change();
}
static void m_set_level(int id, float level) {
    fprintf(stdout, "[mock_ms] set_level ch=%d level=%.3f\n", id, level);
}
static void m_set_mute(int id, bool mute) {
    fprintf(stdout, "[mock_ms] set_mute ch=%d mute=%d\n", id, (int)mute);
}
static void m_set_name(int id, const char *name) {
    fprintf(stdout, "[mock_ms] set_name ch=%d name=%s\n", id, name ? name : "(null)");
}

static app_ms_state_t m_get_state(void)         { return s_state; }
static const char    *m_get_host(void)          { return "127.0.0.1"; }
static int            m_get_port(void)          { return 9000; }
static void           m_register(app_ms_on_change_t cb, void *ctx) { s_cb = cb; s_cb_ctx = ctx; }

static int            m_get_mix(void)           { return s_mix_idx; }
static void           m_set_mix(int idx)        { s_mix_idx = idx; fire_change(); }
static void           m_set_mix_layout(int off, int count) { s_mix_offset = off; s_mix_count = count; }

static const char    *m_get_mix_name(int idx) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "Mix %d", idx + 1);
    return buf;
}

static bool m_is_mix_routed(int idx)        { (void)idx; return true; }
static void m_fetch_mix_routing(void)       {}
static bool m_is_mix_list_ready(void)       { return s_mix_count > 0; }
static void m_resubscribe(void)             {}
static void m_reconnect(void)               { m_stop(); m_start(); }

static void m_set_master_level(float l)     { fprintf(stdout, "[mock_ms] master level=%.3f\n", l); }
static void m_set_master_mute (bool mute)   { fprintf(stdout, "[mock_ms] master mute=%d\n", (int)mute); }

static void m_fetch_all_strip_names(int total) { (void)total; }
static const char *m_get_strip_name(int id) {
    snprintf(s_strip_name_buf, sizeof(s_strip_name_buf), "CH %02d", id + 1);
    return s_strip_name_buf;
}
static void m_fetch_channel_routability(int total) { (void)total; }
static bool m_is_channel_routable(int id)          { (void)id; return true; }
static void m_set_meter_enabled(bool on)           { (void)on; }
static void m_set_level_format(app_level_format_t f) { (void)f; }

static const ms_client_iface_t s_iface = {
    .start                       = m_start,
    .set_level                   = m_set_level,
    .set_mute                    = m_set_mute,
    .set_name                    = m_set_name,
    .stop                        = m_stop,
    .get_state                   = m_get_state,
    .get_host                    = m_get_host,
    .get_port                    = m_get_port,
    .register_on_change          = m_register,
    .get_mix                     = m_get_mix,
    .set_mix                     = m_set_mix,
    .set_mix_layout              = m_set_mix_layout,
    .get_mix_name                = m_get_mix_name,
    .is_mix_routed               = m_is_mix_routed,
    .fetch_mix_routing           = m_fetch_mix_routing,
    .is_mix_list_ready           = m_is_mix_list_ready,
    .resubscribe                 = m_resubscribe,
    .reconnect                   = m_reconnect,
    .set_master_level            = m_set_master_level,
    .set_master_mute             = m_set_master_mute,
    .fetch_all_strip_names       = m_fetch_all_strip_names,
    .get_strip_name              = m_get_strip_name,
    .fetch_channel_routability   = m_fetch_channel_routability,
    .is_channel_routable         = m_is_channel_routable,
    .set_meter_enabled           = m_set_meter_enabled,
    .set_level_format            = m_set_level_format,
};

const ms_client_iface_t *app_ms_client_ws(void) { return &s_iface; }
