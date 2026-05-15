// app_pp_ui — main stage display page. Landscape 1024 × 600.
//
// Layout:
//   header  (60 px):   clock + countdown + Prev / Next buttons + ⚙
//   current (340 px):  current-slide text region
//   next    (180 px):  NEXT label band + next-slide text region (CLICKABLE)
//   footer  ( 20 px):  presentation title + WiFi/PP status placeholders
//
// All slide content is rendered as text via bundled Montserrat. Slide
// thumbnails are explicitly NOT used as the primary display (too low-res);
// they're reserved for a later gallery / picker secondary screen.
//
// Built without HW-only deps so it links into both the sim and (later)
// the IDF build.

#ifndef APP_PP_UI_H
#define APP_PP_UI_H

#include "app_pp_client.h"

// Wire the iface in early. Must be called before app_pp_ui_mount.
void app_pp_ui_init(const pp_client_iface_t *client);

// Build the widget tree on the given parent (typically lv_screen_active()).
// Caller must hold lvgl_port_lock (or be on the LVGL task) — matches the
// monmix convention.
void app_pp_ui_mount(struct _lv_obj_t *parent);

#endif // APP_PP_UI_H
