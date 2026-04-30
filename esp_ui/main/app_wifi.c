#include "app_wifi.h"
#include "secrets.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "app_wifi";

#define BIT_CONNECTED (1 << 0)
#define BIT_FAILED    (1 << 1)
#define MAX_RETRIES   10

static EventGroupHandle_t s_evt;
static int                s_retry;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < MAX_RETRIES) {
            ++s_retry;
            ESP_LOGW(TAG, "disconnect, retry %d/%d", s_retry, MAX_RETRIES);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_evt, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_evt, BIT_CONNECTED);
    }
}

bool app_wifi_connect(void)
{
    s_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_event, NULL, NULL));

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     APP_WIFI_SSID,     sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, APP_WIFI_PASSWORD, sizeof(cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_CONNECTED | BIT_FAILED,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & BIT_CONNECTED) != 0;
}
