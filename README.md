# esp32_c6_peripherals_api

Reusable ESP-IDF firmware APIs for ESP32-C6 peripherals, plus a small example app in `main/`.

## What Is Included

- `dht20_api`: DHT20 temperature/humidity sensor over I2C
- `display_api`: ST7789 display over SPI
- `display_image`: RGB565 image drawing utilities on top of `display_api`
- `knob_api`: rotary encoder API (CLK/DT/SW)

## Repository Layout

- `components/dht20_api/`
- `components/display_api/`
- `components/knob_api/`
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

## Configuration

Main feature toggles are in `main/main.c`:

- `APP_ENABLE_DHT20`
- `APP_ENABLE_DISPLAY`
- `APP_ENABLE_KNOB`
- `APP_ENABLE_RGB_LED`

Display text presentation is also configured in `main/main.c`:

- `DISPLAY_TEXT_SCALE`: text scale factor (base font is 5x7)
- `DISPLAY_TEXT_LINE_GAP`: vertical spacing between lines
- `DISPLAY_TEXT_COLOR`: RGB565 value used for text

Notes:

- The app centers temperature/humidity text on screen.
- Some ST7789 panels use different channel order. If the on-screen color is not the expected cyan, tune `DISPLAY_TEXT_COLOR`.

## Engineering Principles

- Keep behavior stable and predictable
- Keep modules isolated and reusable
- Avoid unnecessary runtime allocation
- Use explicit error handling (`esp_err_t`)

## License

This project is released under `0BSD` (see `LICENSE`).
