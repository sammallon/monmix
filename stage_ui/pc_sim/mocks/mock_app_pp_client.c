// Mock ProPresenter client. Pumps canned slide payloads on a periodic
// LVGL timer so the UI animates without a live PP instance. trigger_next
// and trigger_previous step through the fake sequence.
//
// Real client (chunked HTTP/1.1 subscriber to /v1/status/updates,
// REST POSTs for the triggers) is Round 2.

#include "app_pp_client.h"
#include "app_pp_state.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// ─── Canned content ────────────────────────────────────────────────────

typedef struct {
    const char *title;
    const char *text;
} canned_slide_t;

static const canned_slide_t s_deck[] = {
    { "Welcome",                 "Welcome\n\nGood morning" },
    { "Opening Song: Holy",      "Holy holy holy\nLord God Almighty\nEarly in the morning\nOur song shall rise to Thee" },
    { "Opening Song: Holy",      "Holy holy holy\nMerciful and mighty\nGod in three persons\nBlessed Trinity" },
    { "Scripture Reading",       "Psalm 23\n\nThe Lord is my shepherd\nI shall not want" },
    { "Sermon",                  "The Good Shepherd" },
    { "Closing Song: Doxology",  "Praise God\nfrom whom all blessings flow" },
    { "Closing Song: Doxology",  "Praise Him\nall creatures here below" },
    { "Announcements",           "See you next week\n\n10:30 AM" },
};
static const int s_deck_n = (int)(sizeof(s_deck) / sizeof(s_deck[0]));

// ─── State ─────────────────────────────────────────────────────────────

static app_pp_conn_state_t s_state = APP_PP_CONN_BOOT;
static app_pp_on_change_t  s_change_cb;
static void               *s_change_ctx;
static int                 s_idx     = 0;
static lv_timer_t         *s_advance_timer;
static lv_timer_t         *s_clock_timer;

static void fire_change(void) {
    if (s_change_cb) s_change_cb(s_change_ctx);
}

static void push_slide_pair(void) {
    app_pp_slide_t cur  = (app_pp_slide_t){ 0 };
    app_pp_slide_t next = (app_pp_slide_t){ 0 };

    if (s_idx >= 0 && s_idx < s_deck_n) {
        cur.present = true;
        snprintf(cur.uuid, sizeof(cur.uuid), "slide-%d", s_idx);
        snprintf(cur.text, sizeof(cur.text), "%s", s_deck[s_idx].text);
    }
    if (s_idx + 1 < s_deck_n) {
        next.present = true;
        snprintf(next.uuid, sizeof(next.uuid), "slide-%d", s_idx + 1);
        snprintf(next.text, sizeof(next.text), "%s", s_deck[s_idx + 1].text);
    }
    app_pp_state_set_current_slide(&cur);
    app_pp_state_set_next_slide(&next);

    app_pp_presentation_info_t pres = (app_pp_presentation_info_t){ 0 };
    pres.present       = true;
    pres.current_index = (s_idx >= 0 && s_idx < s_deck_n) ? s_idx : -1;
    pres.total_slides  = s_deck_n;
    if (s_idx >= 0 && s_idx < s_deck_n) {
        snprintf(pres.title, sizeof(pres.title), "%s", s_deck[s_idx].title);
    } else {
        snprintf(pres.title, sizeof(pres.title), "Service Plan");
    }
    app_pp_state_set_presentation(&pres);
}

static void clock_tick(lv_timer_t *t) {
    (void)t;
    time_t now = time(NULL);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    app_pp_timing_t timing = (app_pp_timing_t){ 0 };
    timing.present = true;
    strftime(timing.wall_clock, sizeof(timing.wall_clock), "%H:%M:%S", &lt);
    // Pseudo-countdown: minutes until top-of-hour. Just visible movement
    // until the real /v1/timers/current wiring lands.
    int rem_min = 59 - lt.tm_min;
    int rem_sec = 59 - lt.tm_sec;
    snprintf(timing.countdown, sizeof(timing.countdown), "T-%02d:%02d", rem_min, rem_sec);
    app_pp_state_set_timing(&timing);
}

static void advance_tick(lv_timer_t *t) {
    (void)t;
    s_idx = (s_idx + 1) % s_deck_n;
    push_slide_pair();
}

// ─── Iface ─────────────────────────────────────────────────────────────

static bool mock_start(void) {
    if (s_state == APP_PP_CONN_CONNECTED) return true;
    s_state = APP_PP_CONN_CONNECTED;
    s_idx   = 0;
    push_slide_pair();

    // Auto-advance every 4 s so the UI shows transitions without user
    // input. Easy to disable later via a sim flag if a soak run wants
    // a fixed slide.
    if (!s_advance_timer) {
        s_advance_timer = lv_timer_create(advance_tick, 4000, NULL);
    }
    if (!s_clock_timer) {
        s_clock_timer   = lv_timer_create(clock_tick, 250, NULL);
        clock_tick(NULL);
    }
    fprintf(stdout, "[mock_pp] start -> CONNECTED, %d slides\n", s_deck_n);
    fire_change();
    return true;
}

static void mock_stop(void) {
    s_state = APP_PP_CONN_DISCONNECTED;
    if (s_advance_timer) { lv_timer_delete(s_advance_timer); s_advance_timer = NULL; }
    if (s_clock_timer)   { lv_timer_delete(s_clock_timer);   s_clock_timer   = NULL; }
    fprintf(stdout, "[mock_pp] stop\n");
    fire_change();
}

static void mock_trigger_next(void) {
    fprintf(stdout, "[mock_pp] trigger_next  (slide %d -> %d)\n",
            s_idx, (s_idx + 1) % s_deck_n);
    s_idx = (s_idx + 1) % s_deck_n;
    push_slide_pair();
    // Reset auto-advance so a user tap doesn't get clipped by the next
    // auto-tick firing immediately.
    if (s_advance_timer) lv_timer_reset(s_advance_timer);
}

static void mock_trigger_previous(void) {
    int new_idx = (s_idx - 1 + s_deck_n) % s_deck_n;
    fprintf(stdout, "[mock_pp] trigger_prev  (slide %d -> %d)\n", s_idx, new_idx);
    s_idx = new_idx;
    push_slide_pair();
    if (s_advance_timer) lv_timer_reset(s_advance_timer);
}

static void mock_reconnect(void) {
    // Drop + restart sim state. Pumps a CONNECTING blip so the UI
    // status icon visibly transitions.
    fprintf(stdout, "[mock_pp] reconnect\n");
    s_state = APP_PP_CONN_CONNECTING;
    fire_change();
    // Step to a different slide so the user gets a visible signal that
    // something happened.
    s_idx = 0;
    push_slide_pair();
    s_state = APP_PP_CONN_CONNECTED;
    fire_change();
}

static app_pp_conn_state_t mock_get_state(void) { return s_state; }
static const char         *mock_get_host (void) { return "mock-pp.local"; }
static int                 mock_get_port (void) { return 49850; }

static void mock_register_on_change(app_pp_on_change_t cb, void *ctx) {
    s_change_cb  = cb;
    s_change_ctx = ctx;
}

static const pp_client_iface_t s_iface = {
    .start              = mock_start,
    .stop               = mock_stop,
    .trigger_next       = mock_trigger_next,
    .trigger_previous   = mock_trigger_previous,
    .reconnect          = mock_reconnect,
    .get_state          = mock_get_state,
    .get_host           = mock_get_host,
    .get_port           = mock_get_port,
    .register_on_change = mock_register_on_change,
};

const pp_client_iface_t *app_pp_client(void) { return &s_iface; }
