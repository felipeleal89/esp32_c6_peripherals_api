/*
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

/**
 * @file display_api.h
 * @brief Reusable ST7789 display API for ESP-IDF.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Display module status code alias. */
typedef esp_err_t display_status_t;

/** @brief Physical display pin mapping. */
typedef struct {
    gpio_num_t sck;
    gpio_num_t mosi;
    gpio_num_t cs;
    gpio_num_t dc;
    gpio_num_t reset;
    gpio_num_t backlight;
} display_pins_t;

/** @brief Static display configuration. */
typedef struct {
    int width;
    int height;
    int x_offset;
    int y_offset;
    int spi_clock_hz;
} display_cfg_t;

#define DISPLAY_ROTATION_0 0U
#define DISPLAY_ROTATION_90 1U
#define DISPLAY_ROTATION_180 2U
#define DISPLAY_ROTATION_270 3U

/** @brief Initialize panel, SPI I/O and backlight control. */
display_status_t display_init(const display_pins_t *pins, const display_cfg_t *cfg);
/** @brief Apply rotation (0/90/180/270). */
void display_set_rotation(uint8_t rotation);
/** @brief Get active display width after rotation. */
int display_get_width(void);
/** @brief Get active display height after rotation. */
int display_get_height(void);
/** @brief Get the initialized ESP-IDF panel handle. */
esp_lcd_panel_handle_t display_get_panel_handle(void);
/** @brief Set backlight brightness in percent [0..100]. */
void display_backlight_set(uint8_t percent);
/** @brief Fill full active display with one color. */
void display_fill_color(uint16_t rgb565);
/** @brief Draw a filled rectangle clipped to active display area. */
void display_draw_rect(int x, int y, int w, int h, uint16_t rgb565);
/** @brief Draw minimal built-in monochrome text. */
void display_draw_text_minimal(int x, int y, const char *s, uint16_t rgb565);
/**
 * @brief Draw minimal built-in monochrome text with integer scale factor.
 * @param scale Pixel scale for each glyph dot (1 = original 5x7 font).
 */
void display_draw_text_minimal_scaled(int x, int y, const char *s, uint16_t rgb565, uint8_t scale);
/** @brief Run basic panel self-test visuals. */
void display_self_test(void);

#ifdef __cplusplus
}
#endif
