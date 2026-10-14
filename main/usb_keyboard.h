/**
 * @file usb_keyboard.h
 * @brief USB HID host keyboard input for the ESP32-P4 UI UART Logger.
 *
 * Reads keystrokes from a plugged-in USB keyboard (USB host mode) and exposes them
 * as a queue of ASCII characters. The display task drains these and forwards them to
 * the monitored UART, giving full duplex communication with the connected board.
 */

#ifndef USB_KEYBOARD_H
#define USB_KEYBOARD_H

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the USB host and HID host drivers and start the input reader.
 *
 * Returns ESP_OK on success. Does not require a keyboard to be connected.
 */
esp_err_t usb_keyboard_init(void);

/**
 * @brief Fetch one pending keystroke character.
 *
 * @param out_char          Buffer to store the character (non-NULL required)
 * @param wait_ticks        Wait time, e.g. portMAX_DELAY or 0 for non-blocking
 * @return true if a character was retrieved, false otherwise
 */
bool usb_keyboard_get_key(char *out_char, TickType_t wait_ticks);

#ifdef __cplusplus
extern "C" {
#endif
#endif // USB_KEYBOARD_H
