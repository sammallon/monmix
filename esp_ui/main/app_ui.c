#include "app_ui.h"
#include "app_logd.h"
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
#define SLIDER_H            420
#define DOT_SIZE            12
#define DOT_GAP             10

typedef struct {
    lv_obj_t *slider;
    lv_obj_t *label_name;
    lv_obj_t *label_val;
} fader_widgets_t;

static fader_widgets_t          s_widgets[APP_CONFIG_MAX_CHANNELS];
static lv_obj_t                *s_status_label;
static lv_obj_t                *s_tileview;
static lv_obj_t                *s_page_tiles[MAX_PAGES];
static lv_obj_t                *s_page_dots[MAX_PAGES];
static size_t                   s_page_count;
static const ms_client_iface_t *s_ms;

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

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", v);
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
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", v);
        lv_label_set_text(s_widgets[i].label_val, buf);
        lv_label_set_text(s_widgets[i].label_name, ch.name);
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
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101010), 0);

    // Status line at the top. app_wifi / ms_ws push updates here so the user
    // sees boot progress instead of a static screen.
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "Booting...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xC0C0C0), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 8);

    // Tileview holds one tile per page. Horizontal swipe between pages.
    s_tileview = lv_tileview_create(scr);
    lv_obj_set_size(s_tileview, SCREEN_W, TILEVIEW_H);
    lv_obj_set_pos(s_tileview, 0, TILEVIEW_Y);
    lv_obj_set_style_bg_color(s_tileview, lv_color_hex(0x101010), 0);
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
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x101010), 0);
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

    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI mounted: %u faders across %u page(s)",
             (unsigned)total, (unsigned)s_page_count);
}
