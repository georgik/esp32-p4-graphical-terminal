/**
 * @file board_init.c
 * @brief Board-specific initialization for the ESP32-P4 graphical terminal
 *
 * Only the ESP32-P4-Function-EV board is supported. The P4-Function-EV display
 * is wired through the ESP-BSP (noglib) layer (DSI interface).
 *
 * The raylib software renderer is driven directly through the display
 * callbacks registered with raylib_esp_set_display_callbacks(): the application
 * creates the esp_lcd panel and provides a flush callback that pushes the
 * framebuffer to the panel, plus a callback reporting the display dimensions.
 */

#include "board_init.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include <stdint.h>

static const char *TAG = "board_init";

// Dimensions reported to raylib (filled in by board_init_display, read by the app).
static uint16_t g_disp_width = 1024;
static uint16_t g_disp_height = 600;

// Panel handle used by the flush callback.
static esp_lcd_panel_handle_t g_panel = NULL;

// Externally-defined entry point from the raylib ESP-IDF platform backend.
extern void raylib_esp_set_display_callbacks(
    void (*flush_fn)(const uint16_t *buf, uint16_t x, uint16_t y, uint16_t w, uint16_t h),
    void (*get_dim_fn)(uint16_t *w, uint16_t *h)
);

// Flush the RGB565 framebuffer to the display. The ESP32-P4-Function-EV uses a
// DSI panel, so the whole framebuffer can be sent in one bitmap transfer with
// no per-row chunking and no byte swapping.
static void display_flush(const uint16_t *buf, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (buf == NULL || g_panel == NULL) {
        return;
    }

    esp_err_t ret = esp_lcd_panel_draw_bitmap(g_panel, x, y, x + w, y + h, buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to flush framebuffer to display: %s", esp_err_to_name(ret));
    }
}

static void display_get_dimensions(uint16_t *w, uint16_t *h)
{
    if (w) *w = g_disp_width;
    if (h) *h = g_disp_height;
}

void board_init_display_dimensions(uint16_t *width, uint16_t *height)
{
    if (width) *width = g_disp_width;
    if (height) *height = g_disp_height;
}

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
    g_panel = panel_handle;

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

    // Display dimensions come straight from the BSP definitions, which already
    // honor the CONFIG_BSP_LCD_TYPE_* selection.
    g_disp_width = BSP_LCD_H_RES;
    g_disp_height = BSP_LCD_V_RES;

    // Register the display callbacks with the raylib software renderer.
    raylib_esp_set_display_callbacks(display_flush, display_get_dimensions);

    ESP_LOGI(TAG, "Display initialized: %dx%d", g_disp_width, g_disp_height);
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
    ESP_LOGE(TAG, "  - Create esp_lcd panel/io handles");
    ESP_LOGE(TAG, "  - Provide flush + get-dimension callbacks");
    ESP_LOGE(TAG, "  - Call raylib_esp_set_display_callbacks() before InitWindow()");
    return ESP_FAIL;
}

#endif
