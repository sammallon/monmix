// Subset of nvs.h sufficient for main/app_prefs.c. The implementation
// (mocks/mock_nvs.c) is file-backed: every commit serializes to
// pc_sim_state/nvs.json so prefs survive sim relaunches. Same shape as
// IDF NVS so app_prefs.c compiles unchanged.
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef int      esp_err_t;
typedef uint32_t nvs_handle_t;

#ifndef ESP_OK
#define ESP_OK                0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL              -1
#endif
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#endif
#ifndef ESP_ERR_NVS_INVALID_LENGTH
#define ESP_ERR_NVS_INVALID_LENGTH 0x110A
#endif

typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
void      nvs_close(nvs_handle_t handle);

esp_err_t nvs_set_u8 (nvs_handle_t h, const char *key, uint8_t  value);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t value);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value);
esp_err_t nvs_set_u64(nvs_handle_t h, const char *key, uint64_t value);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t length);

esp_err_t nvs_get_u8 (nvs_handle_t h, const char *key, uint8_t  *out_value);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out_value);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out_value);
esp_err_t nvs_get_u64(nvs_handle_t h, const char *key, uint64_t *out_value);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out_value, size_t *length);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out_value, size_t *length);

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
