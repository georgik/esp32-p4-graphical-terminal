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
#include <stdint.h>
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
 * @brief One keystroke: the character plus the Ctrl modifier that was held.
 *
 * Ctrl+A-Z is reported as the 0x00-0x1F control code (e.g. Ctrl+C => 0x03).
 * `hid_code` carries the raw HID usage code, so callers can detect shortcuts that
 * carry no character (e.g. F5/F6 font resize) instead of forwarding raw bytes to
 * the UART.
 */
typedef struct {
    char ch;      /* character (control code for Ctrl+letter) */
    bool ctrl;    /* Ctrl was held when the key was pressed   */
    uint8_t hid_code; /* raw HID usage code of the pressed key  */
} key_event_t;

/**
 * @brief Fetch one pending keystroke.
 *
 * @param out_event         Buffer to store the keystroke (non-NULL required)
 * @param wait_ticks        Wait time, e.g. portMAX_DELAY or 0 for non-blocking
 * @return true if a keystroke was retrieved, false otherwise
 */
bool usb_keyboard_get_key(key_event_t *out_event, TickType_t wait_ticks);

/**
 * @brief Detect the F5 font-resize shortcut carried in a keystroke.
 *
 * @param event  Keystroke to test (non-NULL required)
 * @return true if the event is the F5 key
 */
bool usb_keyboard_font_resize_f5(const key_event_t *event);

/**
 * @brief Detect the F6 font-resize shortcut carried in a keystroke.
 *
 * @param event  Keystroke to test (non-NULL required)
 * @return true if the event is the F6 key
 */
bool usb_keyboard_font_resize_f6(const key_event_t *event);

#ifdef __cplusplus
extern "C" {
#endif
#endif // USB_KEYBOARD_H
