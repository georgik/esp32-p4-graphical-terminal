/**
 * @file uart_monitor.c
 * @brief UART Monitor Implementation for the ESP32-P4 graphical terminal
 */

#include "uart_monitor.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "uart_monitor";

/* Internal state */
typedef struct uart_monitor_s {
    uart_port_t uart_num;
    int rx_pin;
    int tx_pin;
    int baud_rate;
    QueueHandle_t uart_queue;
    uint32_t rx_count;
    bool is_receiving;
    char line_buffer[512];          // Accumulated line buffer (may span multiple reads)
    size_t line_index;
} uart_monitor_t;

static uart_monitor_t *s_monitor = NULL;

esp_err_t uart_monitor_init(void)
{
    const uart_monitor_config_t default_config = {
        .uart_num = UART_MONITOR_UART_NUM,
        .tx_pin = UART_MONITOR_TX_PIN,
        .rx_pin = UART_MONITOR_RX_PIN,
        .baud_rate = UART_MONITOR_BAUD_RATE,
        .rx_buf_size = UART_MONITOR_RX_BUF_SIZE,
        .tx_buf_size = UART_MONITOR_TX_BUF_SIZE,
        .max_lines = UART_MONITOR_MAX_LINES,
        .line_length = UART_MONITOR_LINE_LENGTH,
    };
    return uart_monitor_init_with_config(&default_config, NULL);
}

esp_err_t uart_monitor_init_with_config(const uart_monitor_config_t *config,
                                         uart_monitor_handle_t *handle)
{
    if (s_monitor != NULL) {
        ESP_LOGW(TAG, "UART monitor already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "Config cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    /* Allocate monitor structure */
    uart_monitor_t *monitor = calloc(1, sizeof(uart_monitor_t));
    if (monitor == NULL) {
        ESP_LOGE(TAG, "Failed to allocate monitor structure");
        return ESP_ERR_NO_MEM;
    }

    monitor->uart_num = config->uart_num;
    monitor->rx_pin = config->rx_pin;
    monitor->tx_pin = config->tx_pin;
    monitor->baud_rate = config->baud_rate;
    monitor->rx_count = 0;
    monitor->is_receiving = false;
    monitor->line_index = 0;

    ESP_LOGI(TAG, "UART initialization: UART_NUM=%d, TX_GPIO=%d, RX_GPIO=%d, BAUD=%d",
             monitor->uart_num, monitor->tx_pin, monitor->rx_pin, monitor->baud_rate);

    /* Configure UART parameters */
    uart_config_t uart_config = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* Install UART driver FIRST (important: must be before param_config and set_pin) */
    esp_err_t ret = uart_driver_install(monitor->uart_num,
                              config->rx_buf_size,
                              config->tx_buf_size,
                              10,
                              &monitor->uart_queue,
                              0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %d", ret);
        free(monitor);
        return ret;
    }

    /* Configure UART parameters SECOND */
    ret = uart_param_config(monitor->uart_num, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART: %d", ret);
        uart_driver_delete(monitor->uart_num);
        free(monitor);
        return ret;
    }

    /* Set UART pins THIRD */
    ESP_LOGI(TAG, "Configuring UART%d: TX=GPIO%d, RX=GPIO%d, Baud=%d",
             monitor->uart_num, config->tx_pin, config->rx_pin, config->baud_rate);
    ret = uart_set_pin(monitor->uart_num,
                       config->tx_pin,
                       config->rx_pin,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %d", ret);
        uart_driver_delete(monitor->uart_num);
        free(monitor);
        return ret;
    }

    s_monitor = monitor;
    if (handle != NULL) {
        *handle = (uart_monitor_handle_t)monitor;
    }

    ESP_LOGI(TAG, "UART monitor initialized: UART%d, TX=%d, RX=%d, %d baud",
             monitor->uart_num, monitor->tx_pin, monitor->rx_pin, monitor->baud_rate);

    return ESP_OK;
}

esp_err_t uart_monitor_get_line(char *buffer, size_t max_len, TickType_t wait_ticks)
{
    if (s_monitor == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (buffer == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t start_tick = xTaskGetTickCount();

    while (1) {
        /* If a complete line is already buffered, return it. A line ends on
           '\r' or '\n'; a following '\n'/'\r' is part of the same break, so a
           "\r\n" / "\n\r" pair yields exactly one line (no phantom empty line). */
        size_t i = 0;
        for (i = 0; i < s_monitor->line_index; i++) {
            if (s_monitor->line_buffer[i] == '\n' || s_monitor->line_buffer[i] == '\r') {
                char term = s_monitor->line_buffer[i];
                s_monitor->line_buffer[i] = '\0';
                snprintf(buffer, max_len, "%s", s_monitor->line_buffer);

                size_t skip = i + 1;
                if (skip < s_monitor->line_index &&
                    ((term == '\r' && s_monitor->line_buffer[skip] == '\n') ||
                     (term == '\n' && s_monitor->line_buffer[skip] == '\r'))) {
                    skip++;
                }

                s_monitor->line_index -= skip;
                if (s_monitor->line_index > 0) {
                    memmove(s_monitor->line_buffer, &s_monitor->line_buffer[skip], s_monitor->line_index);
                }
                s_monitor->line_buffer[s_monitor->line_index] = '\0';
                return ESP_OK;
            }
        }

        /* No complete line yet: grab the whole burst currently in the FIFO. */
        uint8_t chunk[512];
        size_t n_read = uart_read_bytes(s_monitor->uart_num, chunk, sizeof(chunk), pdMS_TO_TICKS(1));

        for (size_t j = 0; j < n_read; j++) {
            uint8_t byte = chunk[j];

            if (byte == '\n' || byte == '\r') {
                snprintf(buffer, max_len, "%s", s_monitor->line_buffer);

                /* Skip the second half of a "\r\n" / "\n\r" pair so it is not
                   returned as a separate empty line. */
                size_t tail_start = j + 1;
                if (tail_start < n_read &&
                    ((byte == '\r' && chunk[tail_start] == '\n') ||
                     (byte == '\n' && chunk[tail_start] == '\r'))) {
                    tail_start++;
                }

                size_t keep = n_read - tail_start;
                if (keep > sizeof(s_monitor->line_buffer) - 1) {
                    keep = sizeof(s_monitor->line_buffer) - 1;
                }
                if (keep > 0) {
                    memmove(s_monitor->line_buffer, &chunk[tail_start], keep);
                }
                s_monitor->line_index = keep;
                s_monitor->line_buffer[s_monitor->line_index] = '\0';
                return ESP_OK;
            } else if (byte == '\b' || byte == 0x7F) {
                if (s_monitor->line_index > 0) {
                    s_monitor->line_index--;
                }
            } else if (byte == '\t') {
                const char *spaces = "    ";
                for (int k = 0; k < 4 && s_monitor->line_index < sizeof(s_monitor->line_buffer) - 1; k++) {
                    s_monitor->line_buffer[s_monitor->line_index++] = spaces[k];
                }
                s_monitor->line_buffer[s_monitor->line_index] = '\0';
            } else if (byte >= 32 && byte < 127) {
                if (s_monitor->line_index < sizeof(s_monitor->line_buffer) - 1) {
                    s_monitor->line_buffer[s_monitor->line_index++] = (char)byte;
                    s_monitor->line_buffer[s_monitor->line_index] = '\0';
                }
            }
            /* Ignore other control characters. */
        }

        /* Burst consumed with no terminator yet. */
        if (wait_ticks == 0) {
            /* Non-blocking: only return complete lines. */
            return ESP_ERR_TIMEOUT;
        }

        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (elapsed >= wait_ticks) {
            if (s_monitor->line_index > 0) {
                snprintf(buffer, max_len, "%s", s_monitor->line_buffer);
                s_monitor->line_index = 0;
                return ESP_OK;
            }
            return ESP_ERR_TIMEOUT;
        }
    }
}

bool uart_monitor_data_available(void)
{
    if (s_monitor == NULL) {
        return false;
    }

    size_t available;
    uart_get_buffered_data_len(s_monitor->uart_num, &available);
    return available > 0;
}

bool uart_monitor_has_pending_line(void)
{
    if (s_monitor == NULL) {
        return false;
    }

    return s_monitor->line_index > 0;
}

bool uart_monitor_get_pending_line(char *buffer, size_t max_len)
{
    if (s_monitor == NULL || buffer == NULL || max_len == 0) {
        return false;
    }

    if (s_monitor->line_index == 0) {
        buffer[0] = '\0';
        return false;
    }

    snprintf(buffer, max_len, "%s", s_monitor->line_buffer);
    return true;
}

esp_err_t uart_monitor_send(const char *data, size_t len)
{
    if (s_monitor == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = uart_write_bytes(s_monitor->uart_num, data, len);
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write to UART");
        return ESP_FAIL;
    }

    return ESP_OK;
}

uint32_t uart_monitor_get_rx_count(void)
{
    if (s_monitor == NULL) {
        return 0;
    }
    return s_monitor->rx_count;
}

bool uart_monitor_is_receiving(void)
{
    if (s_monitor == NULL) {
        return false;
    }
    return s_monitor->is_receiving;
}

void uart_monitor_clear_receiving(void)
{
    if (s_monitor != NULL) {
        s_monitor->is_receiving = false;
    }
}

esp_err_t uart_monitor_deinit(void)
{
    if (s_monitor == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing UART monitor...");

    /* Uninstall UART driver */
    uart_driver_delete(s_monitor->uart_num);

    /* Free monitor structure */
    free(s_monitor);
    s_monitor = NULL;

    return ESP_OK;
}
