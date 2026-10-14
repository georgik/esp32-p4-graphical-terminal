/**
 * @file board_init.c
 * @brief Board-specific initialization for the ESP32-P4 graphical terminal
 *
 * Only the ESP32-P4-Function-EV board is supported. The P4-Function-EV display
 * is wired through the ESP-BSP (noglib) layer; any other board must implement
 * board_init_display() in the #else fallback below.
 */

#include "board_init.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_raylib_port.h"

static const char *TAG = "board_init";

#ifdef CONFIG_BOARD_ESP32_P4_FUNCTION_EV

#include "bsp/esp-bsp.h"

esp_err_t board_init_display(void)
{
    ESP_LOGI(TAG, "Initializing ESP32-P4-Function-EV display via BSP...");

    // Initialize BSP display (noglib version - DSI interface)
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_io_handle_t io_handle = NULL;

    // P4 requires proper DSI bus configuration
    const bsp_display_config_t bsp_disp_cfg = {
        .hdmi_resolution = 0,  // BSP_HDMI_RES_NONE - Use LCD, not HDMI
        .dsi_bus = {
            .phy_clk_src = 0,  // MIPI_DSI_PHY_CLK_SRC_DEFAULT
            .lane_bit_rate_mbps = 1000,  // BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS default
        },
    };

    esp_err_t ret = bsp_display_new(&bsp_disp_cfg, &panel_handle, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP display: %d", ret);
        return ret;
    }

    if (!panel_handle) {
        ESP_LOGE(TAG, "Failed to initialize BSP display");
        return ESP_FAIL;
    }

    // Initialize backlight control
    ret = bsp_display_brightness_init();
    if (ret == ESP_OK) {
        ret = bsp_display_backlight_on();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to turn on backlight: %d", ret);
            // Don't fail initialization if backlight fails
        }
    } else {
        ESP_LOGW(TAG, "Backlight initialization failed: %d", ret);
        // Don't fail initialization if backlight init fails
    }

    // Get display dimensions from BSP
    // P4 typically has 1024x600 or 1280x800
    uint16_t width = 1024;  // Default, adjust based on CONFIG
    uint16_t height = 600;

    #ifdef CONFIG_BSP_LCD_TYPE_1280_800
    width = 1280;
    height = 800;
    #endif

    // Initialize raylib port layer
    ray_port_cfg_t port_cfg = {
        .buff_psram = true,
        .double_buffer = false,
        .buffer_pixels = 0,
        .chunk_bytes = 0,  // DSI doesn't need chunking
        .swap_rgb_bytes = false,  // DSI panels don't need byte swap
        .hres = width,
        .vres = height,
        .rotation = 0,
        .perf_stats = true,
    };

    ESP_ERROR_CHECK(ray_port_init(&port_cfg));

    ray_port_display_cfg_t disp_cfg = {
        .panel = panel_handle,
        .io = io_handle,
        .hres = width,
        .vres = height,
        .monochrome = false,
        .dma_capable = true,
    };

    ESP_ERROR_CHECK(ray_port_add_display(&disp_cfg));

    ESP_LOGI(TAG, "Display initialized: %dx%d", width, height);
    return ESP_OK;
}

#else

// Fallback: custom board - implement board_init_display() manually
esp_err_t board_init_display(void)
{
    ESP_LOGE(TAG, "Unsupported board! The ESP32-P4 graphical terminal targets the");
    ESP_LOGE(TAG, "ESP32-P4-Function-EV board. Set CONFIG_BOARD_ESP32_P4_FUNCTION_EV,");
    ESP_LOGE(TAG, "or implement board_init_display() here for custom hardware.");
    ESP_LOGE(TAG, "Example for custom hardware:");
    ESP_LOGE(TAG, "  - Create SPI bus and esp_lcd_panel_handle_t");
    ESP_LOGE(TAG, "  - Call ray_port_init() with your config");
    ESP_LOGE(TAG, "  - Call ray_port_add_display() with panel handle");
    return ESP_FAIL;
}

#endif
