#include "app_display.h"

#include "esp_log.h"

static const char *TAG = "app_display";

bool app_display_init(void)
{
    // TODO(unbox): wire up real panel + touch + LVGL via esp_lvgl_port.
    //
    // Sketch of the real implementation:
    //   1. i2c_new_master_bus(...) for the touch bus
    //   2. esp_lcd_new_panel_io_dbi + esp_lcd_new_panel_ili9881c (or jd9365)
    //   3. esp_lcd_touch_new_i2c_gt911(...)
    //   4. lvgl_port_init(...) + lvgl_port_add_disp(...) + lvgl_port_add_touch(...)
    //
    // For now, log and continue so the rest of the firmware can be built and
    // exercised with serial logs while the board is on its way.
    ESP_LOGW(TAG, "display init STUB — panel/touch not yet wired");
    return true;
}
