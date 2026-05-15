#include "app_pp_ui_overlay.h"

#include "lvgl.h"

#include <stddef.h>

// Geometry — landscape 1024×600. Modal panel sized to leave generous
// margin so the keyboard fits underneath when shown.
#define UI_W           1024
#define UI_H            600
#define PANEL_W         700
#define PANEL_H         440
#define TITLE_BAR_H      48
#define KB_H            220

static lv_obj_t *s_scrim;
static lv_obj_t *s_panel;
static lv_obj_t *s_body;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_active_ta;

// ─── Internal helpers ─────────────────────────────────────────────────

static void scrim_clicked_passthrough(lv_event_t *e) {
    (void)e;
    // Empty handler — exists so the scrim absorbs taps that miss the
    // panel rather than passing through to the underlying UI. Closing
    // is via the X button, deliberately — accidental scrim-tap-to-close
    // loses unsaved input.
}

static void close_clicked(lv_event_t *e) {
    (void)e;
    app_pp_ui_overlay_close();
}

static void destroy_keyboard(void) {
    if (s_keyboard) {
        lv_obj_delete(s_keyboard);
        s_keyboard = NULL;
    }
    s_active_ta = NULL;
}

static void hide_keyboard(void) {
    if (s_keyboard) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_keyboard_for(lv_obj_t *ta, bool numeric) {
    if (!s_keyboard) {
        s_keyboard = lv_keyboard_create(lv_screen_active());
        lv_obj_set_size(s_keyboard, UI_W, KB_H);
        lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    lv_keyboard_set_mode(s_keyboard,
                         numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
    s_active_ta = ta;
}

typedef struct {
    bool numeric;
} ta_attach_ctx_t;

static void ta_focus_event(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    ta_attach_ctx_t *c = (ta_attach_ctx_t *)lv_event_get_user_data(e);
    bool numeric = c ? c->numeric : false;
    show_keyboard_for(ta, numeric);
}

// ─── Public API ───────────────────────────────────────────────────────

void app_pp_ui_overlay_close(void) {
    destroy_keyboard();
    if (s_panel) { lv_obj_delete(s_panel); s_panel = NULL; }
    if (s_scrim) { lv_obj_delete(s_scrim); s_scrim = NULL; }
    s_body = NULL;
}

lv_obj_t *app_pp_ui_overlay_open(const char *title,
                                 app_pp_ui_overlay_body_t build_body,
                                 void *ctx) {
    // Close any prior overlay first.
    app_pp_ui_overlay_close();

    lv_obj_t *scr = lv_screen_active();

    // Scrim — dimmed full-screen background that catches stray taps.
    s_scrim = lv_obj_create(scr);
    lv_obj_set_size(s_scrim, UI_W, UI_H);
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_set_style_radius(s_scrim, 0, 0);
    lv_obj_set_style_border_width(s_scrim, 0, 0);
    lv_obj_set_style_pad_all(s_scrim, 0, 0);
    lv_obj_set_style_bg_color(s_scrim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa  (s_scrim, LV_OPA_70, 0);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scrim, scrim_clicked_passthrough, LV_EVENT_CLICKED, NULL);

    // Centred panel.
    s_panel = lv_obj_create(scr);
    lv_obj_set_size(s_panel, PANEL_W, PANEL_H);
    lv_obj_center(s_panel);
    lv_obj_set_style_radius(s_panel, 12, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Title bar.
    lv_obj_t *titlebar = lv_obj_create(s_panel);
    lv_obj_set_size(titlebar, PANEL_W, TITLE_BAR_H);
    lv_obj_set_pos(titlebar, 0, 0);
    lv_obj_set_style_pad_all(titlebar, 8, 0);
    lv_obj_set_style_radius(titlebar, 0, 0);
    lv_obj_set_style_border_width(titlebar, 0, 0);
    lv_obj_clear_flag(titlebar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(titlebar);
    lv_label_set_text(title_lbl, title ? title : "Settings");
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *close_btn = lv_button_create(titlebar);
    lv_obj_set_size(close_btn, 40, 32);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_event_cb(close_btn, close_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);

    // Body — scrollable vertical-flex container under the title bar.
    s_body = lv_obj_create(s_panel);
    lv_obj_set_size(s_body, PANEL_W, PANEL_H - TITLE_BAR_H);
    lv_obj_set_pos(s_body, 0, TITLE_BAR_H);
    lv_obj_set_style_radius(s_body, 0, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 16, 0);
    lv_obj_set_style_pad_row(s_body, 12, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);

    if (build_body) build_body(s_body, ctx);

    return s_panel;
}

void app_pp_ui_overlay_attach_keyboard(lv_obj_t *textarea, bool numeric) {
    if (!textarea) return;
    // Allocate a small ctx that lives as long as the textarea (lvgl
    // deletes user_data via lv_obj_set_user_data when it sees this
    // pattern in practice -- safer to attach to the event subscription
    // which is freed when the object is). Static here avoids the
    // alloc + LVGL doesn't free user_data so we keep it simple.
    static ta_attach_ctx_t text_ctx = { .numeric = false };
    static ta_attach_ctx_t num_ctx  = { .numeric = true  };
    ta_attach_ctx_t *c = numeric ? &num_ctx : &text_ctx;
    lv_obj_add_event_cb(textarea, ta_focus_event, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(textarea, ta_focus_event, LV_EVENT_FOCUSED, c);
}
