/**
 * @file usb_keyboard.c
 * @brief USB HID host keyboard input for the ESP32-P4 UI UART Logger.
 */

#include "usb_keyboard.h"

#include "esp_err.h"
#include "esp_log.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"

#include <string.h>

static const char *TAG = "usb_keyboard";

#define USB_KEYBOARD_QUEUE_ITEMS  64

static QueueHandle_t s_key_queue = NULL;

/*
 * The USB host drives the HID driver callback from inside the event-processing path,
 * so blocking calls (hid_host_device_open()/start()) must not run there. Enqueue the
 * connect event off the callback and perform the blocking device open/start from
 * hid_process_task instead.
 */
typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} hid_connect_event_t;

static QueueHandle_t s_hid_event_queue = NULL;

/* Keyboard report is 8 bytes: modifier + reserved + 6 key codes. */
#define HID_KB_REPORT_LEN  8

static uint8_t s_prev_keys[HID_KEYBOARD_KEY_MAX] = { 0 };

/**
 * @brief Convert a HID keyboard usage code plus shift state to a printable ASCII char.
 *
 * The usage codes follow the USB HID Usage Tables (Keyboard/Keypad, US-101 layout).
 * The enum names in hid_usage_keyboard.h do not always match the layout, so mapping is
 * done by usage code value.
 *
 * @param key     HID usage code (e.g. HID_KEY_A)
 * @param shift   True when a shift modifier is pressed
 * @return ASCII character, or 0 when the key has no associated character
 */
static char hid_key_to_ascii(uint8_t key, bool shift)
{
    switch (key) {
    case HID_KEY_A:  return shift ? 'A' : 'a';
    case HID_KEY_B:  return shift ? 'B' : 'b';
    case HID_KEY_C:  return shift ? 'C' : 'c';
    case HID_KEY_D:  return shift ? 'D' : 'd';
    case HID_KEY_E:  return shift ? 'E' : 'e';
    case HID_KEY_F:  return shift ? 'F' : 'f';
    case HID_KEY_G:  return shift ? 'G' : 'g';
    case HID_KEY_H:  return shift ? 'H' : 'h';
    case HID_KEY_I:  return shift ? 'I' : 'i';
    case HID_KEY_J:  return shift ? 'J' : 'j';
    case HID_KEY_K:  return shift ? 'K' : 'k';
    case HID_KEY_L:  return shift ? 'L' : 'l';
    case HID_KEY_M:  return shift ? 'M' : 'm';
    case HID_KEY_N:  return shift ? 'N' : 'n';
    case HID_KEY_O:  return shift ? 'O' : 'o';
    case HID_KEY_P:  return shift ? 'P' : 'p';
    case HID_KEY_Q:  return shift ? 'Q' : 'q';
    case HID_KEY_R:  return shift ? 'R' : 'r';
    case HID_KEY_S:  return shift ? 'S' : 's';
    case HID_KEY_T:  return shift ? 'T' : 't';
    case HID_KEY_U:  return shift ? 'U' : 'u';
    case HID_KEY_V:  return shift ? 'V' : 'v';
    case HID_KEY_W:  return shift ? 'W' : 'w';
    case HID_KEY_X:  return shift ? 'X' : 'x';
    case HID_KEY_Y:  return shift ? 'Y' : 'y';
    case HID_KEY_Z:  return shift ? 'Z' : 'z';

    case HID_KEY_1:  return shift ? '!' : '1';
    case HID_KEY_2:  return shift ? '@' : '2';
    case HID_KEY_3:  return shift ? '#' : '3';
    case HID_KEY_4:  return shift ? '$' : '4';
    case HID_KEY_5:  return shift ? '%' : '5';
    case HID_KEY_6:  return shift ? '^' : '6';
    case HID_KEY_7:  return shift ? '&' : '7';
    case HID_KEY_8:  return shift ? '*' : '8';
    case HID_KEY_9:  return shift ? '(' : '9';
    case HID_KEY_0:  return shift ? ')' : '0';

    case HID_KEY_ENTER:  return '\n';
    case HID_KEY_ESC:    return 27;
    case HID_KEY_TAB:    return '\t';
    case HID_KEY_SPACE:  return ' ';

    /* Backspace is reported as both DEL (0x2A) and, on some keyboards, 0x7F. */
    case HID_KEY_DEL:
    case 0x7F:
        return '\b';

    case HID_KEY_MINUS:  return shift ? '_' : '-';
    case HID_KEY_EQUAL:  return shift ? '+' : '=';
    case HID_KEY_OPEN_BRACKET:  return shift ? '{' : '[';
    case HID_KEY_CLOSE_BRACKET: return shift ? '}' : ']';
    case HID_KEY_BACK_SLASH: return shift ? '|' : '\\';
    case HID_KEY_SHARP:    return shift ? '~' : '`';
    case HID_KEY_COLON:    return shift ? ':' : ';';
    case HID_KEY_QUOTE:    return shift ? '"' : '\'';
    case HID_KEY_TILDE:    return shift ? '<' : ',';
    case HID_KEY_LESS:     return shift ? '>' : '.';
    case HID_KEY_GREATER:  return shift ? '?' : '/';

    default:
        return 0;
    }
}

static bool key_present_in_report(const uint8_t *report, uint8_t key)
{
    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        if (report[i] == key) {
            return true;
        }
    }
    return false;
}

static void keyboard_report_process(const hid_keyboard_input_report_boot_t *report)
{
    bool shift = (report->modifier.val & (HID_LEFT_SHIFT | HID_RIGHT_SHIFT)) != 0;

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        uint8_t released = s_prev_keys[i];
        uint8_t pressed = report->key[i];

        /* A key that was pressed before but is gone -> release (ignored for UART). */
        if (released > HID_KEY_ERROR_UNDEFINED && !key_present_in_report(report->key, released)) {
            continue;
        }

        /* A newly pressed key -> emit its character. */
        if (pressed > HID_KEY_ERROR_UNDEFINED && !key_present_in_report(s_prev_keys, pressed)) {
            char c = hid_key_to_ascii(pressed, shift);
            if (c != 0 && s_key_queue != NULL) {
                xQueueSend(s_key_queue, &c, 0);
                ESP_LOGV(TAG, "report_process: key '%c'(%d) enqueued", c, (int)c);
            }
        }
    }

    memcpy(s_prev_keys, report->key, sizeof(s_prev_keys));
}

static void keyboard_interface_callback(hid_host_device_handle_t handle,
                                        const hid_host_interface_event_t event,
                                        void *arg)
{
    (void) arg;

    if (event != HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        return;
    }

    uint8_t report[HID_KB_REPORT_LEN] = { 0 };
    size_t report_len = sizeof(report);

    if (hid_host_device_get_raw_input_report_data(handle, report, sizeof(report), &report_len) != ESP_OK) {
        return;
    }

    if (report_len < sizeof(hid_keyboard_input_report_boot_t)) {
        return;
    }

    ESP_LOGV(TAG, "interface_cb: report received (%zu bytes)", report_len);

    keyboard_report_process((const hid_keyboard_input_report_boot_t *) report);
}

/*
 * HID host driver-event callback. The USB host invokes it from inside the
 * event-processing path, so this only records the connect event off-callback;
 * the blocking device open/start/polling setup runs in hid_process_task.
 */
static void keyboard_driver_callback(hid_host_device_handle_t handle,
                                      const hid_host_driver_event_t event,
                                      void *arg)
{
    (void) arg;

    ESP_LOGV(TAG, "driver_cb: event=%d", (int)event);
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    /* Reset transition tracking for a freshly connected keyboard. */
    memset(s_prev_keys, 0, sizeof(s_prev_keys));

    /*
     * The callback runs inside the event-processing path, so enqueue only.
     * Blocking device open/start runs off-callback in hid_process_task.
     */
    hid_connect_event_t evt = {
        .handle = handle,
        .event = event,
    };
    if (s_hid_event_queue != NULL) {
        if (xQueueSend(s_hid_event_queue, &evt, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGV(TAG, "driver_cb: queue send failed");
        }
    }
}

/*
 * Blocking device open/start/polling setup run here, off the HID callback context.
 * Mirrors the proven-working OpenTyrian USB HID host flow.
 */
static void hid_process_task(void *arg)
{
    (void) arg;

    hid_connect_event_t evt;
    esp_err_t err;
    while (1) {
        if (xQueueReceive(s_hid_event_queue, &evt, portMAX_DELAY) != pdTRUE ||
                evt.event != HID_HOST_DRIVER_EVENT_CONNECTED) {
            continue;
        }

        hid_host_device_handle_t handle = evt.handle;

        const hid_host_device_config_t cfg = {
            .callback = keyboard_interface_callback,
            .callback_arg = NULL,
        };

        esp_err_t open_ret = hid_host_device_open(handle, &cfg);
        ESP_LOGV(TAG, "hid_process: open=%d %s", (int)open_ret,
                open_ret >= 0 ? "ok" : esp_err_to_name(open_ret));

        /* Set the boot protocol (and idle) for a boot-protocol keyboard. */
        hid_host_dev_params_t dev_params;
        if (open_ret == ESP_OK &&
                hid_host_device_get_params(handle, &dev_params) == ESP_OK &&
                HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class &&
                HID_PROTOCOL_KEYBOARD == dev_params.proto) {
            err = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
            ESP_LOGV(TAG, "hid_process: set_protocol=%d", (int)err);
            err = hid_class_request_set_idle(handle, 0, 0);
            ESP_LOGV(TAG, "hid_process: set_idle=%d", (int)err);
        }

        if (open_ret != ESP_OK) {
            ESP_LOGW(TAG, "hid_host_device_open() failed: %s", esp_err_to_name(open_ret));
        }

        esp_err_t start_ret = hid_host_device_start(handle);
        ESP_LOGV(TAG, "hid_process: start=%d %s", (int)start_ret,
                start_ret >= 0 ? "ok" : esp_err_to_name(start_ret));
        if (start_ret != ESP_OK) {
            ESP_LOGW(TAG, "hid_host_device_start() failed: %s", esp_err_to_name(start_ret));
        }
    }
    vTaskDelete(NULL);
}

static void usb_lib_task(void *arg)
{
    (void) arg;

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
    vTaskDelete(NULL);
}

static void hid_event_task(void *arg)
{
    (void) arg;

    while (1) {
        hid_host_handle_events(portMAX_DELAY);
    }
    vTaskDelete(NULL);
}

esp_err_t usb_keyboard_init(void)
{
    if (s_key_queue != NULL) {
        ESP_LOGW(TAG, "USB keyboard already initialized");
        return ESP_OK;
    }

    s_hid_event_queue = xQueueCreate(10, sizeof(hid_connect_event_t));
    if (s_hid_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create HID event queue");
        return ESP_FAIL;
    }

    /* P4 USB host line-state interrupts require the GPIO ISR service. */
    if (gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1) != ESP_OK) {
        ESP_LOGW(TAG, "gpio_install_isr_service() failed or already installed");
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB host: %s", esp_err_to_name(ret));
        vQueueDelete(s_hid_event_queue);
        s_hid_event_queue = NULL;
        return ret;
    }

    if (!xTaskCreate(usb_lib_task, "usb_lib", 16384, NULL, 9, NULL)) {
        ESP_LOGE(TAG, "Failed to create USB lib task");
        usb_host_uninstall();
        vQueueDelete(s_hid_event_queue);
        s_hid_event_queue = NULL;
        return ESP_FAIL;
    }

    const hid_host_driver_config_t hid_config = {
        .create_background_task = false,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = keyboard_driver_callback,
        .callback_arg = NULL,
    };
    ret = hid_host_install(&hid_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install HID host: %s", esp_err_to_name(ret));
        usb_host_uninstall();
        vQueueDelete(s_hid_event_queue);
        s_hid_event_queue = NULL;
        return ret;
    }

    if (!xTaskCreate(hid_event_task, "hid_evt", 16384, NULL, 8, NULL)) {
        ESP_LOGE(TAG, "Failed to create HID event task");
        hid_host_uninstall();
        usb_host_uninstall();
        vQueueDelete(s_hid_event_queue);
        s_hid_event_queue = NULL;
        return ESP_FAIL;
    }

    if (!xTaskCreate(hid_process_task, "hid_proc", 16384, NULL, 7, NULL)) {
        ESP_LOGE(TAG, "Failed to create HID process task");
        hid_host_uninstall();
        usb_host_uninstall();
        vQueueDelete(s_hid_event_queue);
        s_hid_event_queue = NULL;
        return ESP_FAIL;
    }

    s_key_queue = xQueueCreate(USB_KEYBOARD_QUEUE_ITEMS, sizeof(char));
    if (s_key_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create keyboard queue");
        hid_host_uninstall();
        usb_host_uninstall();
        vQueueDelete(s_hid_event_queue);
        s_hid_event_queue = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "USB keyboard ready");
    return ESP_OK;
}

bool usb_keyboard_get_key(char *out_char, TickType_t wait_ticks)
{
    if (s_key_queue == NULL || out_char == NULL) {
        return false;
    }
    return (xQueueReceive(s_key_queue, out_char, wait_ticks) == pdTRUE);
}
