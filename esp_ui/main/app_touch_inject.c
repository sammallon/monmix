#include "app_touch_inject.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "app_touch";

static lv_indev_t *s_indev;

// Single-writer (console / app) → single-reader (LVGL polling task) state.
// The two sides serialize through lvgl_port_lock — every set takes the
// lock, and the read callback runs under LVGL's task which already holds
// it. Plain int writes are atomic on the P4's RISC-V word size, but the
// lock keeps the (x, y, pressed) triple consistent at the boundary.
static struct {
    int32_t x;
    int32_t y;
    bool    pressed;
} s_state;

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void) indev;
    data->point.x = s_state.x;
    data->point.y = s_state.y;
    data->state   = s_state.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void app_touch_inject_init(void)
{
    if (s_indev) return;
    if (!lvgl_port_lock(2000)) {
        ESP_LOGE(TAG, "lvgl_port_lock timeout during init");
        return;
    }
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, read_cb);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "virtual touch indev registered — `touch x y [tap|down|up]`");
}

void app_touch_inject_set(int x, int y, bool pressed)
{
    if (!s_indev) return;
    if (!lvgl_port_lock(100)) return;

    // LVGL expects indev coordinates in the PANEL'S physical frame and
    // applies the display rotation internally to map them to logical
    // (hit-testable) coordinates. The real GT911 driver gives raw panel
    // coords, lvgl_port_add_touch wires it up automatically. Our virtual
    // indev gets to skip that, but we want to take coords in the same
    // frame as `tools/fetch_screenshot.py` produces (logical) — so we
    // do the inverse-rotation here, mirroring what a real driver would
    // hand back to LVGL.
    lv_display_t *disp = lv_display_get_default();
    int32_t lw = lv_display_get_horizontal_resolution(disp);
    int32_t lh = lv_display_get_vertical_resolution(disp);
    int32_t px = x, py = y;
    switch (lv_display_get_rotation(disp)) {
        case LV_DISPLAY_ROTATION_0:   px = x;        py = y;        break;
        case LV_DISPLAY_ROTATION_90:  px = y;        py = lw - x;   break;
        case LV_DISPLAY_ROTATION_180: px = lw - x;   py = lh - y;   break;
        case LV_DISPLAY_ROTATION_270: px = lh - y;   py = x;        break;
    }
    s_state.x       = px;
    s_state.y       = py;
    s_state.pressed = pressed;
    lvgl_port_unlock();
}
