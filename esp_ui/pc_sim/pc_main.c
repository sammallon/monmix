// PC sim entry point. Brings up SDL + LVGL, mounts the same fader UI the
// tablet runs, and pumps the LVGL handler loop.
//
// Two run modes:
//   default        — interactive SDL window; mouse drives the UI, ^C/X to quit.
//   --script PATH  — run the commands in PATH single-threaded inside the
//                    LVGL loop, then exit. Used for autonomous repro/verify
//                    runs from the agent harness.
//
// Script grammar (one command per line, blank lines and # comments ignored):
//   sleep MS         pump the LVGL loop for MS milliseconds
//   tap X Y          press + 80ms hold + release at (X,Y)
//   press X Y        mouse-down at (X,Y), no release
//   release          mouse-up at the current cursor position
//   move X Y         mouse-motion to (X,Y)
//   screenshot PATH  save current SDL renderer back-buffer to PATH (.bmp)
//   echo TEXT...     write TEXT to stdout, useful for phase markers
//   quit             clean exit
//
// Each command writes one "OK …" or "ERR …" line to stdout so the harness
// can grep for completion.

#define SDL_MAIN_HANDLED
#include <SDL.h>

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

// Suppress every Windows popup the runtime might raise on a crash:
//   - the MSVC debug-runtime assert dialog (Abort/Retry/Ignore)
//   - the abort()-on-uncaught-fault dialog
//   - the Windows Error Reporting "this program has stopped" dialog
// Each of these blocks the sim until a human dismisses it -- fine when
// you're in front of the machine, useless for scripted/CI runs. With
// these set, a crash exits with a non-zero code and stderr message
// instead of putting up UI.
#ifdef _WIN32
static void quiet_invalid_parameter(
    const wchar_t *expr, const wchar_t *func, const wchar_t *file,
    unsigned int line, uintptr_t reserved) {
    (void)expr; (void)func; (void)file; (void)line; (void)reserved;
    // Swallow. Without this handler MSVC raises STATUS_STACK_BUFFER_OVERRUN
    // (0xC0000409) on certain stdlib calls (e.g. strftime with a non-MSVC
    // format spec). Returning lets the call complete normally with
    // whatever the function's documented "bad input" behaviour is.
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

#include "lvgl.h"
#include "drivers/sdl/lv_sdl_window.h"
#include "drivers/sdl/lv_sdl_mouse.h"
#include "drivers/sdl/lv_sdl_keyboard.h"
#include "drivers/sdl/lv_sdl_mousewheel.h"

#include "app_config.h"
#include "app_ms_client.h"
#include "app_power.h"
#include "app_prefs.h"
#include "app_state.h"
#include "app_storage.h"
#include "app_ui.h"

#include "app_display.h"
#include "app_wifi.h"
#include "esp_lvgl_port.h"
#include "ms_client_real.h"
#include "throttle.h"

#include "nvs.h"  // for nvs_flash_init

#define WIN_W   1024
#define WIN_H   600

static SDL_Window  *g_sdl_window;
static lv_display_t *g_disp;
static int           g_last_x;
static int           g_last_y;
static const ms_client_iface_t *g_ms;

static void inject_motion(int x, int y) {
    g_last_x = x; g_last_y = y;
    SDL_Event ev = (SDL_Event){0};
    ev.type            = SDL_MOUSEMOTION;
    ev.motion.windowID = SDL_GetWindowID(g_sdl_window);
    ev.motion.state    = SDL_GetMouseState(NULL, NULL);
    ev.motion.x        = x;
    ev.motion.y        = y;
    SDL_PushEvent(&ev);
}

static void inject_button(int x, int y, bool down) {
    g_last_x = x; g_last_y = y;
    SDL_Event ev = (SDL_Event){0};
    ev.type            = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    ev.button.windowID = SDL_GetWindowID(g_sdl_window);
    ev.button.button   = SDL_BUTTON_LEFT;
    ev.button.state    = down ? SDL_PRESSED : SDL_RELEASED;
    ev.button.clicks   = 1;
    ev.button.x        = x;
    ev.button.y        = y;
    SDL_PushEvent(&ev);
}

// Pump the LVGL loop for at least `ms` milliseconds, ticking and handling
// timers so animations + lv_async_calls run during scripted waits.
//
// lv_timer_handler must run with lvgl_port_lock held: non-LVGL threads
// (the ms_client worker) take the same lock before invalidating widgets
// or queuing async calls, and unlike the real esp_lvgl_port (which
// interrupts the LVGL task) our SDL-backed mock can't suspend an
// already-running handler. Without this, the worker thread can
// invalidate during a render -> "lv_inv_area asserted at expression:
// !disp->rendering_in_progress" crash.
static void pump_for(uint32_t ms, uint32_t *prev_ticks) {
    uint32_t deadline = SDL_GetTicks() + ms;
    while ((int32_t)(deadline - SDL_GetTicks()) > 0) {
        uint32_t now = SDL_GetTicks();
        lv_tick_inc(now - *prev_ticks);
        *prev_ticks = now;
        lvgl_port_lock(0);
        uint32_t next_ms = lv_timer_handler();
        lvgl_port_unlock();
        if (next_ms > 8) next_ms = 8;
        SDL_Delay(next_ms);
    }
}

static int take_screenshot(const char *path) {
    SDL_Renderer *r = lv_sdl_window_get_renderer(g_disp);
    if (!r) { fprintf(stderr, "screenshot: no renderer\n"); return -1; }
    int w, h;
    if (SDL_GetRendererOutputSize(r, &w, &h) != 0) {
        fprintf(stderr, "screenshot: GetRendererOutputSize: %s\n", SDL_GetError());
        return -2;
    }
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_BGRA32);
    if (!surf) {
        fprintf(stderr, "screenshot: CreateRGBSurface: %s\n", SDL_GetError());
        return -3;
    }
    if (SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_BGRA32, surf->pixels, surf->pitch) != 0) {
        fprintf(stderr, "screenshot: RenderReadPixels: %s\n", SDL_GetError());
        SDL_FreeSurface(surf);
        return -4;
    }
    int rc = SDL_SaveBMP(surf, path);
    SDL_FreeSurface(surf);
    if (rc != 0) {
        fprintf(stderr, "screenshot: SaveBMP(%s): %s\n", path, SDL_GetError());
        return -5;
    }
    return 0;
}

// Strip leading whitespace + trailing newline. Returns pointer into `s`.
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') ++s;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
        *--end = 0;
    }
    return s;
}

// Returns 1 if the script is exhausted and main loop should exit.
static int run_script(FILE *script, uint32_t *prev_ticks) {
    char line[1024];
    while (fgets(line, sizeof(line), script)) {
        char *cmd = trim(line);
        if (!*cmd || *cmd == '#') continue;
        int x, y, ms;
        char path[512];
        char text[1024];
        if (sscanf(cmd, "tap %d %d", &x, &y) == 2) {
            inject_motion(x, y);
            inject_button(x, y, true);
            pump_for(80, prev_ticks);
            inject_button(x, y, false);
            pump_for(40, prev_ticks);
            printf("OK tap %d %d\n", x, y);
        } else if (sscanf(cmd, "press %d %d", &x, &y) == 2) {
            inject_motion(x, y);
            inject_button(x, y, true);
            printf("OK press %d %d\n", x, y);
        } else if (strcmp(cmd, "release") == 0) {
            inject_button(g_last_x, g_last_y, false);
            printf("OK release\n");
        } else if (sscanf(cmd, "move %d %d", &x, &y) == 2) {
            inject_motion(x, y);
            printf("OK move %d %d\n", x, y);
        } else if (sscanf(cmd, "sleep %d", &ms) == 1) {
            pump_for((uint32_t)ms, prev_ticks);
            printf("OK sleep %d\n", ms);
        } else if (sscanf(cmd, "screenshot %511s", path) == 1) {
            // Pump one frame so any pending LVGL work flushes to the
            // renderer before we read it back.
            pump_for(20, prev_ticks);
            int rc = take_screenshot(path);
            printf("%s screenshot %s rc=%d\n", rc == 0 ? "OK" : "ERR", path, rc);
        } else if (sscanf(cmd, "echo %1023[^\n]", text) == 1) {
            printf("OK echo %s\n", text);
        } else if (sscanf(cmd, "set_format %15s", text) == 1) {
            // Mirror app_ui's on_lvl_*_clicked handler exactly: persist
            // the pref AND tell the ms_client to re-subscribe in the new
            // format. Otherwise pref-change observers fire but the lvl
            // subscription stays in the old format.
            app_level_format_t f = (strcmp(text, "db") == 0)
                ? APP_LEVEL_FORMAT_DB : APP_LEVEL_FORMAT_NORM;
            app_prefs_set_level_format(f);
            if (g_ms && g_ms->set_level_format) g_ms->set_level_format(f);
            printf("OK set_format %s\n", text);
        } else if (sscanf(cmd, "set_mix %d", &x) == 1) {
            // Drive the mix-change path the same way the picker does:
            // ms->set_mix() updates the active subscription, AND
            // app_prefs_set_selected_mix_index persists the choice so
            // a subsequent boot restores it (try_apply_ms_info path).
            // Both pieces are part of the on_mix_picker_btn_clicked
            // flow on the device; mirroring them here lets a script
            // exercise either path without faking touch coords.
            if (g_ms && g_ms->set_mix) g_ms->set_mix(x);
            if (x >= 0 && x <= 255) {
                app_prefs_set_selected_mix_index((uint8_t) x);
            }
            printf("OK set_mix %d\n", x);
        } else if (strcmp(cmd, "power_phase") == 0) {
            // Emit current M7 phase + effective timeout so test scripts
            // can grep for transitions instead of inferring from
            // screenshots.
            const char *names[] = {
                "AWAKE", "WARNING", "SLEEP", "WAKE_MENU"
            };
            app_power_phase_t ph = app_power_get_phase();
            const char *n = (ph >= 0 && (size_t)ph < sizeof(names)/sizeof(names[0]))
                                ? names[ph] : "?";
            printf("OK power_phase=%s eff_to_ms=%u\n",
                   n, (unsigned) app_power_get_effective_timeout_ms());
        } else if (strcmp(cmd, "power_kick") == 0) {
            app_power_kick();
            printf("OK power_kick\n");
        } else if (strcmp(cmd, "power_force_sleep") == 0) {
            app_power_force_sleep();
            printf("OK power_force_sleep\n");
        } else if (sscanf(cmd, "power_set_user_timeout_ms %d", &x) == 1) {
            app_power_set_user_timeout_ms((uint32_t) x);
            printf("OK power_set_user_timeout_ms %d\n", x);
        } else if (sscanf(cmd, "chan_id %d", &x) == 1) {
            // Print the MS channel id at app_state slot x. Used by
            // drag-to-reorder tests so they can assert the swap
            // landed without a screenshot.
            int id = app_state_id_for_idx((size_t) x);
            printf("OK chan_id idx=%d ms_id=%d\n", x, id);
        } else if (sscanf(cmd, "prefs_get_color %d", &x) == 1) {
            // Print the saved color-palette index for MS channel id x.
            // Used by master-color tests to verify on_picker_choice
            // wrote through to app_prefs (per-mix-bus, since master id
            // = mix_offset + mix_idx).
            int color = app_prefs_get_channel_color(x);
            printf("OK prefs_get_color id=%d color=%d\n", x, color);
        } else if (sscanf(cmd, "master_state %1023[^\n]", text) == 1
                   && strcmp(text, "get") == 0) {
            // Print the master strip's MS channel id + current name.
            // Drives the master-rename + master-color tests.
            app_channel_t m;
            if (app_state_master_get(&m)) {
                printf("OK master_state id=%d name=\"%s\"\n", m.id, m.name);
            } else {
                printf("ERR master_state: get failed\n");
            }
        } else if (sscanf(cmd, "power_degraded %15s", text) == 1) {
            // Flip the mock wifi state to drive M7's degraded-cap
            // path. The corresponding observer fires, M7's tick_cb
            // recomputes effective_timeout, and the user gets the
            // 60 s scaled cap instead of the chosen duration.
            extern void mock_app_wifi_set_state(app_wifi_state_t);
            app_wifi_state_t s = (strcmp(text, "on") == 0)
                                     ? APP_WIFI_STATE_FAILED
                                     : APP_WIFI_STATE_CONNECTED;
            mock_app_wifi_set_state(s);
            printf("OK power_degraded %s\n", text);
        } else if (strcmp(cmd, "quit") == 0) {
            printf("OK quit\n");
            return 1;
        } else {
            printf("ERR unknown: %s\n", cmd);
        }
        fflush(stdout);
    }
    return 1;
}

int main(int argc, char **argv) {
    silence_windows_crash_popups();
    const char *script_path = NULL;
    const char *ms_host     = NULL;
    int         ms_port     = 8080;
    int         ms_osc_port = 3000;
    const char *protocol    = NULL;       // "ws" | "osc"; NULL -> follow app_config
    bool        do_throttle = false;
    bool        headless    = false;
    // M7 power-save time scale. Default 1/1 (real time); tests pass
    // --power-scale N to make 1 h equal (3600/N) s -- e.g. N=120 makes
    // 1 h = 30 s, the 30 s warn = 250 ms.
    int         power_scale_den = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--script") && i + 1 < argc) {
            script_path = argv[++i];
        } else if (!strcmp(argv[i], "--ms-host") && i + 1 < argc) {
            ms_host = argv[++i];
        } else if (!strcmp(argv[i], "--ms-port") && i + 1 < argc) {
            ms_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--osc-port") && i + 1 < argc) {
            ms_osc_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--protocol") && i + 1 < argc) {
            protocol = argv[++i];
        } else if (!strcmp(argv[i], "--throttle")) {
            do_throttle = true;
        } else if (!strcmp(argv[i], "--headless")) {
            headless = true;
        } else if (!strcmp(argv[i], "--power-scale") && i + 1 < argc) {
            power_scale_den = atoi(argv[++i]);
            if (power_scale_den <= 0) power_scale_den = 1;
        }
    }
    if (do_throttle) throttle_apply();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    lv_init();

    g_disp = lv_sdl_window_create(WIN_W, WIN_H);
    lv_sdl_window_set_title(g_disp, "monmix sim");
    g_sdl_window = lv_sdl_window_get_window(g_disp);

    // Headless: hide the SDL window after creation so scripted runs don't
    // pop a window onto the user's screen. Rendering still happens to the
    // off-screen renderer, so screenshot commands work normally.
    if (headless && g_sdl_window) {
        SDL_HideWindow(g_sdl_window);
    }

    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();
    lv_sdl_mousewheel_create();

    // Match the tablet's boot order from esp_ui_main.c: nvs_flash_init
    // first (so the namespace is mountable), then app_storage_init for
    // the SD mirror, then app_prefs_init reconciles them.
    nvs_flash_init();
    app_storage_init();
    app_prefs_init();

    size_t      n;
    const int  *ids = app_config_channel_ids(&n);
    app_state_init(ids, n);
    app_state_master_set_id(60);
    app_state_master_set_name("Mix 1", false);

    // Pick the protocol. CLI --protocol wins; otherwise fall through to
    // whatever app_config says (the settings panel may have persisted a
    // prior choice into mock NVS).
    bool use_osc = false;
    if (protocol) {
        use_osc = (strcmp(protocol, "osc") == 0);
        // Mirror the firmware: persist via app_config so the settings
        // panel reflects the active backend on first open.
        extern void mock_app_config_seed_protocol(app_ms_protocol_t);
        extern void mock_app_config_seed_osc_port(uint16_t);
        mock_app_config_seed_protocol(use_osc ? APP_MS_PROTOCOL_OSC : APP_MS_PROTOCOL_WS);
        mock_app_config_seed_osc_port((uint16_t)ms_osc_port);
    } else {
        use_osc = (app_config_ms_protocol() == APP_MS_PROTOCOL_OSC);
    }

    const ms_client_iface_t *ms;
    if (ms_host) {
        ms = use_osc ? ms_client_real_osc_create(ms_host, ms_port, ms_osc_port)
                     : ms_client_real_create(ms_host, ms_port);
    } else {
        ms = use_osc ? app_ms_client_osc() : app_ms_client_ws();
    }
    g_ms = ms;
    app_ui_init(ms);
    // M7 power save -- apply scale BEFORE init so the first tick honours
    // the requested timeout. With --power-scale 120, 1 h becomes 30 s
    // and the 30 s warn becomes 250 ms.
    app_power_set_time_scale(1, (uint32_t) power_scale_den);
    app_power_init(ms);
    app_ui_set_channel_total(80);
    app_ui_set_mix_count(14);
    if (!ms_host) {
        // Mock path needs us to seed mix layout; the real path discovers
        // it via /console/information after the worker thread connects.
        ms->set_mix_layout(60, 14);
    }
    ms->start();
    app_ui_present_channels();

    // app_display_apply_theme is only called from app_ui's on_prefs_change
    // (i.e. on actual pref CHANGES). On the tablet, main/app_display.c's
    // app_display_init applies the theme at boot; the sim's mock_display
    // doesn't, so apply once manually here. Otherwise the sim renders in
    // LVGL's bare default theme (light) regardless of the dark pref.
    app_display_apply_theme(app_prefs_get_theme());

    // The mock wifi observer is registered during app_ui_init but the
    // tablet only fires a state-change after the radio actually associates.
    // Force one synchronous dispatch here so on_wifi_state_change runs and
    // start_clock_once registers the clock timer (otherwise the status
    // label sits at "Booting..." forever).
    extern void mock_app_wifi_fire_initial_change(void);
    mock_app_wifi_fire_initial_change();

    FILE *script = NULL;
    if (script_path) {
        script = fopen(script_path, "r");
        if (!script) {
            fprintf(stderr, "could not open script %s\n", script_path);
            return 2;
        }
        // Give LVGL a beat to mount the splash + initial UI before the
        // script's first tap fires; the fader UI's present_channels
        // returned but its layout pass needs one tick to land.
        uint32_t prev = SDL_GetTicks();
        pump_for(200, &prev);
        run_script(script, &prev);
        fclose(script);
        printf("script done\n");
        return 0;
    }

    // Interactive mode: pump until the SDL window is closed. Same
    // lvgl_port_lock discipline as pump_for -- non-LVGL threads (mongoose
    // worker) take the lock to touch widgets, so the handler must hold
    // it during render too.
    uint32_t prev = SDL_GetTicks();
    for (;;) {
        uint32_t now = SDL_GetTicks();
        lv_tick_inc(now - prev);
        prev = now;
        lvgl_port_lock(0);
        uint32_t next_ms = lv_timer_handler();
        lvgl_port_unlock();
        uint32_t cap = throttle_active() ? throttle_frame_ms() : 16u;
        if (next_ms > cap) next_ms = cap;
        SDL_Delay(next_ms);
    }
}
