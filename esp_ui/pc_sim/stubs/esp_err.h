// Subset of esp_err.h. The NVS shim already exports esp_err_t / ESP_OK
// / ESP_FAIL, so this is effectively a re-export for TUs that
// #include "esp_err.h" directly (app_prefs.c does).
#pragma once

#include "nvs.h"  // pulls in esp_err_t and the ESP_OK/FAIL constants

// app_prefs.c logs nvs errors via esp_err_to_name. Cheap stub: integer
// printf into a thread-local buffer. Faithful enough for diagnostics.
static __inline const char *esp_err_to_name(esp_err_t code) {
    static __declspec(thread) char buf[32];
    switch (code) {
        case ESP_OK:                snprintf(buf, sizeof(buf), "ESP_OK");                break;
        case ESP_ERR_NVS_NOT_FOUND: snprintf(buf, sizeof(buf), "ESP_ERR_NVS_NOT_FOUND"); break;
        default:                    snprintf(buf, sizeof(buf), "ESP_ERR(0x%x)", code);   break;
    }
    return buf;
}
