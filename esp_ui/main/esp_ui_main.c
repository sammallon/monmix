#include "esp_log.h"
#include "nvs_flash.h"

#include "app_display.h"
#include "app_ms_client.h"
#include "app_state.h"
#include "app_ui.h"
#include "app_wifi.h"

static const char *TAG = "esp_ui";

void app_main(void)
{
    ESP_LOGI(TAG, "stage monitor mix controller — boot");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    app_state_init();

    if (!app_wifi_connect()) {
        ESP_LOGE(TAG, "WiFi connect failed; halting");
        return;
    }

    if (!app_display_init()) {
        ESP_LOGE(TAG, "display init failed; halting");
        return;
    }

    const ms_client_iface_t *ms = app_ms_client_ws();
    app_ui_init(ms);

    if (!ms->start()) {
        ESP_LOGE(TAG, "MS client failed to start");
    }
}
