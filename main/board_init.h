/**
 * @file board_init.h
 * @brief Board initialization interface
 */

#pragma once

#include "esp_err.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize display hardware for the raylib software renderer
 *
 * Board-specific initialization including:
 * - Creating esp_lcd panel/io handles
 * - Initializing backlight
 * - Registering flush + get-dimension display callbacks with raylib
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t board_init_display(void);

/**
 * @brief Read the display dimensions reported to raylib
 *
 * Fills the provided pointers with the display resolution that was passed to
 * raylib_esp_set_display_callbacks() during board_init_display().
 *
 * @param width  Output: display width in pixels
 * @param height Output: display height in pixels
 */
void board_init_display_dimensions(uint16_t *width, uint16_t *height);

#ifdef __cplusplus
}
#endif
