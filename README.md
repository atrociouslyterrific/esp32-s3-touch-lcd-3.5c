# ESP32-S3-Touch-LCD-3.5-C projects

[![Compile sketches](https://github.com/atrociouslyterrific/esp32-s3-touch-lcd-3.5c/actions/workflows/compile.yml/badge.svg)](https://github.com/atrociouslyterrific/esp32-s3-touch-lcd-3.5c/actions/workflows/compile.yml)

Arduino sketches for the Waveshare **ESP32-S3-Touch-LCD-3.5-C**: an ESP32-S3 (16 MB flash, 8 MB PSRAM) with a 3.5" 320×480 IPS screen and capacitive touch. The -C version also comes with an OV5640 camera.

## Sketches

| Folder | What it does |
|---|---|
| `hello_touch` | Screen and touch test. Shows a title and color bars, prints the touch position, and lets you draw with your finger. Tap a swatch to change the pen color, or **CLEAR** to wipe the screen. |

## Hardware

| Part | Chip | Connection |
|---|---|---|
| Display | ST7796 | SPI: SCLK 5, MOSI 1, MISO 2, DC 3, CS tied low |
| Backlight | | GPIO 6 |
| Touch | FT6336 | I2C: SDA 8, SCL 7 |
| IO expander | TCA9554 (0x20) | I2C. Its pin 1 is the LCD reset |

These pins come from [Waveshare's official examples](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-3.5).

## Arduino IDE setup

1. Install the **esp32** board package by Espressif (version 3.3.6 or newer).
2. Install these libraries with the Library Manager:
   - **GFX Library for Arduino** by Moon On Our Nation (1.6.9 or newer; older versions do not build with ESP32 core 3.3.6+)
   - **SensorLib** by Lewis He (tested with 0.3.1)
   - **TCA9554** by Rob Tillaart (tested with 0.1.2)
3. Choose these settings in the **Tools** menu:

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB |
| PSRAM | OPI PSRAM |
| Flash Mode | QIO |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| USB CDC On Boot | Enabled |

4. Open `hello_touch/hello_touch.ino` and upload it. The Serial Monitor runs at 115200 baud.

If the upload won't start, hold **BOOT**, press **RESET**, release **BOOT**, then upload again.
