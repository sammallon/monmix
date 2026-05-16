#include "app_ui.h"

#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "app_ui";

static lv_obj_t *s_status_label;

void app_ui_init(void)
{
    // Widget construction must hold lvgl_port_lock — otherwise the LVGL
    // render task can iterate through half-built widgets and fault.
    // See esp_ui's reference_lvgl_async_call_lock memory.
    if (lvgl_port_lock(0) != true) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xE6E8EB), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ProPresenter Stage Display");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "Boot...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x9AA0A6), 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 40);

    lvgl_port_unlock();
}

void app_ui_set_status(const char *text)
{
    if (!s_status_label) return;
    const char *s = text ? text : "";
    if (lvgl_port_lock(0) != true) return;
    lv_label_set_text(s_status_label, s);
    lvgl_port_unlock();
}
