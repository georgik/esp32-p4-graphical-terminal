#include "raylib.h"
#include "board_init.h"
#include "esp_raylib_port.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uart_monitor.h"
#include "usb_keyboard.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#define TAG "UARTMonitor"

/* Display configuration - scale based on target board */
/* ESP32-P4-Function-EV: 1024x600 */
#define RAYLIB_TASK_STACK_SIZE (256 * 1024)  // 256KB stack for P4
#define MAX_VISIBLE_LINES 45
#define LINE_BUFFER_SIZE 100
#define STATUS_BAR_HEIGHT 30         // Fits 20px font + padding
#define BOTTOM_BAR_HEIGHT 24         // Fits 18px font + padding
#define CONTENT_GAP 2                // Black gap above content only
#define FONT_SIZE 28
#define LINE_SPACING 34
#define MARGIN_LEFT 10
#define MARGIN_TOP (STATUS_BAR_HEIGHT + CONTENT_GAP)  // Start below status bar with gap
#define MARGIN_BOTTOM BOTTOM_BAR_HEIGHT  // Content goes right to bottom bar

/* Line history buffer */
typedef struct {
    char lines[MAX_VISIBLE_LINES][LINE_BUFFER_SIZE];
    int line_count;
    int line_start;  // Ring buffer start index
} line_history_t;

/* HP core state enumeration */
typedef enum {
    HP_STATE_ACTIVE,      // HP core is running (full brightness, green outline)
    HP_STATE_LIGHT_SLEEP, // HP core in light sleep (80% opacity)
    HP_STATE_DEEP_SLEEP   // HP core in deep sleep (50% opacity)
} hp_core_state_t;

/**
 * @brief Add a line to the history buffer
 */
static void add_line_to_history(line_history_t *history, const char *line)
{
    if (history->line_count < MAX_VISIBLE_LINES) {
        // Buffer not full yet
        snprintf(history->lines[history->line_count], LINE_BUFFER_SIZE, "%s", line);
        history->line_count++;
    } else {
        // Buffer full, wrap around
        snprintf(history->lines[history->line_start], LINE_BUFFER_SIZE, "%s", line);
        history->line_start = (history->line_start + 1) % MAX_VISIBLE_LINES;
    }
}

/**
 * @brief Get a line from history by index (0 = oldest in buffer, count-1 = newest)
 * When buffer is not full, index 0 is the first line ever received.
 */
static const char *get_line_from_history(const line_history_t *history, int index)
{
    if (index < 0 || index >= history->line_count) {
        return "";
    }
    // When buffer is not full, just use index directly
    if (history->line_count < MAX_VISIBLE_LINES) {
        return history->lines[index];
    }
    // When buffer is full, use circular buffer logic
    int actual_index = (history->line_start + index) % MAX_VISIBLE_LINES;
    return history->lines[actual_index];
}

/**
 * @brief Parse UART message to determine HP core state
 * @param message The received UART message
 * @return The detected HP core state
 */
static hp_core_state_t parse_hp_state(const char *message)
{
    if (message == NULL) {
        return HP_STATE_ACTIVE;  // Default to active
    }

    if (strstr(message, "HP_ACTIVE") != NULL) {
        return HP_STATE_ACTIVE;
    } else if (strstr(message, "HP_LIGHT_SLEEP") != NULL) {
        return HP_STATE_LIGHT_SLEEP;
    } else if (strstr(message, "HP_DEEP_SLEEP") != NULL) {
        return HP_STATE_DEEP_SLEEP;
    }

    // Return current state if no state change detected (caller should track)
    return HP_STATE_ACTIVE;  // This won't be used if caller implements proper tracking
}

/**
 * @brief Draw the status bar at the top
 */
static void draw_status_bar(int screenWidth, int screenHeight, uint32_t rx_count, int baud_rate, Font mono_font)
{
    // Background
    DrawRectangle(0, 0, screenWidth, STATUS_BAR_HEIGHT, DARKBLUE);

    // Title
    DrawTextEx(mono_font, "P4 Graphical Terminal", (Vector2){MARGIN_LEFT, 5}, 20, 0, WHITE);

    // Baud rate
    char baud_text[32];
    int baud_x = MARGIN_LEFT + 250;  // Position after title (adjusted for larger font)
    snprintf(baud_text, sizeof(baud_text), "%d baud", baud_rate);
    DrawTextEx(mono_font, baud_text, (Vector2){baud_x, 5}, 20, 0, WHITE);

    // RX count - position from right edge
    char count_text[32];
    if (rx_count < 1000) {
        snprintf(count_text, sizeof(count_text), "RX: %" PRIu32, rx_count);
    } else if (rx_count < 1000000) {
        snprintf(count_text, sizeof(count_text), "RX: %" PRIu32 "K", rx_count / 1000);
    } else {
        snprintf(count_text, sizeof(count_text), "RX: %" PRIu32 "M", rx_count / 1000000);
    }
    int count_width = (int)(MeasureTextEx(mono_font, count_text, 20, 0).x);
    DrawTextEx(mono_font, count_text, (Vector2){screenWidth - count_width - MARGIN_LEFT, 5}, 20, 0, YELLOW);
}

/**
 * @brief Draw the main content area with UART lines
 */
static void draw_content_area(int screenWidth, int screenHeight, const line_history_t *history, Font mono_font)
{
    // Content area: left half of screen (50%)
    int available_height = screenHeight - MARGIN_TOP - MARGIN_BOTTOM;
    int max_visible_lines = available_height / LINE_SPACING;

    // Calculate which lines to show (always show the most recent)
    int start_index = 0;
    int lines_to_draw = history->line_count;
    int start_y = MARGIN_TOP;

    if (history->line_count >= max_visible_lines) {
        // Buffer is full, show only the most recent lines
        start_index = history->line_count - max_visible_lines;
        lines_to_draw = max_visible_lines;
    } else {
        // Buffer not full, start drawing from bottom (terminal style)
        start_y = MARGIN_TOP + (max_visible_lines - history->line_count) * LINE_SPACING;
    }

    // Draw each visible line
    for (int i = 0; i < lines_to_draw; i++) {
        const char *line = get_line_from_history(history, start_index + i);
        int y = start_y + (i * LINE_SPACING);
        DrawTextEx(mono_font, line, (Vector2){MARGIN_LEFT, y}, FONT_SIZE, 0, GREEN);
    }
}

/**
 * @brief Draw the bottom status bar
 */
static void draw_bottom_bar(int screenWidth, int screenHeight, bool is_receiving, Font mono_font)
{
    int y = screenHeight - BOTTOM_BAR_HEIGHT;

    // Background
    DrawRectangle(0, y, screenWidth, BOTTOM_BAR_HEIGHT, DARKGRAY);

    // Status text - board-specific
#ifdef CONFIG_BOARD_ESP32_P4_FUNCTION_EV
    DrawTextEx(mono_font, "Logs", (Vector2){MARGIN_LEFT, y + 5}, 18, 0, WHITE);
#elif CONFIG_BOARD_ESP_BOX_3
    DrawTextEx(mono_font, "Logs | Cores", (Vector2){MARGIN_LEFT, y + 5}, 18, 0, WHITE);
#else
    DrawTextEx(mono_font, "Logs | Cores", (Vector2){MARGIN_LEFT, y + 5}, 18, 0, WHITE);
#endif

    // RX indicator (blinking green circle)
    static int blink_counter = 0;
    if (is_receiving) {
        blink_counter++;
        if ((blink_counter / 10) % 2 == 0) {
            DrawCircle(screenWidth - 15, y + 7, 4, GREEN);
        }
    } else {
        blink_counter = 0;
    }
}

/**
 * @brief Draw ESP32-C6 core visualization on the right side
 */
static void draw_core_visualization(int screenWidth, int screenHeight, hp_core_state_t hp_state,
                                      Texture2D hp_active_texture, Texture2D hp_light_sleep_texture, Texture2D hp_deep_sleep_texture,
                                      Texture2D lp_core_texture)
{
    static bool s_core_viz_enabled = false;

    // Right panel area (50% of screen width)
    int panel_x = screenWidth / 2;
    int panel_width = screenWidth - panel_x;
    int panel_y = MARGIN_TOP;
    int panel_height = screenHeight - MARGIN_TOP - MARGIN_BOTTOM;

    // HP/LP core pictures are hidden (kept in code for a later feature) and the
    // vertical divider has been removed so the full line width is used for UART.
    if (!s_core_viz_enabled) {
        return;
    }

    // Calculate square sizes
    // HP core: 72% of the right panel area (10% smaller than 80%)
    int hp_size = (int)(panel_height * 0.72);
    if (hp_size > panel_width - 20) hp_size = panel_width - 20;  // Fit in width

    // LP core: 20% of the right panel area (2x bigger than 10%)
    int lp_size = (int)(panel_height * 0.2);
    if (lp_size < 20) lp_size = 20;  // Minimum size

    // HP core position (centered in upper part)
    int hp_x = panel_x + (panel_width - hp_size) / 2;
    int hp_y = panel_y + 20;

    // LP core position (moved to the right side)
    int lp_x = panel_x + panel_width - lp_size - 20;
    int lp_y = panel_y + panel_height - lp_size - 20;

    // Select HP core icon based on state
    Texture2D hp_texture_to_draw = {0};

    switch (hp_state) {
        case HP_STATE_ACTIVE:
            hp_texture_to_draw = hp_active_texture;
            break;
        case HP_STATE_LIGHT_SLEEP:
            hp_texture_to_draw = hp_light_sleep_texture;
            break;
        case HP_STATE_DEEP_SLEEP:
            hp_texture_to_draw = hp_deep_sleep_texture;
            break;
    }

    // Draw HP core (use state-appropriate texture if available, otherwise fallback to square)
    if (hp_texture_to_draw.id != 0) {
        // Draw the texture
        Rectangle source_rect = {0, 0, hp_texture_to_draw.width, hp_texture_to_draw.height};
        Rectangle dest_rect = {hp_x, hp_y, hp_size, hp_size};
        Vector2 origin = {0, 0};

        DrawTexturePro(hp_texture_to_draw, source_rect, dest_rect, origin, 0.0f, WHITE);
    } else {
        // Fallback: Draw HP core square with state-based color
        Color hp_color = BLUE;
        switch (hp_state) {
            case HP_STATE_ACTIVE:
                hp_color = BLUE;
                break;
            case HP_STATE_LIGHT_SLEEP:
            case HP_STATE_DEEP_SLEEP:
                hp_color = DARKBLUE;
                break;
        }
        DrawRectangle(hp_x, hp_y, hp_size, hp_size, hp_color);
    }

    // HP core label (two lines: title and state)
    int hp_title_size = (screenWidth > 500) ? 20 : 10;
    int hp_state_size = (screenWidth > 500) ? 16 : 8;
    const char *hp_title = "High Power Core";

    // State name based on current state
    const char *hp_state_text = "Unknown";
    switch (hp_state) {
        case HP_STATE_ACTIVE:
            hp_state_text = "Active";
            break;
        case HP_STATE_LIGHT_SLEEP:
            hp_state_text = "Light Sleep";
            break;
        case HP_STATE_DEEP_SLEEP:
            hp_state_text = "Deep Sleep";
            break;
    }

    // Position title in top 25% of HP core
    int hp_title_y = hp_y + (hp_size * 0.25) - (hp_title_size / 2);
    int hp_title_width = MeasureText(hp_title, hp_title_size);
    DrawText(hp_title, hp_x + (hp_size - hp_title_width) / 2, hp_title_y, hp_title_size, WHITE);

    // Position state in bottom 25% of HP core
    int hp_state_y = hp_y + (hp_size * 0.75) - (hp_state_size / 2);
    int hp_state_width = MeasureText(hp_state_text, hp_state_size);
    DrawText(hp_state_text, hp_x + (hp_size - hp_state_width) / 2, hp_state_y, hp_state_size, WHITE);

    // Draw LP core (use texture if available, otherwise fallback to square)
    if (lp_core_texture.id != 0) {
        // Draw the texture
        Rectangle source_rect = {0, 0, lp_core_texture.width, lp_core_texture.height};
        Rectangle dest_rect = {lp_x, lp_y, lp_size, lp_size};
        Vector2 origin = {0, 0};

        DrawTexturePro(lp_core_texture, source_rect, dest_rect, origin, 0.0f, WHITE);

        // LP core label (centered over the image)
        int lp_label_size = (screenWidth > 500) ? 10 : 6;
        const char *lp_label = "LP Core";
        int lp_label_width = MeasureText(lp_label, lp_label_size);
        DrawText(lp_label, lp_x + (lp_size - lp_label_width) / 2, lp_y + lp_size / 2 - lp_label_size / 2, lp_label_size, WHITE);
    } else {
        // Fallback: Draw LP core square (small, orange)
        DrawRectangle(lp_x, lp_y, lp_size, lp_size, ORANGE);

        // LP core label
        int lp_label_size = (screenWidth > 500) ? 10 : 6;
        const char *lp_label = "LP Core";
        int lp_label_width = MeasureText(lp_label, lp_label_size);
        DrawText(lp_label, lp_x + (lp_size - lp_label_width) / 2, lp_y + lp_size / 2 - lp_label_size / 2, lp_label_size, WHITE);
    }

    // Title for the right panel
    int title_size = (screenWidth > 500) ? 20 : 12;
    const char *title = "ESP32-C6 Cores";
    int title_width = MeasureText(title, title_size);
    DrawText(title, panel_x + (panel_width - title_width) / 2, panel_y + 5, title_size, GRAY);
}

/**
 * @brief Draw startup screen
 */
static void draw_startup_screen(int screenWidth, int screenHeight, int baud_rate, int rx_pin, int tx_pin, Font mono_font)
{
    BeginDrawing();
    ClearBackground(BLACK);

    // Title - scale with screen size
    const char *title = "UART Monitor";
    int title_size = (screenWidth > 500) ? 100 : 30;  // 2x larger title for P4
    int title_width = (int)(MeasureTextEx(mono_font, title, title_size, 0).x);
    DrawTextEx(mono_font, title, (Vector2){(screenWidth - title_width) / 2, 80}, title_size, 0, WHITE);

    // Configuration info
    char info[128];
    int y = (screenHeight > 400) ? 240 : 100;  // Lower start position for larger screen
    int font_size = (screenWidth > 500) ? 40 : 14;  // 2x larger font for P4

    snprintf(info, sizeof(info), "Baud Rate: %d", baud_rate);
    DrawTextEx(mono_font, info, (Vector2){80, y}, font_size, 0, WHITE);
    y += (screenWidth > 500) ? 60 : 25;

    snprintf(info, sizeof(info), "RX Pin: GPIO%d", rx_pin);
    DrawTextEx(mono_font, info, (Vector2){80, y}, font_size, 0, WHITE);
    y += (screenWidth > 500) ? 60 : 25;

    snprintf(info, sizeof(info), "TX Pin: GPIO%d", tx_pin);
    DrawTextEx(mono_font, info, (Vector2){80, y}, font_size, 0, WHITE);
    y += (screenWidth > 500) ? 100 : 40;

    // Instructions - board-specific
    DrawTextEx(mono_font, "Waiting for data...", (Vector2){80, y}, font_size, 0, YELLOW);
    y += (screenWidth > 500) ? 80 : 30;

#ifdef CONFIG_BOARD_ESP32_P4_FUNCTION_EV
    DrawTextEx(mono_font, "Connect ESP32-C6 LP_UART:", (Vector2){80, y}, 24, 0, GRAY);
    y += 40;
    DrawTextEx(mono_font, "C6 GPIO5 (TX) -> P4 GPIO22 (RX)", (Vector2){80, y}, 24, 0, GRAY);
    y += 30;
    DrawTextEx(mono_font, "C6 GPIO4 (RX) <- P4 GPIO21 (TX)", (Vector2){80, y}, 24, 0, GRAY);
    y += 30;
    DrawTextEx(mono_font, "GND -> GND", (Vector2){80, y}, 24, 0, GRAY);
#else
    DrawTextEx(mono_font, "Connect external UART:", (Vector2){80, y}, 24, 0, GRAY);
    y += 40;
    snprintf(info, sizeof(info), "External TX -> GPIO%d (RX)", rx_pin);
    DrawTextEx(mono_font, info, (Vector2){80, y}, 24, 0, GRAY);
    y += 30;
    DrawTextEx(mono_font, "GND -> GND", (Vector2){80, y}, 24, 0, GRAY);
#endif

    EndDrawing();
}

void uart_monitor_display_task(void *pvParameter)
{
    // Query actual display dimensions
    uint16_t screenWidth = 320;
    uint16_t screenHeight = 240;

    esp_err_t ret = ray_port_get_dimensions(&screenWidth, &screenHeight);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get display dimensions, using defaults");
    }

    ESP_LOGI(TAG, "Initializing UART Monitor with display: %dx%d...", screenWidth, screenHeight);
    InitWindow(screenWidth, screenHeight, "UART Monitor");

    // Initialize and mount LittleFS
    ESP_LOGI(TAG, "Initializing LittleFS...");
    esp_vfs_littlefs_conf_t littlefs_conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    ret = esp_vfs_littlefs_register(&littlefs_conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        ESP_LOGW(TAG, "Will use fallback rendering (blue/orange squares)");
    } else {
        ESP_LOGI(TAG, "LittleFS mounted successfully");
    }

    // Load HP core icons (3 states) and LP core icon from LittleFS
    Texture2D hp_active_texture = {0};
    Texture2D hp_light_sleep_texture = {0};
    Texture2D hp_deep_sleep_texture = {0};
    Texture2D lp_core_texture = {0};

    if (ret == ESP_OK) {
        // Load HP core ACTIVE state icon
        const char *hp_active_path = "/littlefs/png/core-icon-active-512.png";
        FILE *fp = fopen(hp_active_path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            uint8_t *png_data = malloc(file_size);
            if (png_data) {
                size_t read_size = fread(png_data, 1, file_size, fp);
                fclose(fp);

                Image icon_image = LoadImageFromMemory(".png", png_data, read_size);
                free(png_data);

                if (icon_image.data != NULL) {
                    hp_active_texture = LoadTextureFromImage(icon_image);
                    UnloadImage(icon_image);
                    ESP_LOGI(TAG, "HP ACTIVE icon loaded: %dx%d", icon_image.width, icon_image.height);
                }
            } else {
                fclose(fp);
            }
        } else {
            ESP_LOGW(TAG, "HP ACTIVE icon not found at %s", hp_active_path);
        }

        // Load HP core LIGHT SLEEP state icon
        const char *hp_light_sleep_path = "/littlefs/png/core-icon-light-sleep-512.png";
        fp = fopen(hp_light_sleep_path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            uint8_t *png_data = malloc(file_size);
            if (png_data) {
                size_t read_size = fread(png_data, 1, file_size, fp);
                fclose(fp);

                Image icon_image = LoadImageFromMemory(".png", png_data, read_size);
                free(png_data);

                if (icon_image.data != NULL) {
                    hp_light_sleep_texture = LoadTextureFromImage(icon_image);
                    UnloadImage(icon_image);
                    ESP_LOGI(TAG, "HP LIGHT SLEEP icon loaded: %dx%d", icon_image.width, icon_image.height);
                }
            } else {
                fclose(fp);
            }
        } else {
            ESP_LOGW(TAG, "HP LIGHT SLEEP icon not found at %s", hp_light_sleep_path);
        }

        // Load HP core DEEP SLEEP state icon
        const char *hp_deep_sleep_path = "/littlefs/png/core-icon-deep-sleep-512.png";
        fp = fopen(hp_deep_sleep_path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            uint8_t *png_data = malloc(file_size);
            if (png_data) {
                size_t read_size = fread(png_data, 1, file_size, fp);
                fclose(fp);

                Image icon_image = LoadImageFromMemory(".png", png_data, read_size);
                free(png_data);

                if (icon_image.data != NULL) {
                    hp_deep_sleep_texture = LoadTextureFromImage(icon_image);
                    UnloadImage(icon_image);
                    ESP_LOGI(TAG, "HP DEEP SLEEP icon loaded: %dx%d", icon_image.width, icon_image.height);
                }
            } else {
                fclose(fp);
            }
        } else {
            ESP_LOGW(TAG, "HP DEEP SLEEP icon not found at %s", hp_deep_sleep_path);
        }

        // Load LP core icon
        const char *lp_png_path = "/littlefs/png/lp-core-icon-128.png";
        fp = fopen(lp_png_path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            uint8_t *png_data = malloc(file_size);
            if (png_data) {
                size_t read_size = fread(png_data, 1, file_size, fp);
                fclose(fp);

                Image icon_image = LoadImageFromMemory(".png", png_data, read_size);
                free(png_data);

                if (icon_image.data != NULL) {
                    lp_core_texture = LoadTextureFromImage(icon_image);
                    UnloadImage(icon_image);
                    ESP_LOGI(TAG, "LP core icon loaded: %dx%d", icon_image.width, icon_image.height);
                }
            } else {
                fclose(fp);
            }
        } else {
            ESP_LOGW(TAG, "LP core icon not found at %s", lp_png_path);
        }
    }

    ESP_LOGI(TAG, "UART Monitor Initialized. Starting main loop...");
    ESP_LOGI(TAG, "Screen dimensions: %dx%d", screenWidth, screenHeight);

    /* Load the monospace (Anonymous Pro) font for terminal-style text.
     * Falls back to Raylib's default font if the font is unavailable. */
     Font mono_font = GetFontDefault();
     bool mono_loaded = false;
     {
        const char *mono_path = "/littlefs/fonts/anonymous_pro_bold.ttf";
        FILE *fp = fopen(mono_path, "r");
        if (fp) {
            fclose(fp);
            Font loaded = LoadFontEx(mono_path, FONT_SIZE, NULL, 0);
            if (loaded.glyphCount > 0) {
                mono_font = loaded;
                mono_loaded = true;
                ESP_LOGI(TAG, "Anonymous Pro monospace font loaded");
            } else {
                ESP_LOGW(TAG, "Anonymous Pro font failed to rasterize, using default font");
            }
        } else {
            ESP_LOGW(TAG, "Anonymous Pro font not found at %s, using default font", mono_path);
        }
     }

    // Show startup screen for 2 seconds
    draw_startup_screen(screenWidth, screenHeight, UART_MONITOR_BAUD_RATE,
                        UART_MONITOR_RX_PIN, UART_MONITOR_TX_PIN, mono_font);
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Initialize line history
    line_history_t history = {
        .line_count = 0,
        .line_start = 0,
    };

    // Add welcome message
    add_line_to_history(&history, "--- UART Monitor Started ---");
    add_line_to_history(&history, "Waiting for data...");

    uint32_t last_rx_count = 0;
    int frame_counter = 0;
    char temp_line[LINE_BUFFER_SIZE];
    hp_core_state_t hp_state = HP_STATE_ACTIVE;  // Default to active state

    // Main display loop
    while (!WindowShouldClose()) {
        bool got_any_line = false;

        // Drain every line currently available (near-real-time: no per-line throttle,
        // drain the whole burst then draw once).
        while (uart_monitor_get_line(temp_line, sizeof(temp_line), 0) == ESP_OK) {
            // Add line to history
            add_line_to_history(&history, temp_line);
            // Log to console
            ESP_LOGI(TAG, "RX: %s", temp_line);

            // Check for HP state change
            hp_core_state_t new_state = parse_hp_state(temp_line);
            if (new_state != HP_STATE_ACTIVE ||
                strstr(temp_line, "HP_ACTIVE") != NULL ||
                strstr(temp_line, "HP_LIGHT_SLEEP") != NULL ||
                strstr(temp_line, "HP_DEEP_SLEEP") != NULL) {
                hp_state = new_state;
            }
        }

        // Forward any keystrokes from the USB keyboard to the UART so the
        // connected board sees what is typed (full duplex communication).
        char key;
        while (usb_keyboard_get_key(&key, 0)) {
            uart_monitor_send(&key, 1);
        }

        // Update receiving status (clear if no data for 10 frames)
        uint32_t current_rx_count = uart_monitor_get_rx_count();
        bool is_receiving = (current_rx_count > last_rx_count);
        if (!is_receiving && frame_counter % 10 == 0) {
            uart_monitor_clear_receiving();
        }
        last_rx_count = current_rx_count;

        // Begin drawing
        BeginDrawing();

        // Clear background
        ClearBackground(BLACK);

        // Draw UI elements
        draw_status_bar(screenWidth, screenHeight, current_rx_count, UART_MONITOR_BAUD_RATE, mono_font);
        draw_content_area(screenWidth, screenHeight, &history, mono_font);
        draw_core_visualization(screenWidth, screenHeight, hp_state, hp_active_texture, hp_light_sleep_texture, hp_deep_sleep_texture, lp_core_texture);
        draw_bottom_bar(screenWidth, screenHeight, uart_monitor_is_receiving(), mono_font);

        // End drawing
        EndDrawing();

        frame_counter++;

        // Adaptive delay: yield briefly after drawing new data so the next burst is
        // captured promptly, block a little longer when idle to save CPU.
        if (got_any_line) {
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // De-initialize (never reached in embedded mode)
    ESP_LOGI(TAG, "De-initializing UART Monitor...");

    // Unload textures
    if (hp_active_texture.id != 0) {
        UnloadTexture(hp_active_texture);
    }
    if (hp_light_sleep_texture.id != 0) {
        UnloadTexture(hp_light_sleep_texture);
    }
    if (hp_deep_sleep_texture.id != 0) {
        UnloadTexture(hp_deep_sleep_texture);
    }
    if (lp_core_texture.id != 0) {
        UnloadTexture(lp_core_texture);
    }

    if (mono_loaded) {
        UnloadFont(mono_font);
    }

    uart_monitor_deinit();
    CloseWindow();

    // Unmount LittleFS
    esp_vfs_littlefs_unregister(NULL);

    vTaskDelete(NULL);
}

void app_main(void)
{
ESP_LOGI(TAG, "Initializing ESP32-P4 Graphical Terminal UART Monitor...");

    // Reduce verbosity of ray_port flush messages
    esp_log_level_set("ray_port", ESP_LOG_WARN);

    // Initialize display hardware
    esp_err_t ret = board_init_display();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display: %d", ret);
        return;
    }

    // Initialize UART monitor
    ret = uart_monitor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize UART monitor: %d", ret);
        return;
    }

    // Initialize USB keyboard (P4 host port) for full duplex communication
    ret = usb_keyboard_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "USB keyboard init failed (no keyboard detected?)");
    }

    ESP_LOGI(TAG, "Creating UART monitor task with %dKB stack...", RAYLIB_TASK_STACK_SIZE / 1024);

    // Create dedicated task for UART monitor display
    xTaskCreatePinnedToCore(
        uart_monitor_display_task,    // Task function
        "uart_monitor_task",          // Task name
        RAYLIB_TASK_STACK_SIZE,       // Stack size in bytes
        NULL,                         // Parameters
        5,                            // Priority
        NULL,                         // Task handle
        1                             // Core ID (run on core 1)
    );
}
