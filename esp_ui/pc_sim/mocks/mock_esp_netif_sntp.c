// All SNTP entry points used by app_ui.c, no-op'd. The sim leaves the
// host clock alone.
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

int  esp_netif_sntp_init  (const esp_sntp_config_t *cfg) { (void)cfg; return 0; }
int  esp_netif_sntp_deinit(void)                         { return 0; }
int  esp_netif_sntp_start (void)                         { return 0; }

void sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t cb) { (void)cb; }
