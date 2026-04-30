#include "app_ui.h"
#include "app_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "app_ui";

typedef struct {
    lv_obj_t *slider;
    lv_obj_t *label_name;
    lv_obj_t *label_val;
} fader_widgets_t;

static fader_widgets_t        s_widgets[APP_STATE_MAX_CHANNELS];
static const ms_client_iface_t *s_ms;

static void on_slider_changed(lv_event_t *e)
{
    size_t    idx    = (size_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int       v      = lv_slider_get_value(slider);
    float     level  = (float)v / 100.0f;

    // Update local state without notifying — UI already reflects this value.
    app_state_set_level(idx, level, false);

    int ch_id = app_state_id_for_idx(idx);
    if (s_ms && ch_id >= 0) {
        s_ms->set_level(ch_id, level);
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(s_widgets[idx].label_val, buf);
}

typedef struct { size_t idx; float level; } update_t;

static void apply_remote_update(void *arg)
{
    update_t *u = (update_t *)arg;
    int v = (int)(u->level * 100.0f);
    lv_slider_set_value(s_widgets[u->idx].slider, v, LV_ANIM_ON);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(s_widgets[u->idx].label_val, buf);
    free(u);
}

static void on_state_change(size_t idx, void *ctx)
{
    (void)ctx;
    app_channel_t ch;
    if (!app_state_get(idx, &ch)) return;
    update_t *u = malloc(sizeof(update_t));
    if (!u) return;
    u->idx   = idx;
    u->level = ch.level;
    lv_async_call(apply_remote_update, u);
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

    // 10.1" panel is 1024x600. Lay out 3 faders horizontally.
    int spacing = 1024 / APP_STATE_MAX_CHANNELS;
    for (size_t i = 0; i < APP_STATE_MAX_CHANNELS; ++i) {
        int x = (int)i * spacing + (spacing - 160) / 2;
        build_fader(scr, i, x, 40);
    }

    app_state_register_on_change(on_state_change, NULL);
    ESP_LOGI(TAG, "UI mounted: %d faders", (int)APP_STATE_MAX_CHANNELS);
}
