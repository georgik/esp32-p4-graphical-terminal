# ESP32-P4 Graphical Terminal

A graphical, **full-duplex** terminal for the ESP32-P4-Function-EV board. It renders an external
UART stream in a scrolling terminal view (Raylib) and, via a USB HID host keyboard, sends
keystrokes back out over UART. This lets you drive a remote Linux box on an **ESP32-S31** chip
over its serial line: read its output on the display and type commands to it.

## Features

- Full-duplex terminal over UART1
  - Reads remote output and shows it in a full-width scrolling terminal view.
  - Types keystrokes from a plugged-in USB keyboard back to the remote device (via `uart_monitor_send`).
- Real-time UART display on 1024x600 LCD
- Console logging of all received messages via serial output
- Configurable UART settings (baud rate, pins, buffer sizes)
- Status indicators (RX count, baud rate, activity status)
- Ring buffer for line history
- LittleFS filesystem for independent asset management

## Hardware Requirements

### Primary Platform
- ESP32-P4-Function-EV Board (1024x600 LCD display)
- USB-C cable for power and programming

### Remote device
- Any UART-capable device, e.g. a Linux box on an **ESP32-S31** chip

### Optional (for the USB keyboard)
- A standard HID keyboard (boot-protocol keyboards work best; a powered USB hub helps for
  high-current keyboards). The P4 host port is powered from the board's USB port, so the board
  must be connected to a powered USB source for the keyboard to be detected.

## Pin Configuration (ESP32-P4-Function-EV)

- UART1_RX: GPIO21
- UART1_TX: GPIO22
- USB host (USB-OTG0): D+ = GPIO20, D- = GPIO19

## Wiring for full-duplex communication

```
ESP32-S31 (Linux box)                ESP32-P4 (Expansion Header)
----------------------------------    --------------------------------
       TX  -------------->  GPIO21 (UART1_RX)
       RX  <--------------  GPIO22 (UART1_TX)
       GND  ---------->     GND
```

The TX/RX/GND trio gives serial bidirectionality. In addition, keystrokes from a USB keyboard
plugged into the P4 host port are sent out on UART1_TX, so you can type commands at the remote
device without any host-side serial tool.

## Software Requirements

- ESP-IDF v6.1 or later
- CMake 3.16 or later
- ImageMagick (for asset conversion)

## Installation and Setup

### Clone the Repository

```bash
git clone <repository-url>
cd esp32-p4-graphical-terminal
```

### Prepare Assets

The project loads PNG icons from LittleFS. Convert the source SVGs to PNG:

```bash
./convert_assets.sh
```

### Build the Project

```bash
idf.py build
```

### Flash to Device

```bash
idf.py flash
```

## Configuration

Configuration options are available via `idf.py menuconfig` under `UART Monitor
Configuration` (the project targets the ESP32-P4-Function-EV board):

| Option | Default | Description |
|--------|---------|-------------|
| Baud Rate | 115200 | UART communication speed |
| RX Pin | 21 | GPIO pin for UART receive |
| TX Pin | 22 | GPIO pin for UART transmit |
| Max Lines | 45 | Maximum lines in history buffer |
| Line Length | 100 | Maximum characters per line |


## Full-Duplex USB Keyboard

`usb_keyboard.c` runs the ESP32-P4 USB host in HID-host mode (matching the proven OpenTyrian
reference). Keystrokes flow: USB keyboard → HID host → ASCII queue → `uart_monitor_send`, which
sends them out on UART1 to the remote device.

- The HID driver callback only queues the connect event; a separate task opens, sets the boot
  protocol, and starts the device.
- Reports are deduplicated vs the previous report, so held keys are not repeated.
- Shift-aware HID usage → ASCII mapping (`hid_key_to_ascii`).

## Asset Management

PNG icons are stored in a 2MB LittleFS partition on flash, allowing independent updates without
reflashing the firmware. The `convert_assets.sh` script rasterizes the source SVGs to PNGs
(loadable at runtime by the display).

## Project Structure

```
.
├── main/
│   ├── hello.c              # Main application and display logic
│   ├── uart_monitor.c       # UART driver implementation
│   ├── uart_monitor.h       # UART monitor API
│   ├── usb_keyboard.c       # USB HID host keyboard (full-duplex I/O)
│   ├── usb_keyboard.h       # USB keyboard API
│   ├── board_init.c         # Display initialization
│   ├── board_init.h         # Board initialization header
│   ├── Kconfig.projbuild    # Configuration menu
│   ├── CMakeLists.txt       # Build configuration
│   └── idf_component.yml    # Component dependencies
├── assets/                  # Source icons
├── convert_assets.sh        # Asset conversion script
├── partitions.csv           # Flash partition table
└── README.md                # This file
```

## Console Output Example

```
I (2075) UARTMonitor: UART Monitor Initialized. Starting main loop...
I (2085) UARTMonitor: Screen dimensions: 1024x600
W (103622) usb_keyboard: report_process: key 'l' enqueued
W (103700) usb_keyboard: report_process: key 's' enqueued
W (103878) usb_keyboard: report_process: key '\n' enqueued
I (104238) UARTMonitor: RX: root@esp32s31:~# uname -a
```

## Troubleshooting

### No data appearing on display

- Check wiring connections (TX to RX, GND to GND)
- Verify baud rate matches the remote device (default: 115200)
- Check that the UART transmitter is actually sending data
- Verify correct GPIO pins for the board

### USB keyboard not detected

- Ensure the board is connected to a powered USB source (the host port is powered from it)
- Plug a standard boot-protocol keyboard into the P4 host port (D+/D- on GPIO20/19)
- Try the keyboard shortly after power-on; hot-plug is not extensively tested

### Garbled or corrupted characters

- Mismatched baud rates - ensure both devices use the same rate
- Noisy signal - check wire quality and length
- Ground loop - ensure common ground connection
- TX and RX reversed - double-check pin connections

## Building

The project targets the ESP32-P4-Function-EV board.

```bash
idf.py build
idf.py flash
```

## Technical Details

### Display Configuration

- ESP32-P4-Function-EV: 1024x600 resolution, 256KB stack for the Raylib task
- RGB565 color format for display

### Memory Usage

- LittleFS partition: 2MB
- Ring buffer: 45-100 lines depending on config

### Performance Considerations

- Pre-rendered PNG assets for fast display
- Minimal CPU overhead for line history
- Efficient ring buffer for line history

## License

MIT License

## Links

- [espressif/esp32_p4_function_ev_board](https://components.espressif.com/components/espressif/esp32_p4_function_ev_board_noglib) - Board support package
- [georgik/esp_raylib_port](https://components.espressif.com/components/georgik/esp_raylib_port) - Raylib port for ESP32
- [raylib](https://www.raylib.com/) - C library for game programming
- [joltwallet/littlefs](https://components.espressif.com/components/joltwallet/littlefs) - LittleFS filesystem component

