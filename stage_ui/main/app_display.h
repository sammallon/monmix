// Display apply iface — theme, rotation, backlight. Implementation
// lives in pc_sim/mocks/mock_app_display.c for the sim; hardware-port
// round replaces it with the real ESP32-P4 / EK79007 / GT911 wiring
// adapted from esp_ui/main/app_display.c.

#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "app_prefs.h"

bool app_display_init(void);

void app_display_apply_theme   (app_theme_t theme);
void app_display_apply_rotation(app_display_rotation_t rot);
void app_display_set_backlight_pct(uint8_t pct);
void app_display_set_backlight_off(void);

#endif // APP_DISPLAY_H
