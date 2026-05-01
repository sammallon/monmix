#include "esp_log.h"
#include "nvs_flash.h"

#include "app_config.h"
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

    app_config_init();
    size_t      ch_count = 0;
    const int  *ch_ids   = app_config_channel_ids(&ch_count);
    app_state_init(ch_ids, ch_count);

    // Display + UI come up first — the screen should light up immediately,
    // not wait on WiFi/MS. Status line shows progress thereafter.
    if (!app_display_init()) {
        ESP_LOGE(TAG, "display init failed; halting");
        return;
    }

    const ms_client_iface_t *ms = app_ms_client_ws();
    app_ui_init(ms);
    app_ui_set_status("Connecting WiFi...");

    bool wifi_ok = app_wifi_connect();
    if (!wifi_ok) {
        ESP_LOGW(TAG, "WiFi unavailable; UI will still render, MS client will retry");
        app_ui_set_status("WiFi unavailable — UI only");
    }

    // ws_start always returns true when the client object initializes; the
    // websocket subsystem itself handles reconnect once WiFi is up.
    ms->start();
}
