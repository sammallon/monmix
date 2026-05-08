// Mock app_display. Theme is applied through the actual LVGL theme API
// (matches what the tablet does in main/app_display.c) so the sim
// renders with the same colour scheme the tablet would. Rotation +
// backlight are still no-ops -- SDL has no analogue.
#include "app_display.h"
#include "esp_lvgl_port.h"

#include "lvgl.h"

#include <stdio.h>

bool app_display_init(void) { return true; }

void app_display_apply_theme(app_theme_t theme) {
    lv_display_t *disp = lv_display_get_default();
    if (!disp) return;
    if (!lvgl_port_lock(1000)) return;
    bool dark = (theme == APP_THEME_DARK);
    lv_theme_t *t = lv_theme_default_init(
        disp,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_GREY),
        dark,
        LV_FONT_DEFAULT);
    lv_display_set_theme(disp, t);
    lv_obj_set_style_bg_color(lv_screen_active(),
                              lv_color_hex(dark ? 0x101010 : 0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    lvgl_port_unlock();
    fprintf(stdout, "[mock_display] theme=%s\n", dark ? "dark" : "light");
}

void app_display_apply_rotation(app_display_rotation_t rot) {
    fprintf(stdout, "[mock_display] apply_rotation=%d\n", (int)rot);
}

void app_display_set_backlight_pct(uint8_t pct) {
    fprintf(stdout, "[mock_display] backlight=%u%%\n", (unsigned)pct);
}

void app_display_set_backlight_off(void) {
    fprintf(stdout, "[mock_display] backlight=off\n");
}
