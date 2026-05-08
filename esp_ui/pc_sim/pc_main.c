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
#include <SDL_syswm.h>

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
#include "app_prefs.h"
#include "app_state.h"
#include "app_storage.h"
#include "app_ui.h"

#include "app_display.h"
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

// All exit paths (SDL_QUIT, SIGINT/SIGTERM, atexit, `quit` script cmd)
// route through here so MS gets unsubscribes + WS close instead of an
// abrupt TCP RST. Idempotent via the static flag -- atexit fires after
// a signal handler already ran, and the script `quit` flow exits via
// return-from-main which also fires atexit.
static volatile int g_shutdown_done = 0;
static void shutdown_ms_once(void) {
    if (g_shutdown_done) return;
    g_shutdown_done = 1;
    if (g_ms && g_ms->shutdown_graceful) g_ms->shutdown_graceful();
}

// SIGINT/SIGTERM. Print and exit cleanly so atexit + flush run; signal
// handlers can't safely call into mongoose directly, but exit() from
// inside a handler is async-signal-safe and triggers our atexit hook.
static void on_signal(int sig) {
    fprintf(stderr, "pc_sim: caught signal %d, exiting\n", sig);
    fflush(stderr);
    exit(0);
}

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

// LVGL-snapshot-backed BMP write. Used as the fallback when the SDL
// video driver doesn't provide a renderer (e.g. headless+dummy). Renders
// the LVGL active screen into a caller-allocated XRGB8888 buffer, wraps
// it in an SDL_Surface (no renderer required), and saves a BMP. The
// XRGB8888 byte order matches SDL_PIXELFORMAT_BGRA32 on little-endian
// so the wrap is a straight pointer reinterpret.
static int take_screenshot_via_lvgl(const char *path) {
    lv_obj_t *scr = lv_screen_active();
    if (!scr) { fprintf(stderr, "screenshot: no active screen\n"); return -10; }
    int32_t w = lv_obj_get_width(scr);
    int32_t h = lv_obj_get_height(scr);
    if (w <= 0 || h <= 0) { fprintf(stderr, "screenshot: bad screen dims\n"); return -11; }

    size_t buf_size = (size_t) w * (size_t) h * 4;
    uint8_t *pixels = (uint8_t *) malloc(buf_size);
    if (!pixels) { fprintf(stderr, "screenshot: alloc failed\n"); return -12; }

    lv_image_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));
    if (lv_snapshot_take_to_buf(scr, LV_COLOR_FORMAT_XRGB8888, &dsc, pixels, buf_size) != LV_RESULT_OK) {
        fprintf(stderr, "screenshot: lv_snapshot_take_to_buf failed\n");
        free(pixels);
        return -13;
    }

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, w, h, 32, w * 4, SDL_PIXELFORMAT_BGRA32);
    if (!surf) {
        fprintf(stderr, "screenshot: SDL_CreateRGBSurfaceWithFormatFrom: %s\n", SDL_GetError());
        free(pixels);
        return -14;
    }
    int rc = SDL_SaveBMP(surf, path);
    SDL_FreeSurface(surf);
    free(pixels);
    if (rc != 0) {
        fprintf(stderr, "screenshot: SaveBMP(%s): %s\n", path, SDL_GetError());
        return -15;
    }
    return 0;
}

static int take_screenshot(const char *path) {
    SDL_Renderer *r = lv_sdl_window_get_renderer(g_disp);
    if (!r) {
        // dummy/offscreen video drivers don't expose a renderer; render
        // the screen via LVGL's snapshot API instead.
        return take_screenshot_via_lvgl(path);
    }
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
        } else if (strncmp(cmd, "chpick_save", 11) == 0) {
            // Comma-separated id list, e.g. "chpick_save 0,2,5". Empty list
            // means "track no channels"; bare "chpick_save" is the same.
            // Drives app_ui_chpick_apply directly so the test exercises the
            // stop+rebuild+restart path without driving the picker UI.
            int ids[64];
            int n = 0;
            const char *p = cmd + 11;
            while (*p == ' ' || *p == '\t') ++p;
            while (*p && n < (int)(sizeof(ids)/sizeof(ids[0]))) {
                int v;
                int consumed = 0;
                if (sscanf(p, "%d%n", &v, &consumed) == 1 && consumed > 0) {
                    ids[n++] = v;
                    p += consumed;
                    if (*p == ',') ++p;
                } else {
                    break;
                }
            }
            // Hold lvgl_port_lock the same way an LVGL-task event would --
            // app_ui_chpick_apply documents that requirement.
            lvgl_port_lock(0);
            app_ui_chpick_apply(ids, (size_t) n);
            lvgl_port_unlock();
            printf("OK chpick_save n=%d\n", n);
        } else if (strcmp(cmd, "dump_tiles") == 0) {
            // Emits one settings_tile line per channel grid cell. The
            // settings overlay must be open already (gear-icon tap then
            // sleep). Holds lvgl_port_lock for the dump itself even
            // though the read is just lv_obj_get_x — keep the discipline
            // consistent with the rest of the script-driven hooks.
            lvgl_port_lock(0);
            app_ui_settings_dump_tiles();
            lvgl_port_unlock();
            printf("OK dump_tiles\n");
        } else if (strncmp(cmd, "mcfg_apply", 10) == 0) {
            // "mcfg_apply HOST PORT" — drive the MS-config Save path with
            // the given host + port. Same code path the user takes via
            // taps + the `type` REPL command. Used by the names-on-
            // reconfigure regression to flip host without faking taps.
            char host_buf[64];
            int  port_num;
            if (sscanf(cmd, "mcfg_apply %63s %d", host_buf, &port_num) != 2) {
                printf("ERR mcfg_apply usage: mcfg_apply HOST PORT\n");
            } else {
                char port_buf[12];
                snprintf(port_buf, sizeof(port_buf), "%d", port_num);
                lvgl_port_lock(0);
                app_ui_mcfg_apply(host_buf, port_buf);
                lvgl_port_unlock();
                printf("OK mcfg_apply %s %d\n", host_buf, port_num);
            }
        } else if (strncmp(cmd, "type ", 5) == 0) {
            // Mirror the device's `type` REPL command: write into whichever
            // textarea is focused. Useful so a single sim script can stand
            // in for a tap-and-type sequence without coordinate hardcoding.
            // Walks the focused indev's group first, then falls back to
            // scanning the screen tree for a focused textarea.
            const char *text = cmd + 5;
            while (*text == ' ' || *text == '\t') ++text;
            lvgl_port_lock(0);
            lv_obj_t *ta = NULL;
            lv_indev_t *indev = NULL;
            while ((indev = lv_indev_get_next(indev)) != NULL) {
                lv_group_t *g = lv_indev_get_group(indev);
                if (!g) continue;
                lv_obj_t *f = lv_group_get_focused(g);
                if (f && lv_obj_check_type(f, &lv_textarea_class)) { ta = f; break; }
            }
            if (ta) lv_textarea_set_text(ta, text);
            lvgl_port_unlock();
            if (ta) printf("OK type %s\n", text);
            else    printf("ERR type: no focused textarea\n");
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
        } else if (strcmp(cmd, "quit") == 0) {
            // Flush MS before printing the OK marker so test stdout
            // captures the full unsubscribe sequence; the harness
            // greps for "OK quit" as the run-completed marker.
            shutdown_ms_once();
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
        }
    }
    if (do_throttle) throttle_apply();

    // Headless mode: route SDL through the dummy video driver so no real
    // OS window is created. The "offscreen" driver would also work (and
    // would even keep an SDL_Renderer alive) but it isn't compiled into
    // the SDL2 dev pack we use, so dummy + a snapshot-based screenshot
    // fallback is the working combo. Earlier attempts at hiding the
    // window post-create couldn't prevent the activation flash because
    // on Windows SDL_CreateWindow shows + activates the window before
    // any SDL_HideWindow call gets a chance to run.
    if (headless) {
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    }

    // Suppress focus-stealing on every internal SDL_RaiseWindow. Cheap
    // insurance for non-headless interactive runs.
    SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "0");

    // Register exit hooks BEFORE the MS client comes up. atexit covers
    // normal return-from-main + exit(); the signal handlers turn ^C and
    // SIGTERM into a clean exit() call so the atexit chain runs.
    atexit(shutdown_ms_once);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

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
    //
    // On Windows, lv_sdl_window_create shows + raises the window before we
    // get a chance to call SDL_HideWindow, so for a moment the window
    // appears and grabs focus from whatever the user is typing into. Set
    // WS_EX_NOACTIVATE so the OS won't activate the window even on that
    // brief flash, then hide it permanently.
    if (headless && g_sdl_window) {
#ifdef _WIN32
        SDL_SysWMinfo wmi;
        SDL_VERSION(&wmi.version);
        if (SDL_GetWindowWMInfo(g_sdl_window, &wmi)) {
            HWND hwnd = wmi.info.win.window;
            LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
        }
#endif
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
    // it during render too. SDL_QUIT (window close) exits via the same
    // atexit path as the script `quit` command, so MS gets unsubscribes
    // + WS close before we tear down.
    uint32_t prev = SDL_GetTicks();
    for (;;) {
        SDL_Event ev;
        while (SDL_PeepEvents(&ev, 1, SDL_PEEKEVENT, SDL_QUIT, SDL_QUIT) > 0) {
            // PeepEvents leaves it in the queue for LVGL's SDL driver to
            // consume too; we just observe and break out.
            return 0;   // atexit -> shutdown_ms_once -> graceful close
        }
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
