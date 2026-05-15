// Three settings overlays (General / WiFi / PP). All built on the
// overlay framework in app_pp_ui_overlay.

#ifndef APP_PP_UI_SETTINGS_H
#define APP_PP_UI_SETTINGS_H

#include "app_display.h"
#include "app_pp_client.h"

// Wire dependencies once. Called from app_pp_ui_init.
void app_pp_ui_settings_init(const pp_client_iface_t *pp);

// Each opens its modal. The 3 header icon buttons call these.
void app_pp_ui_settings_open_general(void);
void app_pp_ui_settings_open_wifi(void);
void app_pp_ui_settings_open_pp(void);

#endif // APP_PP_UI_SETTINGS_H
