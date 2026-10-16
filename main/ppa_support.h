#ifndef PPA_SUPPORT_H
#define PPA_SUPPORT_H

#include "raylib.h"
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Initialize PPA fill support.
 *
 * Registers a single PPA client for fill operations. The raylib software-renderer
 * framebuffer is the PPA fill destination.
 *
 * @return ESP_OK if PPA fills are active (the software-renderer framebuffer is
 *         PPA DMA-capable), or an error if the framebuffer is not PPA-compatible,
 *         in which case every ppa_fill() call transparently falls back to the CPU
 *         raylib draw path.
 */
esp_err_t ppa_support_init(void);

/**
 * @brief Fill a rectangle [x, x+w) x [y, y+h) in the software-renderer framebuffer
 *        with a constant color using the PPA.
 *
 * @return true if PPA performed the fill, false if it fell back to the CPU raylib
 *         draw (never fails out). Callers must perform the CPU draw when false.
 */
bool ppa_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, Color color);

/**
 * @brief Returns true if PPA fills are currently active.
 */
bool ppa_support_active(void);

#endif // PPA_SUPPORT_H
