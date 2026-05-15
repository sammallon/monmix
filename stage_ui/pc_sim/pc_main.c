// PC sim entry point for stage_ui. Brings up SDL + LVGL, mounts the
// app_pp_ui page, wires the mock PP client, and pumps the LVGL handler
// loop until the SDL window is closed (or ^C).
//
// Same general shape as esp_ui/pc_sim/pc_main.c, slimmed for the skeleton:
//   * no --ms-host / --script / --headless flags yet (Round 2+)
//   * no settings overlay, no theme switching at runtime
//   * no graceful-shutdown dance (no live network connection to flush)

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <crtdbg.h>
#endif

#include "lvgl.h"
#include "drivers/sdl/lv_sdl_window.h"
#include "drivers/sdl/lv_sdl_mouse.h"
#include "drivers/sdl/lv_sdl_keyboard.h"
#include "drivers/sdl/lv_sdl_mousewheel.h"

#include "app_pp_client.h"
#include "app_pp_state.h"
#include "app_pp_ui.h"

#define WIN_W   1024
#define WIN_H    600

// ─── Windows crash popup suppression ───────────────────────────────────
// Without these, MSVC's debug runtime raises Abort/Retry/Ignore dialogs
// on assertions and invalid stdlib args, blocking scripted runs.
#ifdef _WIN32
static void quiet_invalid_parameter(
    const wchar_t *expr, const wchar_t *func, const wchar_t *file,
    unsigned int line, uintptr_t reserved) {
    (void)expr; (void)func; (void)file; (void)line; (void)reserved;
}
#endif
static void silence_windows_crash_popups(void) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(quiet_invalid_parameter);
    _set_thread_local_invalid_parameter_handler(quiet_invalid_parameter);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);
#endif
}

static volatile int g_quit;
static void sigterm_handler(int sig) {
    (void)sig;
    g_quit = 1;
}

static void apply_dark_theme(lv_display_t *disp) {
    lv_theme_t *t = lv_theme_default_init(
        disp,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_GREY),
        /*dark=*/true,
        LV_FONT_DEFAULT);
    lv_display_set_theme(disp, t);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa  (lv_screen_active(), LV_OPA_COVER, 0);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    silence_windows_crash_popups();
    signal(SIGINT,  sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    lv_init();

    lv_display_t *disp = lv_sdl_window_create(WIN_W, WIN_H);
    if (!disp) {
        fprintf(stderr, "lv_sdl_window_create failed\n");
        SDL_Quit();
        return 1;
    }
    lv_sdl_window_set_title(disp, "stage_ui sim");

    // Mouse acts as touch. Keyboard + mousewheel registered for parity
    // with esp_ui's sim — harmless, opens the door for ScreenSaver-style
    // scripted-input features later.
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();
    lv_sdl_mousewheel_create();

    apply_dark_theme(disp);

    app_pp_state_init();
    app_pp_ui_init(app_pp_client());
    app_pp_ui_mount(lv_screen_active());

    if (!app_pp_client()->start()) {
        fprintf(stderr, "pp_client.start failed\n");
        return 1;
    }

    fprintf(stdout, "stage_ui sim running. Click slide buttons / next-preview / SET. ^C to quit.\n");

    // Drive LVGL at ~50 Hz. lv_tick_inc gives the engine real-time so
    // animations / timers work; lv_timer_handler runs pending callbacks
    // including our mock auto-advance.
    uint32_t last_tick = SDL_GetTicks();
    while (!g_quit) {
        uint32_t now = SDL_GetTicks();
        uint32_t delta = now - last_tick;
        last_tick = now;

        lv_tick_inc(delta);
        lv_timer_handler();

        // LV_SDL_DIRECT_EXIT=1 in lv_conf.h means the SDL window's X
        // button calls exit(0) directly. We still pump SDL events here
        // so signals / focus changes propagate.
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { g_quit = 1; break; }
        }

        SDL_Delay(20);
    }

    app_pp_client()->stop();
    lv_deinit();
    SDL_Quit();
    return 0;
}
