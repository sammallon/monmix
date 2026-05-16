// PC sim entry point for esp_pp_display. SDL window + LVGL via the SDL
// backend, mock_pp_client + mock_app_wifi behind app_ui.c. Keyboard
// shortcuts drive sample state injection so the layout can be exercised
// without a live ProPresenter.
//
// Controls (when the SDL window has focus):
//   1..5     pick one of five sample current-slide texts
//   N        toggle "has next slide"
//   M        cycle stage message (empty -> short -> long -> empty)
//   T        cycle the timer state (stopped -> running 00:01:23 ticking
//                                   -> overrun -00:00:05 -> stopped)
//   W        toggle WiFi state (connected <-> disconnected)
//   P        toggle PP connection state (connected <-> reconnecting)
//   ESC      quit
//   Q        quit

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "drivers/sdl/lv_sdl_window.h"
#include "drivers/sdl/lv_sdl_mouse.h"
#include "drivers/sdl/lv_sdl_keyboard.h"

#include "app_pp_state.h"
#include "app_pp_client.h"
#include "app_wifi.h"
#include "app_ui.h"

extern void mock_pp_set_conn_state(app_pp_conn_state_t s);
extern void mock_wifi_set_state(app_wifi_state_t s);

#define WIN_W 1024
#define WIN_H 600

// ---------- sample data ----------

static const char *SAMPLE_SLIDES[] = {
    "Amazing grace, how sweet the sound\nThat saved a wretch like me\nI once was lost but now am found\nWas blind but now I see",
    "When peace like a river attendeth my way\nWhen sorrows like sea billows roll\nWhatever my lot, Thou hast taught me to say\nIt is well, it is well with my soul",
    "Holy holy holy\nLord God Almighty\nEarly in the morning our song shall rise to Thee",
    "How great is our God\nSing with me, how great is our God\nAnd all will see how great, how great is our God",
    "(empty slide)",
};
static const size_t SAMPLE_SLIDES_N = sizeof(SAMPLE_SLIDES) / sizeof(SAMPLE_SLIDES[0]);

static const char *SAMPLE_NEXT_SLIDE =
    "'Twas grace that taught my heart to fear\nAnd grace my fears relieved";

static const char *SAMPLE_STAGE_MSGS[] = {
    "",
    "Go to the bridge",
    "Skip verse 3 -- going straight to chorus then prayer",
};
static size_t s_stage_msg_idx = 0;

static int  s_slide_idx     = 0;
static bool s_has_next      = true;
static int  s_timer_state   = 0;   // 0 stopped, 1 running, 2 overrun
static bool s_wifi_on       = true;
static bool s_pp_connected  = true;

// ---------- helpers ----------

static void load_slide(int idx)
{
    if (idx < 0) idx = 0;
    if ((size_t) idx >= SAMPLE_SLIDES_N) idx = (int)(SAMPLE_SLIDES_N - 1);
    s_slide_idx = idx;

    app_pp_slide_t cur = {0};
    cur.valid = true;
    strncpy(cur.text, SAMPLE_SLIDES[idx], sizeof(cur.text) - 1);
    snprintf(cur.uuid, sizeof(cur.uuid), "sample-uuid-%d", idx);

    app_pp_slide_t nxt = {0};
    if (s_has_next) {
        nxt.valid = true;
        strncpy(nxt.text, SAMPLE_NEXT_SLIDE, sizeof(nxt.text) - 1);
        snprintf(nxt.uuid, sizeof(nxt.uuid), "next-uuid-%d", idx);
    }
    app_pp_state_set_slides(&cur, &nxt);
}

static void cycle_stage_msg(void)
{
    s_stage_msg_idx = (s_stage_msg_idx + 1) %
                      (sizeof(SAMPLE_STAGE_MSGS) / sizeof(SAMPLE_STAGE_MSGS[0]));
    app_pp_state_set_stage_message(SAMPLE_STAGE_MSGS[s_stage_msg_idx]);
}

static void apply_timer_state(void)
{
    app_pp_timer_t t[1] = {0};
    strncpy(t[0].uuid, "sim-timer-0", sizeof(t[0].uuid) - 1);
    strncpy(t[0].name, "Segment Countdown", sizeof(t[0].name) - 1);
    switch (s_timer_state) {
    case 1:
        strncpy(t[0].time_str, "00:01:23", sizeof(t[0].time_str) - 1);
        t[0].state = APP_PP_TIMER_RUNNING;
        break;
    case 2:
        strncpy(t[0].time_str, "-00:00:05", sizeof(t[0].time_str) - 1);
        t[0].state = APP_PP_TIMER_OVERRUN;
        break;
    case 0:
    default:
        strncpy(t[0].time_str, "00:20:00", sizeof(t[0].time_str) - 1);
        t[0].state = APP_PP_TIMER_STOPPED;
        break;
    }
    app_pp_state_set_timers(t, 1);
}

static uint32_t s_running_timer_start_ms;

static void tick_running_timer_if_needed(uint32_t now_ms)
{
    if (s_timer_state != 1) return;
    if (now_ms - s_running_timer_start_ms < 1000) return;
    s_running_timer_start_ms = now_ms;

    app_pp_timer_t cur[APP_PP_MAX_TIMERS];
    size_t n = app_pp_state_get_timers(cur, APP_PP_MAX_TIMERS);
    if (n == 0) return;
    int h = 0, m = 0, s = 0;
    if (sscanf(cur[0].time_str, "%d:%d:%d", &h, &m, &s) == 3) {
        s++;
        if (s >= 60) { s = 0; m++; }
        if (m >= 60) { m = 0; h++; }
        snprintf(cur[0].time_str, sizeof(cur[0].time_str),
                 "%02d:%02d:%02d", h, m, s);
        app_pp_state_set_timers(cur, n);
    }
}

// ---------- main ----------

int main(int argc, char *argv[])
{
    (void) argc; (void) argv;
    fprintf(stderr, "esp_pp_display PC sim -- 1024x600 stage display\n");
    fprintf(stderr, "  Keys: 1-5 slide, N next-slide toggle, M stage msg,\n");
    fprintf(stderr, "        T timer state, W wifi toggle, P pp toggle,\n");
    fprintf(stderr, "        ESC / Q quit\n");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    lv_init();
    lv_display_t *disp = lv_sdl_window_create(WIN_W, WIN_H);
    if (!disp) {
        fprintf(stderr, "lv_sdl_window_create failed\n");
        return 1;
    }
    lv_sdl_window_set_title(disp, "esp_pp_display sim");
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();

    // Subsystems used by app_ui.
    app_pp_state_init();
    const app_pp_client_iface_t *pp = app_pp_client_tcp();
    app_ui_init(pp);

    // Seed with sample data so the layout is populated on first frame.
    load_slide(0);
    apply_timer_state();

    uint32_t last_tick = SDL_GetTicks();
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = false; break; }
            if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_ESCAPE || k == SDLK_q) { running = false; break; }
                if (k >= SDLK_1 && k <= SDLK_5) {
                    load_slide(k - SDLK_1);
                } else if (k == SDLK_n) {
                    s_has_next = !s_has_next;
                    load_slide(s_slide_idx);
                } else if (k == SDLK_m) {
                    cycle_stage_msg();
                } else if (k == SDLK_t) {
                    s_timer_state = (s_timer_state + 1) % 3;
                    s_running_timer_start_ms = SDL_GetTicks();
                    apply_timer_state();
                } else if (k == SDLK_w) {
                    s_wifi_on = !s_wifi_on;
                    mock_wifi_set_state(s_wifi_on ? APP_WIFI_STATE_CONNECTED
                                                  : APP_WIFI_STATE_FAILED);
                } else if (k == SDLK_p) {
                    s_pp_connected = !s_pp_connected;
                    mock_pp_set_conn_state(s_pp_connected ? APP_PP_CONN_CONNECTED
                                                          : APP_PP_CONN_RECONNECTING);
                }
            }
        }
        uint32_t now = SDL_GetTicks();
        uint32_t dt  = now - last_tick;
        last_tick    = now;
        lv_tick_inc(dt);
        tick_running_timer_if_needed(now);
        uint32_t next_ms = lv_timer_handler();
        SDL_Delay(next_ms < 5 ? 5 : (next_ms > 30 ? 30 : next_ms));
    }

    SDL_Quit();
    return 0;
}
