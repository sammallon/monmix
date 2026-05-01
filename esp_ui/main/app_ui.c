#include "app_ui.h"
#include "app_display.h"
#include "app_logd.h"
#include "app_prefs.h"
#include "app_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "app_ui";

// Layout:
//   1024x600 panel (LVGL software-rotated 180°). Each "page" is one tile in
//   an lv_tileview. 12 faders fit across 1024 px (~85 px slot, 72 px box):
//   the default 12-channel config sits on a single page so a musician sees
//   their full mix at a glance. Reconfiguring to >12 channels overflows
//   into a 2nd page and the indicator dots show up automatically.
#define FADERS_PER_PAGE     12
#define MAX_PAGES           ((APP_CONFIG_MAX_CHANNELS + FADERS_PER_PAGE - 1) / FADERS_PER_PAGE)

#define SCREEN_W            1024
#define SCREEN_H            600
#define STATUS_H            32
#define INDICATOR_H         28
#define TILEVIEW_Y          STATUS_H
#define TILEVIEW_H          (SCREEN_H - STATUS_H - INDICATOR_H)
#define FADER_BOX_W         72
#define FADER_BOX_H         500
#define FADER_BOX_PAD       8
#define SLIDER_W            28
// Slider is centered in the box. Vertical budget within the 484 px inner
// area, top-to-bottom: name label (~20) + slider (centered) + mute button
// (32 at bottom-mid offset -28) + value label (~20 at bottom). The LVGL
// slider knob is bigger than the math math (the styled thumb extends past
// the track by more than its bare radius), so even SLIDER_H=320 left the
// knob kissing the MUTE button at value=0. 300 leaves an unambiguous
// gap; we lose 20 px of slider travel which is invisible at the 0–100
// integer scale we display.
#define SLIDER_H            300
#define MUTE_BTN_W          50
#define MUTE_BTN_H          32
#define DOT_SIZE            12
#define DOT_GAP             10

typedef struct {
    lv_obj_t *slider;
    lv_obj_t *label_name;
    lv_obj_t *label_val;
    lv_obj_t *btn_mute;
    lv_obj_t *signal_dot;
} fader_widgets_t;

#define SIGNAL_DOT_SIZE     10
#define DEFAULT_SLIDER_HEX  0x4080E0   // medium blue when no per-channel color set

// 8-color palette for the per-channel color tag — see app_prefs / set-color.
// Roughly evenly spaced around the hue wheel; chose hex values that read
// well on both the default (light box) and the future low-light theme.
// Applied to the slider's filled indicator + knob, so the channel's
// identity is visible at a glance from across the stage.
static const uint32_t COLOR_PALETTE[8] = {
    0xE04040,  // red
    0xE09040,  // orange
    0xE0D040,  // yellow
    0x40C040,  // green
    0x40C0E0,  // cyan
    0x4080E0,  // blue
    0xC060E0,  // purple
    0xE060A0,  // pink
};

static fader_widgets_t          s_widgets[APP_CONFIG_MAX_CHANNELS];
static lv_obj_t                *s_status_label;
static lv_obj_t                *s_tileview;
static lv_obj_t                *s_page_tiles[MAX_PAGES];
static lv_obj_t                *s_page_dots[MAX_PAGES];
static size_t                   s_page_count;
static const ms_client_iface_t *s_ms;

// Settings overlay — declared up here so the gear button's event handler
// can reach it. Overlay is created lazily on first open.
static lv_obj_t *s_settings_overlay;
static lv_obj_t *s_color_swatches[APP_CONFIG_MAX_CHANNELS];
static lv_obj_t *s_lvl_norm_btn;
static lv_obj_t *s_lvl_db_btn;
static lv_obj_t *s_sig_buttons[3];   // none / signal-present / meter
static lv_obj_t *s_theme_buttons[2]; // dark / light

static void settings_open(void);
static void settings_close(void);
static void on_gear_clicked(lv_event_t *e);

// Rate-limit outbound SETs per channel so a fast drag doesn't flood MS
// (each SET produces a broadcast echo, doubling on-wire traffic). 50 ms
// = 20 Hz feels live to the user but keeps the websocket task from
// monopolizing its core.
#define SET_MIN_INTERVAL_MS 50
static uint32_t s_last_send_ms[APP_CONFIG_MAX_CHANNELS];

static void apply_status(void *arg)
{
    char *text = (char *)arg;
    if (s_status_label) {
        lv_label_set_text(s_status_label, text);
    }
    free(text);
}

void app_ui_set_status(const char *text)
{
    if (!text) return;
    char *copy = strdup(text);
    if (!copy) return;
    // lv_async_call mutates LVGL's timer list, so it MUST hold lvgl_port_lock
    // when called from a non-LVGL task. Without it, the WS / wifi tasks race
    // the LVGL task and updates get lost.
    if (!lvgl_port_lock(100)) {
        free(copy);
        return;
    }
    if (lv_async_call(apply_status, copy) != LV_RESULT_OK) {
        free(copy);
    }
    lvgl_port_unlock();
}

static void send_level_now(size_t idx, float level)
{
    int ch_id = app_state_id_for_idx(idx);
    if (s_ms && ch_id >= 0) {
        APP_LOGD_T("app_ui", "fader idx=%u ch=%d -> %.3f",
                   (unsigned) idx, ch_id, (double) level);
        s_ms->set_level(ch_id, level);
    }
    s_last_send_ms[idx] = (uint32_t)(esp_timer_get_time() / 1000);
}

static void on_slider_changed(lv_event_t *e)
{
    size_t    idx    = (size_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int       v      = lv_slider_get_value(slider);
    float     level  = (float)v / 100.0f;

    // Update local state without notifying — UI already reflects this value.
    app_state_set_level(idx, level, false);

    // Rate-limit outbound SETs to ~20 Hz per channel. Each SET produces a
    // server-snap echo on the same WS, so unlimited drag-frequency SETs
    // doubled into a flood that monopolized the websocket task on CPU 1.
    // The final value is sent on slider release, see on_slider_released.
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_last_send_ms[idx] >= SET_MIN_INTERVAL_MS) {
        send_level_now(idx, level);
    }

    // Mid-drag the dB hasn't echoed back yet, so use the slider's
    // immediate value for the readout. The post-drag MS broadcast
    // updates app_state.level_db and apply_pending takes over.
    char buf[12];
    if (app_prefs_get_level_format() == APP_LEVEL_FORMAT_DB) {
        snprintf(buf, sizeof(buf), "...");
    } else {
        snprintf(buf, sizeof(buf), "%d", v);
    }
    lv_label_set_text(s_widgets[idx].label_val, buf);
}

static void on_slider_released(lv_event_t *e)
{
    size_t    idx    = (size_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int       v      = lv_slider_get_value(slider);
    // Always emit the final value so the rate-limiter can't swallow the
    // last position the user landed on.
    send_level_now(idx, (float)v / 100.0f);
}

static void on_mute_clicked(lv_event_t *e)
{
    size_t    idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);

    // Read canonical state from app_state and toggle. We don't set
    // LV_OBJ_FLAG_CHECKABLE on the button (that fires on press and a
    // drag-off-then-release would still toggle), so we drive the visual
    // CHECKED state ourselves on each click.
    app_channel_t ch;
    if (!app_state_get(idx, &ch)) return;
    bool new_mute = !ch.mute;

    int ch_id = app_state_id_for_idx(idx);
    APP_LOGD_I("app_ui", "mute idx=%u ch=%d -> %d",
               (unsigned) idx, ch_id, (int) new_mute);
    if (s_ms && s_ms->set_mute && ch_id >= 0) {
        s_ms->set_mute(ch_id, new_mute);
    }
    app_state_set_mute(idx, new_mute, false);

    // Optimistic UI: flip the visual immediately so the press feels live.
    // The MS broadcast echo will hit apply_pending shortly and reconfirm.
    if (new_mute) lv_obj_add_state   (btn, LV_STATE_CHECKED);
    else          lv_obj_remove_state(btn, LV_STATE_CHECKED);
}

// Inbound updates from the WS task are coalesced via a per-channel dirty
// flag plus a single in-flight async sweep. The sweep, running under LVGL's
// task, reads each channel's latest state and applies it to the widgets.
//
// This replaces the original "malloc a snapshot per WS message + queue an
// async per snapshot" pattern, which had two problems:
//   1. lv_async_call mutates LVGL's timer list and was being called from the
//      WS task without lvgl_port_lock → races dropped occasional updates,
//      including the final "user released" broadcast (visible symptom: the
//      device slider not landing on the same value as the MS slider).
//   2. During a drag MS broadcasts at ~100 Hz, so every drag allocated 200+
//      tiny structs that all had to be freed by the LVGL task in order.
//
// The dirty-flag scheme guarantees the last value wins (the sweep reads
// fresh state at apply time) and only one async is ever in flight.
static volatile bool s_dirty[APP_CONFIG_MAX_CHANNELS];
static volatile bool s_sweep_queued;

static void apply_pending(void *unused)
{
    (void)unused;
    // Clear the queued flag BEFORE iterating so any state change that fires
    // during the sweep (and after we've passed its index) re-arms a fresh
    // sweep rather than getting silently swallowed.
    s_sweep_queued = false;
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) {
        if (!s_dirty[i]) continue;
        s_dirty[i] = false;
        app_channel_t ch;
        if (!app_state_get(i, &ch)) continue;
        int v = (int)(ch.level * 100.0f);
        // LV_ANIM_OFF: network echoes can arrive every ~10ms during a drag;
        // queueing/cancelling 200ms animations on each one trashes LVGL.
        lv_slider_set_value(s_widgets[i].slider, v, LV_ANIM_OFF);
        char buf[12];
        if (app_prefs_get_level_format() == APP_LEVEL_FORMAT_DB) {
            // MS reports min ≈ -138 dB on the Si Expression 2; display "-INF"
            // there since the channel is effectively off. Above the floor we
            // round to the nearest dB — a half-dB step is finer than the
            // mixer's quantization, no need for decimals on a glance-readout.
            if (ch.level_db <= -138.0f) {
                snprintf(buf, sizeof(buf), "-INF");
            } else {
                snprintf(buf, sizeof(buf), "%.0f dB", ch.level_db);
            }
        } else {
            snprintf(buf, sizeof(buf), "%d", v);
        }
        lv_label_set_text(s_widgets[i].label_val, buf);
        lv_label_set_text(s_widgets[i].label_name, ch.name);
        if (s_widgets[i].btn_mute) {
            if (ch.mute) lv_obj_add_state   (s_widgets[i].btn_mute, LV_STATE_CHECKED);
            else         lv_obj_remove_state(s_widgets[i].btn_mute, LV_STATE_CHECKED);
        }
        if (s_widgets[i].slider) {
            int color_idx = app_prefs_get_channel_color(ch.id);
            uint32_t hex = (color_idx >= 0 && color_idx < 8)
                               ? COLOR_PALETTE[color_idx]
                               : DEFAULT_SLIDER_HEX;
            lv_color_t bar_color  = lv_color_hex(hex);
            // Darken the knob ~24% so it reads as a separate piece on top
            // of the filled bar. lvl is 0..255 with 255 being fully black.
            lv_color_t knob_color = lv_color_darken(bar_color, 60);
            lv_obj_set_style_bg_color(s_widgets[i].slider, bar_color,  LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(s_widgets[i].slider, knob_color, LV_PART_KNOB);
        }
        if (s_widgets[i].signal_dot) {
            app_signal_indicator_t mode = app_prefs_get_signal_indicator();
            bool show = (mode != APP_SIGNAL_INDICATOR_NONE) &&
                        !ch.mute && ch.level > 0.01f;
            if (show) {
                lv_obj_set_style_bg_color(s_widgets[i].signal_dot,
                                          lv_color_hex(0x40C040), 0);
                lv_obj_remove_flag(s_widgets[i].signal_dot, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_widgets[i].signal_dot, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void on_state_change(size_t idx, void *ctx)
{
    (void)ctx;
    if (idx >= APP_CONFIG_MAX_CHANNELS) return;
    s_dirty[idx] = true;
    // Fast path: if a sweep is already queued, our dirty flag will be picked
    // up by it. No lock, no allocation, no work.
    if (s_sweep_queued) return;

    if (!lvgl_port_lock(100)) return;
    if (!s_sweep_queued) {
        s_sweep_queued = true;
        if (lv_async_call(apply_pending, NULL) != LV_RESULT_OK) {
            s_sweep_queued = false;
            // Best effort — the next state change retries.
        }
    }
    lvgl_port_unlock();
}

// Pref changes (level format, channel colors, signal indicator, theme) affect
// the rendering of every fader, so we mark all channels dirty and queue a
// single sweep — same plumbing as the per-channel state changes. Theme is
// re-applied here too; lv_theme_default_init is idempotent for an unchanged
// theme so we don't need to track the old value.
static void on_prefs_change(void *ctx)
{
    (void)ctx;
    app_display_apply_theme(app_prefs_get_theme());
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) s_dirty[i] = true;
    if (s_sweep_queued) return;

    if (!lvgl_port_lock(100)) return;
    if (!s_sweep_queued) {
        s_sweep_queued = true;
        if (lv_async_call(apply_pending, NULL) != LV_RESULT_OK) {
            s_sweep_queued = false;
        }
    }
    lvgl_port_unlock();
}

static void style_dot(lv_obj_t *dot, bool active)
{
    lv_color_t fill = active ? lv_color_hex(0xE0E0E0) : lv_color_hex(0x404040);
    lv_obj_set_style_bg_color(dot, fill, 0);
}

static void on_tile_changed(lv_event_t *e)
{
    (void)e;
    lv_obj_t *active = lv_tileview_get_tile_active(s_tileview);
    for (size_t i = 0; i < s_page_count; ++i) {
        style_dot(s_page_dots[i], s_page_tiles[i] == active);
    }
}

static void build_fader(lv_obj_t *parent, size_t idx, int slot_x_in_tile)
{
    app_channel_t ch;
    if (!app_state_get(idx, &ch)) return;

    // Center the box within its slot. slot_w = SCREEN_W / FADERS_PER_PAGE.
    const int slot_w = SCREEN_W / FADERS_PER_PAGE;
    const int box_x  = slot_x_in_tile + (slot_w - FADER_BOX_W) / 2;
    const int box_y  = (TILEVIEW_H - FADER_BOX_H) / 2;

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, FADER_BOX_W, FADER_BOX_H);
    lv_obj_set_pos(box, box_x, box_y);
    lv_obj_set_style_pad_all(box, FADER_BOX_PAD, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(box);
    lv_label_set_text(name, ch.name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, FADER_BOX_W - 2 * FADER_BOX_PAD);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 0);
    s_widgets[idx].label_name = name;

    // Signal-present indicator — small round dot centered below the name
    // label and above the slider. Visible only when mode != none AND the
    // channel is actively passing audio (!mute && level > 0). Without live
    // meter data from MS (offline test instance) this is a "configured
    // to pass audio" indicator, not strict audio presence — useful for
    // the "do I have a cable problem?" question.
    lv_obj_t *dot = lv_obj_create(box);
    lv_obj_set_size(dot, SIGNAL_DOT_SIZE, SIGNAL_DOT_SIZE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    s_widgets[idx].signal_dot = dot;

    lv_obj_t *slider = lv_slider_create(box);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, (int)(ch.level * 100.0f), LV_ANIM_OFF);
    lv_obj_set_size(slider, SLIDER_W, SLIDER_H);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(slider, on_slider_changed, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)idx);
    lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_RELEASED,
                        (void *)(uintptr_t)idx);
    s_widgets[idx].slider = slider;

    lv_obj_t *btn_mute = lv_button_create(box);
    // Note: NOT LV_OBJ_FLAG_CHECKABLE — that auto-toggles on press, which
    // then fires VALUE_CHANGED even if the finger drags off before release.
    // We track checked state ourselves and listen for LV_EVENT_CLICKED,
    // which only fires on press-and-release-on-widget (drag-off cancels).
    lv_obj_set_size(btn_mute, MUTE_BTN_W, MUTE_BTN_H);
    lv_obj_align(btn_mute, LV_ALIGN_BOTTOM_MID, 0, -28);
    // Visible "muted" state — saturated red so a glance distinguishes the
    // silenced channels from the live ones in stage lighting.
    lv_obj_set_style_bg_color(btn_mute, lv_color_hex(0xC00000), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn_mute, lv_color_hex(0x303030), LV_STATE_DEFAULT);
    lv_obj_t *btn_label = lv_label_create(btn_mute);
    lv_label_set_text(btn_label, "MUTE");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn_mute, on_mute_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)idx);
    s_widgets[idx].btn_mute = btn_mute;
    if (ch.mute) lv_obj_add_state(btn_mute, LV_STATE_CHECKED);

    lv_obj_t *val = lv_label_create(box);
    lv_label_set_text(val, "0");
    lv_obj_align(val, LV_ALIGN_BOTTOM_MID, 0, 0);
    s_widgets[idx].label_val = val;
}

static lv_obj_t *create_page_indicator(lv_obj_t *parent, size_t pages)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    int total_w = (int)(pages * DOT_SIZE + (pages - 1) * DOT_GAP);
    lv_obj_set_size(bar, total_w, DOT_SIZE);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    int x = 0;
    for (size_t i = 0; i < pages; ++i) {
        lv_obj_t *dot = lv_obj_create(bar);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_pos(dot, x, 0);
        lv_obj_set_style_radius(dot, DOT_SIZE / 2, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        style_dot(dot, i == 0);
        s_page_dots[i] = dot;
        x += DOT_SIZE + DOT_GAP;
    }
    return bar;
}

void app_ui_init(const ms_client_iface_t *ms)
{
    s_ms = ms;

    // Widget construction must hold the lvgl_port mutex. M1 (3 widgets) got
    // away without it because creation finished within one render tick;
    // M2's tileview + 12 faders + indicator is slow enough that the LVGL
    // render task races in and tries to lay out half-built objects, faulting
    // in get_prop_core on a NULL styles[].style.
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed; UI not built");
        return;
    }

    lv_obj_t *scr = lv_screen_active();

    // Status line at the top. app_wifi / ms_ws push updates here so the user
    // sees boot progress instead of a static screen.
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "Booting...");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 8);

    // Settings gear in the top-right corner of the status bar — opens the
    // touch-driven configuration overlay defined further below.
    lv_obj_t *gear = lv_button_create(scr);
    lv_obj_set_size(gear, 28, 28);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -8, 2);
    lv_obj_set_style_radius(gear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gear, 0, 0);
    lv_obj_t *gear_label = lv_label_create(gear);
    lv_label_set_text(gear_label, LV_SYMBOL_SETTINGS);
    lv_obj_center(gear_label);
    lv_obj_add_event_cb(gear, on_gear_clicked, LV_EVENT_CLICKED, NULL);

    // Tileview holds one tile per page. Horizontal swipe between pages.
    s_tileview = lv_tileview_create(scr);
    lv_obj_set_size(s_tileview, SCREEN_W, TILEVIEW_H);
    lv_obj_set_pos(s_tileview, 0, TILEVIEW_Y);
    lv_obj_set_style_border_width(s_tileview, 0, 0);

    size_t total = app_state_count();
    s_page_count = (total + FADERS_PER_PAGE - 1) / FADERS_PER_PAGE;
    if (s_page_count == 0) s_page_count = 1;
    if (s_page_count > MAX_PAGES) s_page_count = MAX_PAGES;

    for (size_t p = 0; p < s_page_count; ++p) {
        // dir: first page can only swipe right, last page only left, middle
        // pages both. With one page (s_page_count == 1) dir = 0 disables
        // navigation entirely.
        lv_dir_t dir = 0;
        if (p > 0)                    dir |= LV_DIR_LEFT;
        if (p + 1 < s_page_count)     dir |= LV_DIR_RIGHT;

        lv_obj_t *tile = lv_tileview_add_tile(s_tileview, (uint8_t)p, 0, dir);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        // Default lv_obj style has padding + border; zero them so our slot
        // x coordinates land where we expect (relative to the tile's true
        // top-left, not its content area).
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        s_page_tiles[p] = tile;

        const int slot_w = SCREEN_W / FADERS_PER_PAGE;
        for (size_t slot = 0; slot < FADERS_PER_PAGE; ++slot) {
            size_t idx = p * FADERS_PER_PAGE + slot;
            if (idx >= total) break;
            build_fader(tile, idx, (int)slot * slot_w);
        }
    }

    lv_obj_add_event_cb(s_tileview, on_tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Page indicator only when there's more than one page.
    if (s_page_count > 1) {
        create_page_indicator(scr, s_page_count);
    }

    app_state_register_on_change(on_state_change, NULL);
    app_prefs_register_on_change(on_prefs_change, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI mounted: %u faders across %u page(s)",
             (unsigned)total, (unsigned)s_page_count);
}

// ─────────────────────────────────────────────────────────────────────────
// Settings overlay — touch-driven configuration of the prefs that today
// only have console commands (level format, signal indicator, channel
// colors). Built lazily on first gear-icon tap; subsequent opens reuse
// the same overlay and re-sync visible state from app_prefs.
// ─────────────────────────────────────────────────────────────────────────

static void on_gear_clicked(lv_event_t *e)
{
    (void) e;
    settings_open();
}

static void on_close_clicked(lv_event_t *e)
{
    (void) e;
    settings_close();
}

static void update_radio_visuals(lv_obj_t **buttons, size_t count, size_t selected)
{
    for (size_t i = 0; i < count; ++i) {
        if (i == selected) lv_obj_add_state   (buttons[i], LV_STATE_CHECKED);
        else               lv_obj_remove_state(buttons[i], LV_STATE_CHECKED);
    }
}

static void on_lvl_norm_clicked(lv_event_t *e)
{
    (void) e;
    app_prefs_set_level_format(APP_LEVEL_FORMAT_NORM);
    lv_obj_t *btns[2] = { s_lvl_norm_btn, s_lvl_db_btn };
    update_radio_visuals(btns, 2, 0);
}

static void on_lvl_db_clicked(lv_event_t *e)
{
    (void) e;
    app_prefs_set_level_format(APP_LEVEL_FORMAT_DB);
    lv_obj_t *btns[2] = { s_lvl_norm_btn, s_lvl_db_btn };
    update_radio_visuals(btns, 2, 1);
}

static void on_sig_clicked(lv_event_t *e)
{
    int which = (int)(uintptr_t) lv_event_get_user_data(e);
    app_prefs_set_signal_indicator((app_signal_indicator_t) which);
    update_radio_visuals(s_sig_buttons, 3, (size_t) which);
}

static void on_theme_clicked(lv_event_t *e)
{
    int which = (int)(uintptr_t) lv_event_get_user_data(e);
    app_prefs_set_theme((app_theme_t) which);
    update_radio_visuals(s_theme_buttons, 2, (size_t) which);
}

// Tap a swatch → cycle to the next palette index (-1 = no color, 0..7 =
// palette entries). Applies via app_prefs which fires on_prefs_change and
// the dirty sweep, recolouring the slider in real time.
static void on_swatch_clicked(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t) lv_event_get_user_data(e);
    int    cur = -1;
    int    ch_id = app_state_id_for_idx(idx);
    if (ch_id >= 0) cur = app_prefs_get_channel_color(ch_id);
    int next = cur + 1;
    if (next > 7) next = -1;
    if (ch_id >= 0) app_prefs_set_channel_color(ch_id, next);

    // Update the swatch visual itself — apply_pending only repaints the
    // fader sliders, not anything inside the settings overlay.
    lv_obj_t *swatch = (lv_obj_t *) lv_event_get_target(e);
    if (next < 0) {
        lv_obj_set_style_bg_color(swatch, lv_color_hex(0x303030), 0);
    } else {
        lv_obj_set_style_bg_color(swatch, lv_color_hex(COLOR_PALETTE[next]), 0);
    }
}

static lv_obj_t *make_radio_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = lv_button_create(parent);
    // Default (unchecked) is muted; CHECKED uses our accent green so the
    // active option reads at a glance. The dark-theme default styled
    // unchecked and checked the same blue, which made the radios look
    // ambiguous on a screenshot.
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x303744), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x40C060), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xC0C0C0), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, lv_color_hex(0x101010), LV_STATE_CHECKED);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

static void build_settings_overlay(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 16, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    s_settings_overlay = ov;

    // Title bar.
    lv_obj_t *title = lv_label_create(ov);
    lv_label_set_text(title, "Settings");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *close_btn = lv_button_create(ov);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, on_close_clicked, LV_EVENT_CLICKED, NULL);

    // Section: Level Format
    lv_obj_t *lvl_label = lv_label_create(ov);
    lv_label_set_text(lvl_label, "Level Format");
    lv_obj_align(lvl_label, LV_ALIGN_TOP_LEFT, 0, 56);

    s_lvl_norm_btn = make_radio_button(ov, "0..100");
    lv_obj_set_size(s_lvl_norm_btn, 120, 44);
    lv_obj_align(s_lvl_norm_btn, LV_ALIGN_TOP_LEFT, 180, 50);
    lv_obj_add_event_cb(s_lvl_norm_btn, on_lvl_norm_clicked, LV_EVENT_CLICKED, NULL);

    s_lvl_db_btn = make_radio_button(ov, "dB");
    lv_obj_set_size(s_lvl_db_btn, 120, 44);
    lv_obj_align(s_lvl_db_btn, LV_ALIGN_TOP_LEFT, 312, 50);
    lv_obj_add_event_cb(s_lvl_db_btn, on_lvl_db_clicked, LV_EVENT_CLICKED, NULL);

    // Section: Signal Indicator
    lv_obj_t *sig_label = lv_label_create(ov);
    lv_label_set_text(sig_label, "Signal Indicator");
    lv_obj_align(sig_label, LV_ALIGN_TOP_LEFT, 0, 116);

    static const char *sig_text[3] = { "Off", "Signal", "Meter" };
    for (int i = 0; i < 3; ++i) {
        s_sig_buttons[i] = make_radio_button(ov, sig_text[i]);
        lv_obj_set_size(s_sig_buttons[i], 120, 44);
        lv_obj_align(s_sig_buttons[i], LV_ALIGN_TOP_LEFT, 180 + i * 132, 110);
        lv_obj_add_event_cb(s_sig_buttons[i], on_sig_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t) i);
    }

    // Section: Theme — Dark / Light radios. Right of Signal Indicator on the
    // same row to keep the panel vertically compact.
    lv_obj_t *theme_label = lv_label_create(ov);
    lv_label_set_text(theme_label, "Theme");
    lv_obj_align(theme_label, LV_ALIGN_TOP_LEFT, 600, 116);

    static const char *theme_text[2] = { "Dark", "Light" };
    for (int i = 0; i < 2; ++i) {
        s_theme_buttons[i] = make_radio_button(ov, theme_text[i]);
        lv_obj_set_size(s_theme_buttons[i], 120, 44);
        lv_obj_align(s_theme_buttons[i], LV_ALIGN_TOP_LEFT, 700 + i * 132, 110);
        lv_obj_add_event_cb(s_theme_buttons[i], on_theme_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t) i);
    }

    // Section: Channel Colors — 6 rows × 2 columns
    lv_obj_t *col_label = lv_label_create(ov);
    lv_label_set_text(col_label, "Channel Colors");
    lv_obj_align(col_label, LV_ALIGN_TOP_LEFT, 0, 176);

    const int col_w     = 460;
    const int row_h     = 50;
    const int swatch_sz = 32;
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) {
        app_channel_t ch;
        if (!app_state_get(i, &ch)) continue;
        int col = (int)(i / 6);
        int row = (int)(i % 6);
        int x = 8 + col * (col_w + 16);
        int y = 210 + row * row_h;

        lv_obj_t *row_obj = lv_obj_create(ov);
        lv_obj_set_size(row_obj, col_w, row_h - 6);
        lv_obj_set_pos(row_obj, x, y);
        lv_obj_set_style_radius(row_obj, 6, 0);
        lv_obj_set_style_pad_all(row_obj, 8, 0);
        lv_obj_clear_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = lv_label_create(row_obj);
        lv_label_set_text(name, ch.name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *swatch = lv_obj_create(row_obj);
        lv_obj_set_size(swatch, swatch_sz, swatch_sz);
        lv_obj_align(swatch, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(swatch, 6, 0);
        lv_obj_set_style_border_width(swatch, 1, 0);
        lv_obj_set_style_border_color(swatch, lv_color_hex(0x808080), 0);
        lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(swatch, on_swatch_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t) i);
        s_color_swatches[i] = swatch;
    }
}

static void settings_refresh_state(void)
{
    lv_obj_t *lvl_btns[2] = { s_lvl_norm_btn, s_lvl_db_btn };
    update_radio_visuals(lvl_btns, 2,
                         app_prefs_get_level_format() == APP_LEVEL_FORMAT_DB ? 1 : 0);
    update_radio_visuals(s_sig_buttons, 3,
                         (size_t) app_prefs_get_signal_indicator());
    update_radio_visuals(s_theme_buttons, 2,
                         (size_t) app_prefs_get_theme());

    // Refresh swatches from app_prefs.
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) {
        if (!s_color_swatches[i]) continue;
        int ch_id = app_state_id_for_idx(i);
        int color = (ch_id >= 0) ? app_prefs_get_channel_color(ch_id) : -1;
        if (color < 0) {
            lv_obj_set_style_bg_color(s_color_swatches[i], lv_color_hex(0x303030), 0);
        } else {
            lv_obj_set_style_bg_color(s_color_swatches[i],
                                      lv_color_hex(COLOR_PALETTE[color]), 0);
        }
    }
}

static void settings_open(void)
{
    if (!s_settings_overlay) {
        build_settings_overlay();
    }
    settings_refresh_state();
    lv_obj_remove_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings_overlay);
}

static void settings_close(void)
{
    if (s_settings_overlay) {
        lv_obj_add_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}
