// Overlay framework — common modal shell shared by the three settings
// panels (General, WiFi, PP). Each panel calls
// app_pp_ui_overlay_open(title, build_body_fn, ctx) which creates a
// scrim + centred panel with a title bar + X close button + scrollable
// body, then invokes the build_body_fn to populate widgets inside it.
//
// Also owns a single shared on-screen keyboard that pops up when a
// textarea inside any panel receives a CLICKED event. Per LVGL idiom,
// keyboards are heavy widgets; one global instance lazily built on
// first use, hidden between focus events.

#ifndef APP_PP_UI_OVERLAY_H
#define APP_PP_UI_OVERLAY_H

#include "lvgl.h"

// Body builder. Called with the body container (already laid out as a
// vertical flex). The builder adds rows + widgets and returns.
typedef void (*app_pp_ui_overlay_body_t)(lv_obj_t *body, void *ctx);

// Open a modal overlay with the given title. Returns the panel object
// for any post-open tweaks the caller wants; ownership stays inside
// the framework, which closes via the X button.
lv_obj_t *app_pp_ui_overlay_open(const char *title,
                                 app_pp_ui_overlay_body_t build_body,
                                 void *ctx);

// Close the currently open overlay (no-op if none open).
void app_pp_ui_overlay_close(void);

// Attach an on-screen keyboard to the given textarea. Called by panel
// builders after creating a textarea; on tap the keyboard becomes
// visible and types into the textarea. Multiple textareas in one
// overlay share the same keyboard instance; the most-recently-tapped
// one receives input.
void app_pp_ui_overlay_attach_keyboard(lv_obj_t *textarea, bool numeric);

#endif // APP_PP_UI_OVERLAY_H
