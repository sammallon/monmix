#include "app_storage.h"

#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static const char *TAG          = "app_storage";
static const char  MOUNT_POINT[] = "/sdcard";

static sdmmc_card_t        *s_card       = NULL;
static sd_pwr_ctrl_handle_t s_pwr_handle = NULL;

#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
// In IDF v6+, the SDMMC host controller is initialised exactly once. ESP-Hosted
// (on SDIO slot 1, talking to the C6) does it first; a second sdmmc_host_init()
// call would assert. Replace host.init/deinit with no-ops so the mount path
// only configures slot 0's pins/bus and trusts the already-running peripheral.
static esp_err_t s_sdmmc_host_init_dummy(void)   { return ESP_OK; }
static esp_err_t s_sdmmc_host_deinit_dummy(void) { return ESP_OK; }
#endif

bool app_storage_init(void)
{
    if (s_card != NULL) {
        return true;
    }

    // ESP32-P4 SDMMC slot 0 VDD is gated by on-chip LDO output 4 on this
    // board family — the SD card stays unpowered until the LDO is enabled.
    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = 4 };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sd_pwr_ctrl_new_on_chip_ldo: %s", esp_err_to_name(err));
        return false;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot            = SDMMC_HOST_SLOT_0;
    host.max_freq_khz    = 10000;             // 10 MHz — Elecrow's tested value
    host.pwr_ctrl_handle = s_pwr_handle;
#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
    host.init   = &s_sdmmc_host_init_dummy;
    host.deinit = &s_sdmmc_host_deinit_dummy;
#endif

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width  = 1;                          // CrowPanel only routes D0
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    // P4 SDMMC0 has hardware-fixed pins (CLK=43, CMD=44, D0=39); the GPIO
    // matrix isn't used for this controller, so leave slot.clk/cmd/d0 at
    // their defaults.

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    ESP_LOGI(TAG, "mounting SD at %s (slot 0, 1-bit, %d kHz)",
             MOUNT_POINT, host.max_freq_khz);
    err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s — SD-backed features off this boot",
                 esp_err_to_name(err));
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_handle);
        s_pwr_handle = NULL;
        s_card       = NULL;
        return false;
    }
    sdmmc_card_print_info(stdout, s_card);
    return true;
}

bool app_storage_is_mounted(void)
{
    return s_card != NULL;
}

const char *app_storage_mount_point(void)
{
    return MOUNT_POINT;
}
