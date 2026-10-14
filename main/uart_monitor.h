/**
 * @file uart_monitor.h
 * @brief UART Monitor API for ESP32 UART Logger
 *
 * This module provides UART reception functionality for monitoring
 * external UART data.
 *
 * Supported boards:
 * - ESP32-P4-Function-EV (1024x600/1280x800 DSI LCD)
 */

#ifndef UART_MONITOR_H
#define UART_MONITOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "hal/uart_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default configuration values */
/* Use sdkconfig values if available, otherwise use defaults */
#ifdef CONFIG_UART_MONITOR_BAUD_RATE
    #define UART_MONITOR_BAUD_RATE       CONFIG_UART_MONITOR_BAUD_RATE
#else
    #define UART_MONITOR_BAUD_RATE       115200
#endif

#ifdef CONFIG_UART_MONITOR_TX_PIN
    #define UART_MONITOR_TX_PIN          CONFIG_UART_MONITOR_TX_PIN
#else
    #define UART_MONITOR_TX_PIN      21     // P4 J1 header pin 11 (HP domain)
#endif

#ifdef CONFIG_UART_MONITOR_RX_PIN
    #define UART_MONITOR_RX_PIN          CONFIG_UART_MONITOR_RX_PIN
#else
    #define UART_MONITOR_RX_PIN      22     // P4 J1 header pin 12 (HP domain)
#endif

#define UART_MONITOR_UART_NUM        1      // Use UART1 (UART0 is used for console/logging)
#define UART_MONITOR_RX_BUF_SIZE     2048   // RX buffer size
#define UART_MONITOR_TX_BUF_SIZE     512    // TX buffer size

/* Line buffer configuration */
#ifdef CONFIG_UART_MONITOR_MAX_LINES
    #define UART_MONITOR_MAX_LINES       CONFIG_UART_MONITOR_MAX_LINES
#else
    #define UART_MONITOR_MAX_LINES       45     // Maximum lines to store (P4-Function-EV)
#endif

#ifdef CONFIG_UART_MONITOR_LINE_LENGTH
    #define UART_MONITOR_LINE_LENGTH     CONFIG_UART_MONITOR_LINE_LENGTH
#else
    #define UART_MONITOR_LINE_LENGTH     100     // Max characters per line
#endif

/**
 * @brief UART monitor handle
 */
typedef struct uart_monitor_s *uart_monitor_handle_t;

/**
 * @brief Configuration for UART monitor
 */
typedef struct {
    uart_port_t uart_num;           ///< UART port number (1 or 2)
    int tx_pin;                     ///< TX GPIO pin
    int rx_pin;                     ///< RX GPIO pin
    int baud_rate;                  ///< Baud rate
    int rx_buf_size;                ///< RX buffer size
    int tx_buf_size;                ///< TX buffer size
    int max_lines;                  ///< Maximum lines in history
    int line_length;                ///< Maximum characters per line
} uart_monitor_config_t;

/**
 * @brief Initialize UART monitor with default configuration
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t uart_monitor_init(void);

/**
 * @brief Initialize UART monitor with custom configuration
 *
 * @param config Pointer to configuration structure
 * @param handle Pointer to store the monitor handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t uart_monitor_init_with_config(const uart_monitor_config_t *config,
                                         uart_monitor_handle_t *handle);

/**
 * @brief Get one complete line from UART (newline-terminated)
 *
 * @param buffer Buffer to store the line
 * @param max_len Maximum length of buffer
 * @param wait_ticks Ticks to wait for a complete line (0 = non-blocking)
 * @return ESP_OK if line received, ESP_ERR_TIMEOUT if no data, error code otherwise
 */
esp_err_t uart_monitor_get_line(char *buffer, size_t max_len, TickType_t wait_ticks);

/**
 * @brief Check if there is data available to read
 *
 * @return true if data is available, false otherwise
 */
bool uart_monitor_data_available(void);

/**
 * @brief Send data to UART (TX)
 *
 * @param data Data to send
 * @param len Length of data
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t uart_monitor_send(const char *data, size_t len);

/**
 * @brief Get total bytes received since initialization
 *
 * @return Total RX byte count
 */
uint32_t uart_monitor_get_rx_count(void);

/**
 * @brief Check if actively receiving data
 *
 * @return true if receiving data, false otherwise
 */
bool uart_monitor_is_receiving(void);

/**
 * @brief Clear receiving flag
 */
void uart_monitor_clear_receiving(void);

/**
 * @brief Deinitialize UART monitor
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t uart_monitor_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // UART_MONITOR_H
