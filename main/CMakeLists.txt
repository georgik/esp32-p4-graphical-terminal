idf_component_register(
    SRCS
        "hello.c"
        "board_init.c"
        "uart_monitor.c"
        "usb_keyboard.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        raylib
        esp_lcd
        esp_driver_uart
        littlefs
        # USB HID host driver for keyboard input
        usb_host_hid
)

# Create LittleFS partition image from assets directory
# The directory will be created by convert_assets.sh before building
littlefs_create_partition_image(storage "${CMAKE_CURRENT_SOURCE_DIR}/../assets" FLASH_IN_PROJECT)
