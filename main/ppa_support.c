#include "ppa_support.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "driver/ppa.h"

static const char *TAG = "ppa_support";

// raylib software-renderer framebuffer (RGB565, 1024x600). Defined by the raylib
// ESP-IDF software-renderer platform backend and flipped to the LCD by the display
// path, so this is the destination PPA fills write into.
extern void *swGetColorBuffer(int *width, int *height);

static ppa_client_handle_t s_ppa = NULL;
static bool s_initialized = false;
static bool s_active = false;

// The PPA DMA can only target buffers that are both DMA-capable and aligned to the
// cache line size its descriptor engine requires. A 32-byte stride is the value the
// internal-RAM cache line enforces; buffers must be aligned to it. The raylib
// software-renderer framebuffer is malloc'd, so it only qualifies if the heap
// happens to provide a PPA-safe, 32-byte-aligned, DMA-capable pointer.
#define PPA_CACHE_LINE_ALIGN 32

static bool ppa_buffer_is_dma_capable(const void *buf, size_t size)
{
    if (!esp_ptr_dma_capable(buf)) {
        return false;
    }
    if (((uintptr_t)buf % PPA_CACHE_LINE_ALIGN) != 0 || (size % PPA_CACHE_LINE_ALIGN) != 0) {
        return false;
    }
    return true;
}

static uint32_t ppa_color_to_argb8888(Color color)
{
    return ((uint32_t)(color.a & 0xFFu) << 24)
         | ((uint32_t)(color.r & 0xFFu) << 16)
         | ((uint32_t)(color.g & 0xFFu) << 8)
         |  (uint32_t)(color.b & 0xFFu);
}

static uint32_t ppa_framebuffer_size(void)
{
    int w = 0, h = 0;
    uint16_t *fb = (uint16_t *)swGetColorBuffer(&w, &h);
    if (fb == NULL || w <= 0 || h <= 0) {
        return 0;
    }
    return (uint32_t)w * (uint32_t)h * sizeof(uint16_t);
}

bool ppa_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, Color color)
{
    if (!s_initialized || !s_active) {
        return false;  // PPA disabled: caller uses the CPU raylib draw fallback
    }

    int fb_w = 0, fb_h = 0;
    uint16_t *fb = (uint16_t *)swGetColorBuffer(&fb_w, &fb_h);
    if (fb == NULL || fb_w <= 0 || fb_h <= 0) {
        return false;
    }

    if (x + w > (uint32_t)fb_w || y + h > (uint32_t)fb_h) {
        return false;  // rectangle would write past the framebuffer: fall back
    }

    if (!ppa_buffer_is_dma_capable(fb, ppa_framebuffer_size())) {
        return false;  // framebuffer is not PPA-compatible: fall back
    }

    ppa_fill_oper_config_t cfg = {0};
    cfg.out.buffer = fb;
    cfg.out.buffer_size = ppa_framebuffer_size();
    cfg.out.pic_w = (uint32_t)fb_w;
    cfg.out.pic_h = (uint32_t)fb_h;
    cfg.out.block_offset_x = x;
    cfg.out.block_offset_y = y;
    cfg.out.fill_cm = PPA_FILL_COLOR_MODE_RGB565;
    cfg.fill_block_w = w;
    cfg.fill_block_h = h;
    cfg.fill_argb_color.val = ppa_color_to_argb8888(color);
    cfg.mode = PPA_TRANS_MODE_BLOCKING;

    return ppa_do_fill(s_ppa, &cfg) == ESP_OK;
}

esp_err_t ppa_support_init(void)
{
    if (s_initialized) {
        return s_active ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }

    ppa_client_config_t cfg = {0};
    cfg.oper_type = PPA_OPERATION_FILL;
    cfg.max_pending_trans_num = 1;
    cfg.data_burst_length = PPA_DATA_BURST_LENGTH_128;

    esp_err_t ret = ppa_register_client(&cfg, &s_ppa);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA fill client: %s", esp_err_to_name(ret));
        return ret;
    }

    int fb_w = 0, fb_h = 0;
    uint16_t *fb = (uint16_t *)swGetColorBuffer(&fb_w, &fb_h);
    size_t fb_size = ppa_framebuffer_size();
    if (fb == NULL || !ppa_buffer_is_dma_capable(fb, fb_size)) {
        ESP_LOGW(TAG, "Main framebuffer is not PPA DMA-capable: ptr=%p size=%zu dma_capable=%zu align=%zu; "
                      "fill path disabled (CPU raylib draw stays active)",
                      fb, fb_size, fb ? esp_ptr_dma_capable(fb) : 0,
                      fb ? ((uintptr_t)fb % PPA_CACHE_LINE_ALIGN) : 0);
        s_initialized = true;
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_initialized = true;
    s_active = true;
    ESP_LOGI(TAG, "PPA fill support active");
    return ESP_OK;
}

bool ppa_support_active(void)
{
    return s_active;
}
