# esp32_c6_peripherals_api

Reusable ESP-IDF firmware APIs for ESP32-C6 peripherals, with an integration app in `main/`.

## Included Components

- `dht20_api`: DHT20 temperature/humidity over I2C
- `display_api`: ST7789 display over SPI (with minimal text renderer)
- `display_image`: RGB565 image helpers built on `display_api`
- `knob_api`: rotary encoder (CLK/DT/SW)
- `sntp_api`: SNTP sync + 2-line top status bar renderer
- `wifi_http_api`: HTTP server for Wi-Fi AP/STA configuration

## Repository Layout

- `components/dht20_api/`
- `components/display_api/`
- `components/knob_api/`
- `components/sntp_api/`
- `components/wifi_http_api/`
- `main/`
- `docs/`

## Prerequisites

- ESP-IDF v5.x installed and exported in your shell
- Target: ESP32-C6

## Quick Start

```bash
idf.py set-target esp32c6
idf.py build
idf.py flash monitor
```

## Main App Configuration

All runtime toggles and board mapping are in `main/main.c`.

### Feature Toggles

- `APP_ENABLE_DHT20`
- `APP_ENABLE_DISPLAY`
- `APP_ENABLE_KNOB`
- `APP_ENABLE_RGB_LED`
- `APP_ENABLE_SNTP`
- `APP_ENABLE_WIFI_HTTP`
- `APP_RUN_DISPLAY_BENCHMARK`

### Default Pin Mapping (GPIO numbers)

- DHT20: `SDA=6`, `SCL=7`
- TFT ST7789: `SCK=2`, `MOSI=3`, `CS=10`, `DC=11`, `RST=4`, `BLK=5`
- Encoder: `CLK=21`, `DT=9`, `SW=20`
- RGB LED data: `GPIO8`

### Display Orientation and Geometry

- `TFT_WIDTH=170`
- `TFT_HEIGHT=320`
- `DISPLAY_ROTATION=DISPLAY_ROTATION_0` (portrait)
- `DISPLAY_X_OFFSET`, `DISPLAY_Y_OFFSET` for panel alignment

### SNTP Status Bar Tuning Macros

`main/main.c` exposes layout and style controls:

- `SNTP_BAR_BG_COLOR`, `SNTP_BAR_FG_COLOR`
- `SNTP_BASE_TEXT_SCALE`
- `SNTP_DATE_TEXT_SCALE`
- `SNTP_TIME_TEXT_SCALE`
- `SNTP_LINE_GAP_PX`
- `SNTP_DATE_CHAR_SPACING_PX`
- `SNTP_TIME_CHAR_SPACING_PX`

Status text format:

- Line 1: `GMTxx DD.MM.YYYY`
- Line 2: centered `HH:MM`

## Wi-Fi HTTP Config UI

When `APP_ENABLE_WIFI_HTTP=1`, the device starts an AP and config page.

- Default AP SSID: `ESP32C6-Setup`
- Default AP password: `12345678`
- UI URL: `http://192.168.4.1/`

Key API routes:

- `GET /api/status`
- `GET /api/scan`
- `POST /api/mode`
- `POST /api/ap`
- `POST /api/sta`
- `POST /api/sta/disconnect`

More details:

- `components/wifi_http_api/README.md`
- `components/sntp_api/README.md`

## License

This project is released under `0BSD` (see `LICENSE`).
