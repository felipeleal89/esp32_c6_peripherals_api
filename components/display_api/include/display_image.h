/*
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_lcd_panel_handle_t panel;
    uint16_t width;
    uint16_t height;
} display_image_t;

void display_image_init(display_image_t *ctx, esp_lcd_panel_handle_t panel, uint16_t w, uint16_t h);
esp_err_t display_image_draw_full_rgb565(display_image_t *ctx, const uint16_t *img_rgb565, size_t pixels);
esp_err_t display_image_draw_rect_rgb565(display_image_t *ctx, int x, int y, int w, int h, const uint16_t *img_rgb565, size_t pixels);
esp_err_t display_image_draw_test_pattern_streaming(display_image_t *ctx, int block_rows);

#ifdef __cplusplus
}
#endif
