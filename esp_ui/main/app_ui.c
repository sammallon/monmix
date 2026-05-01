#include "app_ui.h"
#include "app_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "app_ui";

typedef struct {
    lv_obj_t *slider;
    lv_obj_t *label_name;
    lv_obj_t *label_val;
} fader_widgets_t;

static fader_widgets_t        s_widgets[APP_STATE_MAX_CHANNELS];
static lv_obj_t              *s_status_label;
static const ms_client_iface_t *s_ms;

// Rate-limit outbound SETs per channel so a fast drag doesn't flood MS
// (each SET produces a broadcast echo, doubling on-wire traffic). 50 ms
// = 20 Hz feels live to the user but keeps the websocket task from
// monopolizing its core.
#define SET_MIN_INTERVAL_MS 50
static uint32_t s_last_send_ms[APP_STATE_MAX_CHANNELS];

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
    if (lv_async_call(apply_status, copy) != LV_RESULT_OK) {
        // LVGL not yet running — drop the update silently. The next call
        // after lvgl_port_init will succeed.
        free(copy);
    }
}

static void send_level_now(size_t idx, float level)
{
    int ch_id = app_state_id_for_idx(idx);
    if (s_ms && ch_id >= 0) {
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

typedef struct {
    size_t idx;
    float  level;
    char   name[32];
} snapshot_t;

static void apply_remote_update(void *arg)
{
    snapshot_t *s = (snapshot_t *)arg;
    int v = (int)(s->level * 100.0f);
    // LV_ANIM_OFF: network echoes can arrive every ~10ms during a drag;
    // queueing/cancelling 200ms animations on each one trashes LVGL.
    lv_slider_set_value(s_widgets[s->idx].slider, v, LV_ANIM_OFF);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(s_widgets[s->idx].label_val, buf);
    lv_label_set_text(s_widgets[s->idx].label_name, s->name);
    free(s);
}

static void on_state_change(size_t idx, void *ctx)
{
    (void)ctx;
    app_channel_t ch;
    if (!app_state_get(idx, &ch)) return;
    snapshot_t *s = malloc(sizeof(snapshot_t));
    if (!s) return;
    s->idx   = idx;
    s->level = ch.level;
    strncpy(s->name, ch.name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    if (lv_async_call(apply_remote_update, s) != LV_RESULT_OK) {
        // Async queue full — drop this update rather than leaking.
        free(s);
    }
}

static void build_fader(lv_obj_t *parent, size_t idx, int x, int y)
{
    app_channel_t ch;
    app_state_get(idx, &ch);

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 160, 520);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(box);
    lv_label_set_text(name, ch.name);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 0);
    s_widgets[idx].label_name = name;

    lv_obj_t *slider = lv_slider_create(box);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, (int)(ch.level * 100.0f), LV_ANIM_OFF);
    lv_obj_set_size(slider, 40, 380);
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

void app_ui_init(const ms_client_iface_t *ms)
{
    s_ms = ms;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101010), 0);

    // Status line at the very top. app_wifi / ms_ws push updates here so
    // the user sees boot progress instead of a static screen.
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "Booting...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xC0C0C0), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 8);

    // 10.1" panel is 1024x600. Lay out 3 faders horizontally below the
    // status line.
    int spacing = 1024 / APP_STATE_MAX_CHANNELS;
    for (size_t i = 0; i < APP_STATE_MAX_CHANNELS; ++i) {
        int x = (int)i * spacing + (spacing - 160) / 2;
        build_fader(scr, i, x, 40);
    }

    app_state_register_on_change(on_state_change, NULL);
    ESP_LOGI(TAG, "UI mounted: %d faders", (int)APP_STATE_MAX_CHANNELS);
}
