// HTTP OTA wrapper. Wraps esp_https_ota (plain-HTTP mode) and emits
// progress lines back to the network console. See app_ota.h.

#include "app_ota.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_netconsole.h"
#include "app_display.h"
#include "app_prefs.h"
#include "app_ui.h"

static const char *TAG = "app_ota";

// Single in-flight guard. The OTA task sets this on start and clears on
// exit; app_ota_start checks-and-sets it atomically.
static atomic_bool s_in_progress = ATOMIC_VAR_INIT(false);

bool app_ota_in_progress(void)
{
    return atomic_load(&s_in_progress);
}

// ─────────────────────────────────────────────────────────────────────────
// OTA worker
// ─────────────────────────────────────────────────────────────────────────

typedef struct {
    char url[256];
} ota_args_t;

#define OTA_PROGRESS_THROTTLE_MS  500    // limit progress spam

// Mitigation for the OTA-time flicker: under flash-write + TCP load
// the LVGL task is starved enough that the DSI panel briefly scans
// stale/uninitialised framebuffer regions, showing bright artifacts.
// Dimming the backlight while the download is in flight makes the
// artifacts much less visible without hiding the progress overlay.
// 15 % is the smallest value where the LVGL widgets are still legible.
#define OTA_BACKLIGHT_PCT  15

static void ota_task(void *arg)
{
    ota_args_t *args = (ota_args_t *) arg;
    uint8_t saved_brightness = app_prefs_get_brightness_pct();

    esp_http_client_config_t http_cfg = {
        .url               = args->url,
        .timeout_ms        = 10000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };
    esp_https_ota_handle_t handle = NULL;

    app_netconsole_send_line("OTA_BEGIN url=%s", args->url);
    app_ui_ota_show();
    // Dim AFTER the overlay is up so the user sees the transition.
    app_display_set_backlight_pct(OTA_BACKLIGHT_PCT);
    ESP_LOGI(TAG, "begin url=%s (backlight dimmed to %d%%)", args->url, OTA_BACKLIGHT_PCT);

    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK || handle == NULL) {
        app_netconsole_send_line("OTA_ERROR begin %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "begin failed: %s", esp_err_to_name(err));
        goto done;
    }

    int total = esp_https_ota_get_image_size(handle);
    if (total > 0) {
        app_netconsole_send_line("OTA_SIZE bytes=%d", total);
        ESP_LOGI(TAG, "image size: %d", total);
    }

    int  last_done = 0;
    int64_t last_progress_us = esp_timer_get_time();

    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        int done = esp_https_ota_get_image_len_read(handle);
        int64_t now = esp_timer_get_time();
        if (done != last_done &&
            (now - last_progress_us) >= (OTA_PROGRESS_THROTTLE_MS * 1000)) {
            app_netconsole_send_line("OTA_PROGRESS done=%d total=%d", done, total);
            app_ui_ota_update(done, total);
            last_done        = done;
            last_progress_us = now;
        }
        // No sleep: esp_https_ota_perform itself blocks on TCP reads
        // (chunked HTTP body), so this loop self-paces.
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        app_netconsole_send_line("OTA_ERROR perform %s", esp_err_to_name(err));
        app_ui_ota_done(false, esp_err_to_name(err));
        app_display_set_backlight_pct(saved_brightness);   // restore on failure
        ESP_LOGE(TAG, "perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        goto done;
    }

    err = esp_https_ota_finish(handle);
    handle = NULL;   // finish always consumes the handle
    if (err != ESP_OK) {
        app_netconsole_send_line("OTA_ERROR finish %s", esp_err_to_name(err));
        app_ui_ota_done(false, esp_err_to_name(err));
        app_display_set_backlight_pct(saved_brightness);
        ESP_LOGE(TAG, "finish failed: %s", esp_err_to_name(err));
        goto done;
    }

    app_netconsole_send_line("OTA_FINISH ok rebooting");
    app_ui_ota_done(true, "Update complete, restarting...");
    // Restore on success too -- the user sees a clean "restart" message
    // at full brightness for ~800 ms before the reboot blanks the panel.
    app_display_set_backlight_pct(saved_brightness);
    ESP_LOGI(TAG, "OTA finished; rebooting");
    // Give the client time to see the line + the LVGL overlay to redraw.
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    /* not reached */

done:
    if (handle) {
        esp_https_ota_abort(handle);
        // begin/perform failed before we could call app_ui_ota_done;
        // emit a generic failure marker so the overlay dismisses.
        app_ui_ota_done(false, "Update failed (begin/abort)");
        app_display_set_backlight_pct(saved_brightness);
    }
    free(args);
    atomic_store(&s_in_progress, false);
    vTaskDelete(NULL);
}

bool app_ota_start(const char *url)
{
    if (!url || *url == '\0') return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_in_progress, &expected, true)) {
        return false;
    }
    ota_args_t *args = (ota_args_t *) calloc(1, sizeof(*args));
    if (!args) {
        atomic_store(&s_in_progress, false);
        return false;
    }
    strncpy(args->url, url, sizeof(args->url) - 1);

    // 8 KB stack: esp_https_ota nests a TLS-capable HTTP client (mbedtls
    // sits on the stack during handshake); even without TLS, the request
    // path needs several KB headroom.
    if (xTaskCreate(ota_task, "ota", 8192, args, 5, NULL) != pdPASS) {
        free(args);
        atomic_store(&s_in_progress, false);
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Rollback validation
// ─────────────────────────────────────────────────────────────────────────

void app_ota_mark_running_valid(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (!p) return;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(p, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        // Already-valid factory or previously-validated OTA. No-op.
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "running image marked valid (rollback cancelled)");
    } else {
        ESP_LOGW(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
    }
}
