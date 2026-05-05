#include "app_display.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"

// Pin map and panel timings for the CrowPanel Advanced 10.1" (Elecrow SKU
// DHE04310D). Values cross-checked against
// elecrow-ref/example/V1.0/Arduino_Code/Lesson07-Turn_on_the_screen/board_config.h
#define LCD_RST_GPIO          41
#define LCD_BL_GPIO           31
#define LCD_BL_PWM_HZ         30000
#define LCD_BL_DUTY_RES       LEDC_TIMER_10_BIT
// Backlight starts at 0 so the user never sees the uninitialised PSRAM
// framebuffer (which renders as a bright blue band on this panel).
// esp_ui_main ramps the backlight up to the saved pref value AFTER
// app_ui_init has built the splash and LVGL has flushed at least one
// dark frame, so the first thing the user sees is the splash.
#define LCD_BL_INITIAL_PCT    0

#define TOUCH_RST_GPIO        40
#define TOUCH_INT_GPIO        42
#define TOUCH_I2C_SCL_GPIO    46
#define TOUCH_I2C_SDA_GPIO    45
#define TOUCH_I2C_PORT        I2C_NUM_0
#define TOUCH_I2C_CLK_HZ      400000

#define LCD_H_RES             1024
#define LCD_V_RES             600
#define LCD_BITS_PER_PIXEL    16  // RGB565, matches CONFIG_LV_COLOR_DEPTH_16

// Lane rate per Elecrow Lesson07 (1000 Mbps); the EK79007 component's default
// macro picks 900, so we override after the initializer expands.
#define LCD_DSI_LANE_NUM           2
#define LCD_DSI_LANE_RATE_MBPS     1000

// DPI pixel clock per Elecrow's tuned timings (51 MHz). The EK79007 default
// macro is close (52 MHz) but Lesson07 settled on 51 for this exact panel.
#define LCD_DPI_CLK_MHZ            51

// On ESP32-P4 the MIPI DSI PHY rail is driven by the on-chip LDO controller
// even when the board provides external power. Channel 3 at 2.5V is the
// standard config for the DSI PHY (matches IDF v6.0.1 test config and the
// ESP32-P4-Function-EV-Board BSP).
#define LCD_MIPI_DSI_PHY_LDO_CHAN  3
#define LCD_MIPI_DSI_PHY_LDO_MV    2500

static const char *TAG = "app_display";

static esp_ldo_channel_handle_t s_phy_pwr_chan;

// Reboot kills the DSI clock immediately, but the EK79007 keeps the
// backlight rail driven for the brief window before the bootloader
// reconfigures it. With no DSI signal the panel falls back to its
// "no input" test pattern -- a bright pale-blue band -- and the user
// sees that flash on every reboot. esp_register_shutdown_handler runs
// synchronously on esp_restart() AND on panic, so we use it to drive
// the backlight pin LOW before the controller loses signal.
//
// ledc_stop with idle-level 0 pins the GPIO low and stops the PWM. The
// pin stays low through bootloader (until init_backlight reconfigures
// it on the next app boot, where it starts at 0% per W6.4 above).
static void on_app_shutdown(void)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    gpio_set_direction(LCD_BL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL_GPIO, 0);
}

static esp_err_t init_backlight(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LCD_BL_DUTY_RES,
        .freq_hz         = LCD_BL_PWM_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc timer");

    const uint32_t max_duty = (1u << LCD_BL_DUTY_RES) - 1u;
    ledc_channel_config_t ch_cfg = {
        .gpio_num   = LCD_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = max_duty * LCD_BL_INITIAL_PCT / 100u,
        .hpoint     = 0,
    };
    return ledc_channel_config(&ch_cfg);
}

static esp_err_t init_i2c_bus(i2c_master_bus_handle_t *out)
{
    i2c_master_bus_config_t cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = TOUCH_I2C_PORT,
        .scl_io_num                   = TOUCH_I2C_SCL_GPIO,
        .sda_io_num                   = TOUCH_I2C_SDA_GPIO,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, out);
}

static esp_err_t init_dsi_phy_power(void)
{
    esp_ldo_channel_config_t cfg = {
        .chan_id    = LCD_MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = LCD_MIPI_DSI_PHY_LDO_MV,
    };
    return esp_ldo_acquire_channel(&cfg, &s_phy_pwr_chan);
}

static esp_err_t init_panel(esp_lcd_panel_io_handle_t *io_out,
                            esp_lcd_panel_handle_t   *panel_out)
{
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
    bus_cfg.lane_bit_rate_mbps       = LCD_DSI_LANE_RATE_MBPS;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "dsi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t   dbi_cfg = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io),
                        TAG, "dbi io");

    // Hand-built dpi config — the EK79007 1.0.4 macro references fields
    // (pixel_format, flags.use_dma2d) that don't exist on IDF v6.0.1's
    // esp_lcd_dpi_panel_config_t. Timings come from Elecrow's Lesson07
    // tuning (board_config.h) for this SKU.
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLK_MHZ,
        .in_color_format    = LCD_COLOR_FMT_RGB565,
        .out_color_format   = LCD_COLOR_FMT_RGB565,
        // 1 frame buffer — matches Elecrow's factory BSP default. LVGL
        // allocates its own rendering buffers (in PSRAM via buff_spiram).
        // Bumping to 2 + full_refresh + avoid_tearing is M3 polish.
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = LCD_H_RES,
            .v_size            = LCD_V_RES,
            .hsync_pulse_width = 70,
            .hsync_back_porch  = 160,
            .hsync_front_porch = 160,
            .vsync_pulse_width = 10,
            .vsync_back_porch  = 23,
            .vsync_front_porch = 12,
        },
    };

    ek79007_vendor_config_t vendor = {
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = LCD_DSI_LANE_NUM,
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config  = &vendor,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(io, &panel_cfg, &panel),
                        TAG, "ek79007 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel),  TAG, "panel init");
    // Note: EK79007's init sequence already enables display output; the
    // explicit disp_on_off command isn't supported (returns ESP_ERR_NOT_SUPPORTED).

    // Note: esp_lcd_panel_mirror returns OK on EK79007 but is a no-op (the
    // driver tracks the bit but doesn't reprogram the scan direction).
    // Rotation is therefore done as an LVGL transform on the root object;
    // see app_ui.c.

    *io_out    = io;
    *panel_out = panel;
    return ESP_OK;
}

static esp_err_t init_touch(i2c_master_bus_handle_t i2c_bus,
                            esp_lcd_touch_handle_t *out)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &tp_io),
                        TAG, "tp io");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = LCD_H_RES,
        .y_max        = LCD_V_RES,
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags  = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    return esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, out);
}

bool app_display_init(void)
{
    ESP_LOGI(TAG, "init: backlight");
    if (init_backlight() != ESP_OK) {
        ESP_LOGE(TAG, "backlight init failed");
        return false;
    }
    // Kill the backlight on esp_restart / panic so the user doesn't see the
    // panel's no-DSI-signal test pattern (bright blue) during the reboot
    // window. See on_app_shutdown.
    esp_register_shutdown_handler(on_app_shutdown);

    ESP_LOGI(TAG, "init: i2c bus");
    i2c_master_bus_handle_t i2c_bus = NULL;
    if (init_i2c_bus(&i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed");
        return false;
    }

    ESP_LOGI(TAG, "init: dsi phy ldo (chan %d @ %dmV)",
             LCD_MIPI_DSI_PHY_LDO_CHAN, LCD_MIPI_DSI_PHY_LDO_MV);
    if (init_dsi_phy_power() != ESP_OK) {
        ESP_LOGE(TAG, "dsi phy ldo failed");
        return false;
    }

    ESP_LOGI(TAG, "init: panel (EK79007)");
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t   panel = NULL;
    if (init_panel(&io, &panel) != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed");
        return false;
    }

    ESP_LOGI(TAG, "init: touch (GT911)");
    esp_lcd_touch_handle_t tp = NULL;
    if (init_touch(i2c_bus, &tp) != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed");
        return false;
    }

    ESP_LOGI(TAG, "init: lvgl port");
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // Pin LVGL to CPU 0. esp_websocket_client's task tends to land on CPU 1
    // and under heavy traffic monopolizes that core; sharing it with LVGL
    // starves the UI. main lives on CPU 0 but is mostly blocked, so LVGL
    // gets clean slack there.
    lvgl_cfg.task_affinity = 0;
    if (lvgl_port_init(&lvgl_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl port init failed");
        return false;
    }

    // LVGL renders into full-frame buffers in PSRAM (we have 32 MB; each
    // 1024×600×2 buffer is ~1.2 MB so 2.4 MB total — trivial). Full-frame
    // buffers are required so lv_display_set_rotation(180°) has somewhere
    // to do the pixel flip during flush; partial buffers crash the renderer.
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .rotation      = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags         = {
            .buff_dma    = false,
            .buff_spiram = true,
            .sw_rotate   = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = { .avoid_tearing = false },
    };
    lv_disp_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed");
        return false;
    }

    // Kill the LVGL pale-blue default screen bg before the first flush. There
    // is a long visible gap between display power-on and the first widget
    // being mounted (WiFi associate, MS info fetch, channel enumeration);
    // without this the user stares at a bright pale-blue rectangle through
    // the whole boot. The splash logo asset is RGB565A8 with a transparent
    // canvas (build_splash_logo.py alpha-keys pure-black pixels), so the
    // screen bg shows through everywhere outside the logo paths -- no
    // quantisation seam, no halo. Both light and dark default themes leave
    // the active screen's bg alone -- they style children only.
    if (lvgl_port_lock(0)) {
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x101010), 0);
        lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
        lvgl_port_unlock();
    }

    // Rotation is user-configurable (0 or 180). LVGL applies the inverse
    // transform to touch coordinates automatically when an indev shares the
    // display, so registering the GT911 below picks the rotation up for free
    // (see lv_indev.c indev_pointer_proc). sw_rotate + full-frame buffers
    // are required for the framebuffer flip during flush; partial buffers
    // crash the renderer.
    app_display_apply_rotation(app_prefs_get_display_rotation());

    app_display_apply_theme(app_prefs_get_theme());

    const lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
    if (!lvgl_port_add_touch(&touch_cfg)) {
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        return false;
    }

    ESP_LOGI(TAG, "display + touch up @ %dx%d", LCD_H_RES, LCD_V_RES);
    return true;
}

void app_display_apply_rotation(app_display_rotation_t rot)
{
    lv_display_t *disp = lv_display_get_default();
    if (!disp) return;
    lv_display_rotation_t lv_rot = (rot == APP_DISPLAY_ROTATION_180)
                                       ? LV_DISPLAY_ROTATION_180
                                       : LV_DISPLAY_ROTATION_0;
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "apply_rotation: lvgl_port_lock timeout");
        return;
    }
    lv_display_set_rotation(disp, lv_rot);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "rotation applied: %u deg", (unsigned) rot);
}

void app_display_set_backlight_pct(uint8_t pct)
{
    if (pct < 5)   pct = 5;       // floor enforced here too: API-level guard
    if (pct > 100) pct = 100;
    const uint32_t max_duty = (1u << LCD_BL_DUTY_RES) - 1u;
    uint32_t duty = max_duty * (uint32_t) pct / 100u;
    ledc_set_duty   (LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void app_display_apply_theme(app_theme_t theme)
{
    lv_display_t *disp = lv_display_get_default();
    if (!disp) return;
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "apply_theme: lvgl_port_lock timeout");
        return;
    }
    // Dark mode keeps the original low-light stage palette: near-black
    // backgrounds, dark-grey boxes, blue primary so the slider track reads
    // at a glance. Light mode is the LVGL default — white surfaces with
    // the same blue accent.
    lv_theme_t *t = lv_theme_default_init(
        disp,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_GREY),
        /* dark */ theme == APP_THEME_DARK,
        LV_FONT_DEFAULT);
    lv_display_set_theme(disp, t);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "theme applied: %s", theme == APP_THEME_DARK ? "dark" : "light");
}
