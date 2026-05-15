#include "app_pp_state.h"

#include <string.h>

// Bounded subscriber list — keeps app_pp_ui as the obvious primary
// consumer + leaves a slot for a future test observer. Match esp_ui's
// MAX_OBS=4 so the shape is identical when we port modules.
#define APP_PP_STATE_MAX_SUBS 4

static struct {
    app_pp_state_on_change_t cb;
    void                    *ctx;
} s_subs[APP_PP_STATE_MAX_SUBS];
static size_t s_subs_n;

static app_pp_slide_t              s_current;
static app_pp_slide_t              s_next;
static app_pp_presentation_info_t  s_pres;
static app_pp_stage_message_t      s_msg;
static app_pp_timing_t             s_timing;

static void notify(void) {
    for (size_t i = 0; i < s_subs_n; ++i) {
        if (s_subs[i].cb) s_subs[i].cb(s_subs[i].ctx);
    }
}

void app_pp_state_init(void) {
    memset(&s_current, 0, sizeof(s_current));
    memset(&s_next,    0, sizeof(s_next));
    memset(&s_pres,    0, sizeof(s_pres));
    memset(&s_msg,     0, sizeof(s_msg));
    memset(&s_timing,  0, sizeof(s_timing));
    s_pres.current_index = -1;
    s_subs_n = 0;
}

void app_pp_state_subscribe(app_pp_state_on_change_t cb, void *ctx) {
    if (s_subs_n >= APP_PP_STATE_MAX_SUBS) return;
    s_subs[s_subs_n].cb  = cb;
    s_subs[s_subs_n].ctx = ctx;
    ++s_subs_n;
}

void app_pp_state_set_current_slide(const app_pp_slide_t *s) {
    if (!s) { memset(&s_current, 0, sizeof(s_current)); }
    else    { s_current = *s; }
    notify();
}

void app_pp_state_set_next_slide(const app_pp_slide_t *s) {
    if (!s) { memset(&s_next, 0, sizeof(s_next)); }
    else    { s_next = *s; }
    notify();
}

void app_pp_state_set_presentation(const app_pp_presentation_info_t *p) {
    if (!p) { memset(&s_pres, 0, sizeof(s_pres)); s_pres.current_index = -1; }
    else    { s_pres = *p; }
    notify();
}

void app_pp_state_set_stage_message(const app_pp_stage_message_t *m) {
    if (!m) { memset(&s_msg, 0, sizeof(s_msg)); }
    else    { s_msg = *m; }
    notify();
}

void app_pp_state_set_timing(const app_pp_timing_t *t) {
    if (!t) { memset(&s_timing, 0, sizeof(s_timing)); }
    else    { s_timing = *t; }
    notify();
}

const app_pp_slide_t             *app_pp_state_current_slide(void) { return &s_current; }
const app_pp_slide_t             *app_pp_state_next_slide   (void) { return &s_next;    }
const app_pp_presentation_info_t *app_pp_state_presentation (void) { return &s_pres;    }
const app_pp_stage_message_t     *app_pp_state_stage_message(void) { return &s_msg;     }
const app_pp_timing_t            *app_pp_state_timing       (void) { return &s_timing;  }
