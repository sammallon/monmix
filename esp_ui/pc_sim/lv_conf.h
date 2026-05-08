// Minimal LVGL configuration for the PC simulator. We only override the
// non-default values; everything else falls through to lv_conf_internal.h's
// defaults. The tablet build does NOT use this file — it gets its config
// from sdkconfig via Kconfig macros.
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// Match the tablet panel: RGB565.
#define LV_COLOR_DEPTH 16

// Use C stdlib for malloc/string/sprintf — easier to debug on PC than
// LVGL's bump allocator, and we have plenty of RAM.
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

// We drive the tick + handler from our own loop and use a single global
// mutex for cross-thread access (mock_esp_lvgl_port.c). LV_OS_NONE keeps
// LVGL itself single-threaded.
#define LV_USE_OS  LV_OS_NONE

// SDL backend. Keep the include path simple — SDL2-VC ships flat headers.
#define LV_USE_SDL                  1
#define LV_SDL_INCLUDE_PATH         <SDL.h>
#define LV_SDL_RENDER_MODE          LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT            1
#define LV_SDL_FULLSCREEN           0
#define LV_SDL_DIRECT_EXIT          1
#define LV_SDL_MOUSEWHEEL_MODE      LV_SDL_MOUSEWHEEL_MODE_ENCODER

// Logging on — level WARN by default. Bump to INFO if a UI bug needs it.
#define LV_USE_LOG          1
#define LV_LOG_LEVEL        LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF       1

// Asserts: keep on, abort on failure so MSVC's debugger catches it.
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
// LV_ASSERT_HANDLER must NOT use assert() under MSVC -- assert() pops the
// "Microsoft Visual C++ Runtime Library" Abort/Retry/Ignore dialog and
// blocks the sim until a human dismisses it, which is a non-starter for
// scripted/CI runs. Print + _exit avoids the dialog entirely while still
// surfacing the message on stderr.
#define LV_ASSERT_HANDLER_INCLUDE   <stdio.h>
#define LV_ASSERT_HANDLER           do { fprintf(stderr, "LVGL ASSERT at %s:%d\n", __FILE__, __LINE__); fflush(stderr); _Exit(98); } while (0);

// Fonts — match what the tablet enables. font_monmix_level.c uses 14.
#define LV_FONT_MONTSERRAT_8    1
#define LV_FONT_MONTSERRAT_10   1
#define LV_FONT_MONTSERRAT_12   1
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_18   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_22   1
#define LV_FONT_MONTSERRAT_24   1
#define LV_FONT_MONTSERRAT_28   1
#define LV_FONT_MONTSERRAT_32   1
#define LV_FONT_MONTSERRAT_48   1

// Themes — dark default (matches device), keep simple too in case prefs
// flip at runtime.
#define LV_USE_THEME_DEFAULT    1
#define LV_USE_THEME_SIMPLE     1
#define LV_USE_THEME_MONO       0

// All standard widgets — the defaults turn most on, but pin them so a
// future LVGL bump can't silently drop one we use.
#define LV_USE_LABEL        1
#define LV_LABEL_TEXT_SELECTION 1
#define LV_LABEL_LONG_TXT_HINT  1
#define LV_USE_BUTTON       1
#define LV_USE_IMAGE        1
#define LV_USE_LINE         1
#define LV_USE_BAR          1
#define LV_USE_SLIDER       1
#define LV_USE_SWITCH       1
#define LV_USE_DROPDOWN     1
#define LV_USE_TEXTAREA     1
#define LV_TEXTAREA_DEF_PWD_SHOW_TIME 0
#define LV_USE_KEYBOARD     1
#define LV_USE_MSGBOX       1
#define LV_USE_SPINNER      1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_TILEVIEW     1
#define LV_USE_LIST         1
#define LV_USE_ROLLER       1
#define LV_USE_TABLE        1
#define LV_USE_SCALE        1
#define LV_USE_CHECKBOX     1
#define LV_USE_ARC          1

// Layouts the UI uses (flex grids in settings overlay).
#define LV_USE_FLEX     1
#define LV_USE_GRID     1

// Snapshot/cache — leave default (off).

// Animations — default on. Slider knob anim, page swipe, etc.

// SW renderer — default on. PPA / DMA2D paths are tablet-only.

// Misc utilities the UI touches.
#define LV_USE_OBSERVER 1
#define LV_USE_SNAPSHOT 0
#define LV_USE_SYSMON   0
#define LV_USE_PROFILER 0

// Tick — we drive lv_tick_inc ourselves from pc_main.c.
#define LV_TICK_CUSTOM 0

#endif // LV_CONF_H
