#include "app_pp_ui.h"

#include "app_pp_state.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

// Geometry — landscape 1024 × 600 (panel native, no rotation).
//   header   y=  0..60
//   current  y= 60..400  (340 px)
//   NEXT bar y=400..424  ( 24 px)
//   next     y=424..580  (156 px)
//   footer   y=580..600  ( 20 px)
#define UI_W            1024
#define UI_H             600
#define HEADER_H          60
#define CURRENT_H        340
#define NEXT_BAR_H        24
#define NEXT_SLIDE_H     156
#define FOOTER_H          20

#define HEADER_Y           0
#define CURRENT_Y         (HEADER_H)
#define NEXT_BAR_Y        (CURRENT_Y + CURRENT_H)
#define NEXT_SLIDE_Y      (NEXT_BAR_Y + NEXT_BAR_H)
#define FOOTER_Y          (NEXT_SLIDE_Y + NEXT_SLIDE_H)

static const pp_client_iface_t *s_client;

// Widget handles — all created lazily inside mount() and held statically
// because the only consumer is app_pp_state notifications, which arrive
// on the LVGL thread (we route them via lv_async_call from non-LVGL
// callers, matching the monmix locking discipline).
static lv_obj_t *s_clock_lbl;
static lv_obj_t *s_countdown_lbl;
static lv_obj_t *s_prev_btn;
static lv_obj_t *s_next_btn;
static lv_obj_t *s_settings_btn;
static lv_obj_t *s_current_panel;
static lv_obj_t *s_current_lbl;
static lv_obj_t *s_next_bar_lbl;
static lv_obj_t *s_next_panel;
static lv_obj_t *s_next_lbl;
static lv_obj_t *s_footer_title_lbl;
static lv_obj_t *s_footer_status_lbl;

// ─── Event handlers ────────────────────────────────────────────────────

static void on_prev_clicked(lv_event_t *e) {
    (void)e;
    if (s_client && s_client->trigger_previous) s_client->trigger_previous();
}
static void on_next_clicked(lv_event_t *e) {
    (void)e;
    if (s_client && s_client->trigger_next) s_client->trigger_next();
}
static void on_next_preview_clicked(lv_event_t *e) {
    (void)e;
    // Same action as the Next button. Multiple input affordances, one
    // code path. The user requested both — tap-on-next-slide is the
    // ergonomic shortcut, the explicit button is the discoverable one.
    if (s_client && s_client->trigger_next) s_client->trigger_next();
}
static void on_settings_clicked(lv_event_t *e) {
    (void)e;
    // Settings overlay lands in Round 4. For the skeleton, log so the
    // tap registers as wired-up.
    fprintf(stdout, "[app_pp_ui] settings tapped (overlay TBD)\n");
}

// ─── State -> widget refresh ──────────────────────────────────────────

static void refresh_slides(void) {
    const app_pp_slide_t *cur  = app_pp_state_current_slide();
    const app_pp_slide_t *next = app_pp_state_next_slide();

    if (s_current_lbl) {
        lv_label_set_text(s_current_lbl,
                          (cur && cur->present && cur->text[0]) ? cur->text
                                                                : "(no current slide)");
    }
    if (s_next_lbl) {
        lv_label_set_text(s_next_lbl,
                          (next && next->present && next->text[0]) ? next->text
                                                                   : "(end of presentation)");
    }
}

static void refresh_timing(void) {
    const app_pp_timing_t *t = app_pp_state_timing();
    if (s_clock_lbl) {
        lv_label_set_text(s_clock_lbl, (t && t->present) ? t->wall_clock : "--:--:--");
    }
    if (s_countdown_lbl) {
        lv_label_set_text(s_countdown_lbl, (t && t->present) ? t->countdown : "");
    }
}

static void refresh_footer(void) {
    const app_pp_presentation_info_t *p = app_pp_state_presentation();
    if (s_footer_title_lbl) {
        lv_label_set_text(s_footer_title_lbl,
                          (p && p->present && p->title[0]) ? p->title : "(no presentation)");
    }
    // PP-status placeholder — driven by the client iface state. Wire
    // properly once the real backend lands.
    if (s_footer_status_lbl) {
        if (s_client && s_client->get_state) {
            switch (s_client->get_state()) {
                case APP_PP_CONN_CONNECTED:     lv_label_set_text(s_footer_status_lbl, "PP \xE2\x97\x8F"); break;
                case APP_PP_CONN_CONNECTING:    lv_label_set_text(s_footer_status_lbl, "PP ..");           break;
                case APP_PP_CONN_DISCONNECTED:  lv_label_set_text(s_footer_status_lbl, "PP --");           break;
                default:                        lv_label_set_text(s_footer_status_lbl, "PP ?");            break;
            }
        }
    }
}

static void on_state_change(void *ctx) {
    (void)ctx;
    refresh_slides();
    refresh_timing();
    refresh_footer();
}

static void on_client_change(void *ctx) {
    (void)ctx;
    refresh_footer();
}

// ─── Construction ─────────────────────────────────────────────────────

static lv_obj_t *build_header(lv_obj_t *parent) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, UI_W, HEADER_H);
    lv_obj_set_pos(bar, 0, HEADER_Y);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Clock — large, left-aligned
    s_clock_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(s_clock_lbl, &lv_font_montserrat_32, 0);
    lv_obj_align(s_clock_lbl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_label_set_text(s_clock_lbl, "--:--:--");

    // Countdown — smaller, dimmed, to the right of the clock
    s_countdown_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(s_countdown_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_opa(s_countdown_lbl, LV_OPA_60, 0);
    lv_obj_align(s_countdown_lbl, LV_ALIGN_LEFT_MID, 200, 0);
    lv_label_set_text(s_countdown_lbl, "");

    // ⚙ button — far right
    s_settings_btn = lv_button_create(bar);
    lv_obj_set_size(s_settings_btn, 56, HEADER_H - 12);
    lv_obj_align(s_settings_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(s_settings_btn, on_settings_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sgear = lv_label_create(s_settings_btn);
    lv_label_set_text(sgear, "SET");
    lv_obj_set_style_text_font(sgear, &lv_font_montserrat_14, 0);
    lv_obj_center(sgear);

    // Next button — to the left of settings
    s_next_btn = lv_button_create(bar);
    lv_obj_set_size(s_next_btn, 96, HEADER_H - 12);
    lv_obj_align(s_next_btn, LV_ALIGN_RIGHT_MID, -76, 0);
    lv_obj_add_event_cb(s_next_btn, on_next_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nlbl = lv_label_create(s_next_btn);
    lv_label_set_text(nlbl, "Next >");
    lv_obj_set_style_text_font(nlbl, &lv_font_montserrat_18, 0);
    lv_obj_center(nlbl);

    // Prev button — to the left of Next
    s_prev_btn = lv_button_create(bar);
    lv_obj_set_size(s_prev_btn, 96, HEADER_H - 12);
    lv_obj_align(s_prev_btn, LV_ALIGN_RIGHT_MID, -180, 0);
    lv_obj_add_event_cb(s_prev_btn, on_prev_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plbl = lv_label_create(s_prev_btn);
    lv_label_set_text(plbl, "< Prev");
    lv_obj_set_style_text_font(plbl, &lv_font_montserrat_18, 0);
    lv_obj_center(plbl);

    return bar;
}

static lv_obj_t *build_current_panel(lv_obj_t *parent) {
    s_current_panel = lv_obj_create(parent);
    lv_obj_set_size(s_current_panel, UI_W, CURRENT_H);
    lv_obj_set_pos(s_current_panel, 0, CURRENT_Y);
    lv_obj_set_style_pad_all(s_current_panel, 16, 0);
    lv_obj_set_style_radius(s_current_panel, 0, 0);
    lv_obj_set_style_border_width(s_current_panel, 0, 0);
    lv_obj_clear_flag(s_current_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_current_lbl = lv_label_create(s_current_panel);
    lv_obj_set_size(s_current_lbl, UI_W - 32, CURRENT_H - 32);
    lv_obj_center(s_current_lbl);
    lv_obj_set_style_text_font(s_current_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(s_current_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_current_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_current_lbl, "(current slide)");

    return s_current_panel;
}

static lv_obj_t *build_next_band(lv_obj_t *parent) {
    lv_obj_t *band = lv_obj_create(parent);
    lv_obj_set_size(band, UI_W, NEXT_BAR_H);
    lv_obj_set_pos(band, 0, NEXT_BAR_Y);
    lv_obj_set_style_pad_all(band, 0, 0);
    lv_obj_set_style_radius(band, 0, 0);
    lv_obj_set_style_border_width(band, 0, 0);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    s_next_bar_lbl = lv_label_create(band);
    lv_obj_align(s_next_bar_lbl, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_text_font(s_next_bar_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(s_next_bar_lbl, LV_OPA_50, 0);
    lv_label_set_text(s_next_bar_lbl, "NEXT  (tap to advance)");

    return band;
}

static lv_obj_t *build_next_panel(lv_obj_t *parent) {
    s_next_panel = lv_obj_create(parent);
    lv_obj_set_size(s_next_panel, UI_W, NEXT_SLIDE_H);
    lv_obj_set_pos(s_next_panel, 0, NEXT_SLIDE_Y);
    lv_obj_set_style_pad_all(s_next_panel, 16, 0);
    lv_obj_set_style_radius(s_next_panel, 0, 0);
    lv_obj_set_style_border_width(s_next_panel, 0, 0);
    lv_obj_clear_flag(s_next_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Whole next-slide region is a tap target. User requested this in
    // addition to the explicit Next button.
    lv_obj_add_flag(s_next_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_next_panel, on_next_preview_clicked, LV_EVENT_CLICKED, NULL);

    s_next_lbl = lv_label_create(s_next_panel);
    lv_obj_set_size(s_next_lbl, UI_W - 32, NEXT_SLIDE_H - 32);
    lv_obj_center(s_next_lbl);
    lv_obj_set_style_text_font(s_next_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_next_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(s_next_lbl, LV_OPA_70, 0);
    lv_label_set_long_mode(s_next_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_next_lbl, "(next slide)");

    return s_next_panel;
}

static lv_obj_t *build_footer(lv_obj_t *parent) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, UI_W, FOOTER_H);
    lv_obj_set_pos(bar, 0, FOOTER_Y);
    lv_obj_set_style_pad_all(bar, 2, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_footer_title_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(s_footer_title_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_footer_title_lbl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_label_set_text(s_footer_title_lbl, "(no presentation)");

    s_footer_status_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(s_footer_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_footer_status_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_label_set_text(s_footer_status_lbl, "PP --");

    return bar;
}

// ─── Public API ───────────────────────────────────────────────────────

void app_pp_ui_init(const pp_client_iface_t *client) {
    s_client = client;
}

void app_pp_ui_mount(lv_obj_t *parent) {
    if (!parent) parent = lv_screen_active();

    // Subscribe to state + client BEFORE building widgets so the first
    // refresh runs after construction. The state.subscribe / client.
    // register_on_change calls themselves don't fire — we manually
    // refresh once below.
    app_pp_state_subscribe(on_state_change, NULL);
    if (s_client && s_client->register_on_change) {
        s_client->register_on_change(on_client_change, NULL);
    }

    build_header(parent);
    build_current_panel(parent);
    build_next_band(parent);
    build_next_panel(parent);
    build_footer(parent);

    refresh_slides();
    refresh_timing();
    refresh_footer();
}
