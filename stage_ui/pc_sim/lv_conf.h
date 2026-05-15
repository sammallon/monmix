// Minimal LVGL configuration for the stage_ui sim. Mirrors esp_ui's
// pc_sim/lv_conf.h since both products target the same panel + same
// LVGL widget surface.
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH 16

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_USE_OS  LV_OS_NONE

// SDL backend.
#define LV_USE_SDL                  1
#define LV_SDL_INCLUDE_PATH         <SDL.h>
#define LV_SDL_RENDER_MODE          LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT            1
#define LV_SDL_FULLSCREEN           0
#define LV_SDL_DIRECT_EXIT          1
#define LV_SDL_MOUSEWHEEL_MODE      LV_SDL_MOUSEWHEEL_MODE_ENCODER

#define LV_USE_LOG          1
#define LV_LOG_LEVEL        LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF       1

#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
// Avoid MSVC's Abort/Retry dialog on assert; print + _Exit instead.
#define LV_ASSERT_HANDLER_INCLUDE   <stdio.h>
#define LV_ASSERT_HANDLER           do { fprintf(stderr, "LVGL ASSERT at %s:%d\n", __FILE__, __LINE__); fflush(stderr); _Exit(98); } while (0);

// Bundled Montserrat sizes. The slide / clock labels use 28, 32, 48.
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

#define LV_USE_THEME_DEFAULT    1
#define LV_USE_THEME_SIMPLE     1
#define LV_USE_THEME_MONO       0

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

#define LV_USE_FLEX     1
#define LV_USE_GRID     1

#define LV_USE_OBSERVER 1
#define LV_USE_SNAPSHOT 1
#define LV_USE_SYSMON   0
#define LV_USE_PROFILER 0

#define LV_TICK_CUSTOM 0

#endif // LV_CONF_H
