// No-op mock of app_display. Theme/rotation/backlight on the sim are
// driven by SDL window state — pc_main owns the lv_display creation, so
// app_ui's apply hooks just log.
#include "app_display.h"

#include <stdio.h>

bool app_display_init(void) { return true; }

void app_display_apply_theme(app_theme_t theme) {
    fprintf(stdout, "[mock_display] apply_theme=%d\n", (int)theme);
}

void app_display_apply_rotation(app_display_rotation_t rot) {
    fprintf(stdout, "[mock_display] apply_rotation=%d\n", (int)rot);
}

void app_display_set_backlight_pct(uint8_t pct) {
    fprintf(stdout, "[mock_display] backlight=%u%%\n", (unsigned)pct);
}
