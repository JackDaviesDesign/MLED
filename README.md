# MLED - Matter-over-Thread CCT LED Controller

A Matter-compatible dual-channel CCT (warm white / cool white) controller for ESP32-C6 that works over Thread networking. Drive 0-10V dimmer modules from Home Assistant, Apple Home, Google Home, or any Matter-compatible smart home platform.

> **Disclaimer:** This entire firmware was written by AI ([Claude](https://claude.ai) by Anthropic). I ([@JackDaviesDesign](https://github.com/JackDaviesDesign)) have not read or written a single line of code — I only provided direction, tested on real hardware, and deployed. Use at your own risk.

## Features

- **Matter over Thread** - Native Matter protocol, no cloud or WiFi required
- **Dual CCT control** - Warm white + cool white channels with colour temperature slider in your smart home app
- **0-10V PWM output** - Two independent PWM channels for industry-standard 0-10V dimmer modules
- **Smooth transitions** - 300ms fades on all changes
- **Thread mesh networking** - Self-healing network, device acts as a router
- **Web-based installer** - Flash firmware directly from your browser
- **USB configuration** - Change settings via serial without recompiling
- **NVS persistence** - Settings survive reboots
- **Temperature monitoring** - Chip temperature exposed to Home Assistant
- **Health monitoring** - Watchdog timer, heap tracking, auto-reboot on hang
- **Power-on behavior** - Configurable: restore last state, always on, or always off

## Hardware

### What You Need

- **ESP32-C6 board** - Seeed Studio XIAO ESP32-C6 or DFRobot Beetle ESP32-C6 (any ESP32-C6 works)
- **0-10V PWM dimmer modules** - One per channel (warm white + cool white). These convert the ESP32's 3.3V PWM signal to the 0-10V control voltage your LED driver expects
- **Mains-dimmable LED driver** - With 0-10V dimming input (e.g. DALI-capable or 0-10V drivers for panel lights, downlights, etc.)
- **Thread border router** - HomePod Mini, Apple TV 4K, Google Nest Hub, or dedicated like SLZB-06/SMLight

### Wiring

```
ESP32-C6                  0-10V Dimmer Module
─────────                 ──────────────────────
GPIO 22 (D4) ──────────── PWM input (warm white channel)
GPIO 23 (D5) ──────────── PWM input (cool white channel)
GND          ──────────── GND
3.3V         ──────────── VCC (if module needs logic supply)

0-10V Dimmer Module       LED Driver
──────────────────────    ──────────
0-10V output  ─────────── 0-10V dim input
```

> **Note:** GPIO numbers above are for the Seeed XIAO ESP32-C6. The silkscreen labels (D4, D5) do **not** match the GPIO numbers 1:1 — see [Pin Mapping](#seeed-xiao-esp32-c6-pin-mapping) below.

### Seeed XIAO ESP32-C6 Pin Mapping

The "D" labels printed on the XIAO board do **not** match GPIO numbers directly. Verified against the official Seeed pin diagram:

| Silkscreen | GPIO | Notes |
|------------|------|-------|
| D4 | GPIO22 | Warm white PWM (default) |
| D5 | GPIO23 | Cool white PWM (default) |
| D6 | GPIO16 | UART TX — avoid for PWM |
| D7 | GPIO17 | UART RX — avoid for PWM |

GPIO6 and GPIO7 are **not exposed** on the XIAO header at all. An earlier revision of this firmware defaulted to GPIO6/7, which silently drove non-existent pins while D4/D5 stayed at 0V. If your dimmer shows no voltage change, double-check you're using GPIO22/23.

## Quick Start

### 1. Flash the Firmware

Visit the **[Web Installer](https://JackDaviesDesign.github.io/MLED)** and click "Install MLED Firmware".

> **Note:** Requires Chrome or Edge browser. If prompted, hold the BOOT button on your ESP32-C6 while clicking Install.

### 2. Configure GPIO Pins and CCT Range

1. Go to the **Configure** tab in the web installer
2. Click **Connect to Device**
3. Set your warm white and cool white GPIO pins to match your wiring
4. Adjust colour temperature range if needed (default: 154–370 mireds = 6500K–2700K)
5. Click **Save & Reboot**

### 3. Commission to Your Smart Home

After reboot, a QR code will appear in the web installer. Scan it with:
- **Home Assistant** - Settings → Devices & Services → Add Integration → Matter
- **Apple Home** - Add Accessory → Scan QR Code
- **Google Home** - Add Device → Matter-enabled device

The device appears as a **colour temperature light** with a warm/cool slider and brightness control.

## Configuration Options

| Setting | Default | Description |
|---------|---------|-------------|
| Warm White GPIO | 22 | PWM output pin for warm white 0-10V module (D4 on XIAO) |
| Cool White GPIO | 23 | PWM output pin for cool white 0-10V module (D5 on XIAO) |
| Min Colour Temp | 154 | Coolest end in mireds (154 = 6500K daylight) |
| Max Colour Temp | 370 | Warmest end in mireds (370 = 2700K warm white) |
| Max Brightness | 255 | Caps PWM duty cycle — lower to limit output |
| PWM Frequency | 1000 Hz | Carrier frequency for dimmer modules |
| Device Name | MLED | Name shown in your smart home app |
| Power-on | restore | Behaviour on power up: restore last state, on, or off |

## Serial Commands

Connect via USB and use the serial console in the web installer, or any terminal at 115200 baud:

```
help                     Show available commands
config                   Show current configuration
set ww_gpio <n>          Warm white PWM GPIO pin
set cw_gpio <n>          Cool white PWM GPIO pin
set pwm_freq <n>         PWM frequency in Hz (100-20000)
set cct_min <n>          Min colour temp in mireds (coolest, e.g. 154 = 6500K)
set cct_max <n>          Max colour temp in mireds (warmest, e.g. 370 = 2700K)
set brightness <1-255>   Cap PWM duty cycle
set name <name>          Set device name
set poweron <mode>       Power-on behaviour (restore/on/off)
save                     Save configuration and reboot
reboot                   Restart device
factory                  Factory reset (erases settings & commissioning)
gpio_test <n>            Toggle GPIO n HIGH/LOW every 1s for 10s (hardware verification)
gpio_sweep               Cycle through candidate GPIOs to identify physical pins
```

### Verifying Pin Wiring

If you're not sure which GPIO corresponds to a physical pin on your board, use the diagnostic commands:

```
gpio_test 22    # Toggles GPIO22 HIGH/LOW for 10s — probe D4 with a meter
gpio_test 23    # Toggles GPIO23 HIGH/LOW for 10s — probe D5 with a meter
gpio_sweep      # Cycles through all candidate GPIOs, 4s each
```

These drive pins directly via `gpio_set_level`, bypassing LEDC/PWM, so you can verify the mapping with just a multimeter without needing the full 0-10V dimmer circuit.

## Building from Source

### Prerequisites

- [ESP-IDF v5.4+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/get-started/)
- [ESP-Matter](https://github.com/espressif/esp-matter)

### Build & Flash

```bash
# Source the environments
source ~/esp/esp-idf/export.sh
source ~/esp/esp-matter/export.sh

# Build
idf.py build

# Flash (keeps commissioning data)
idf.py -p /dev/ttyACM0 flash

# Flash with erase (clears commissioning - need to re-pair)
idf.py -p /dev/ttyACM0 erase-flash flash
```

### Configuration via menuconfig

```bash
idf.py menuconfig
# Navigate to "MLED Configuration" for build-time defaults
```

## Troubleshooting

### Can't flash the device
- Use Chrome or Edge (Firefox doesn't support Web Serial)
- Hold the BOOT button while clicking Install
- Try a different USB cable (some are charge-only)

### Boot loop / "No bootable app"
- Flash was interrupted — try flashing again

### Can't find device during commissioning
- Commission within 30 seconds of boot (BLE advertising slows down)
- Run `factory` command if device was previously commissioned
- Move closer to your Thread border router

### Dimmer shows no voltage change
- Check GPIO pin numbers match your wiring — see [Pin Mapping](#seeed-xiao-esp32-c6-pin-mapping)
- Run `gpio_test 22` to verify the correct physical pin with a multimeter
- Confirm the 0-10V module is powered independently
- Run `config` to see what GPIO pins the firmware is currently using

### Device doesn't appear in Home Assistant
- Ensure your Thread border router is active
- Try `factory` reset and re-commission if device was already paired

## Project Structure

```
MLED/
├── main/
│   ├── app_main.cpp            # Matter setup, endpoint creation
│   ├── app_driver.cpp          # PWM CCT driver, transitions
│   ├── app_nvs_config.cpp      # Runtime configuration storage
│   ├── app_serial_config.cpp   # USB serial command interface
│   ├── app_monitoring.cpp      # Health monitoring, watchdog, temperature
│   ├── app_device_info.cpp     # Matter device branding
│   ├── app_ble_config.cpp      # BLE commissioning configuration
│   └── Kconfig.projbuild       # Build-time configuration options
├── web-installer/
│   ├── index.html              # Web installer & configurator
│   └── manifest.json           # ESP Web Tools manifest
├── partitions.csv              # Flash partition layout
└── sdkconfig.defaults          # Default SDK configuration
```

## License

MIT License - see [LICENSE](LICENSE) for details.

## Acknowledgments

- [ESP-Matter](https://github.com/espressif/esp-matter) - Espressif's Matter SDK
- [ESP Web Tools](https://esphome.github.io/esp-web-tools/) - Browser-based flashing
- [ConnectedHomeIP](https://github.com/project-chip/connectedhomeip) - Matter protocol implementation
