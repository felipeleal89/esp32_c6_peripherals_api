/*
 * SPDX-License-Identifier: 0BSD
 */

#include "display_image.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_heap_caps.h"

#define RGB565_BLACK 0x0000U
#define RGB565_WHITE 0xFFFFU
#define RGB565_RED 0xF800U
#define RGB565_GREEN 0x07E0U
#define RGB565_BLUE 0x001FU
#define RGB565_YELLOW 0xFFE0U
#define RGB565_CYAN 0x07FFU
#define RGB565_MAGENTA 0xF81FU

static const uint16_t s_color_bars[8] = {
    RGB565_BLACK,
    RGB565_WHITE,
    RGB565_RED,
    RGB565_GREEN,
    RGB565_BLUE,
    RGB565_YELLOW,
    RGB565_CYAN,
    RGB565_MAGENTA,
};

static bool display_image_ctx_valid(const display_image_t *ctx)
{
    return (ctx != NULL) && (ctx->panel != NULL) && (ctx->width > 0U) && (ctx->height > 0U);
}

void display_image_init(display_image_t *ctx, esp_lcd_panel_handle_t panel, uint16_t w, uint16_t h)
{
    if (ctx == NULL) {
        return;
    }

    ctx->panel = panel;
    ctx->width = w;
    ctx->height = h;
}

esp_err_t display_image_draw_full_rgb565(display_image_t *ctx, const uint16_t *img_rgb565, size_t pixels)
{
    if (!display_image_ctx_valid(ctx) || img_rgb565 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t expected_pixels = (size_t)ctx->width * (size_t)ctx->height;
    if (pixels < expected_pixels) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->width, ctx->height, img_rgb565);
}

esp_err_t display_image_draw_rect_rgb565(display_image_t *ctx,
                                         int x,
                                         int y,
                                         int w,
                                         int h,
                                         const uint16_t *img_rgb565,
                                         size_t pixels)
{
    if (!display_image_ctx_valid(ctx) || img_rgb565 == NULL || w <= 0 || h <= 0 || x < 0 || y < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((x + w) > (int)ctx->width || (y + h) > (int)ctx->height) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t expected_pixels = (size_t)w * (size_t)h;
    if (pixels < expected_pixels) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_lcd_panel_draw_bitmap(ctx->panel, x, y, x + w, y + h, img_rgb565);
}

esp_err_t display_image_draw_test_pattern_streaming(display_image_t *ctx, int block_rows)
{
    if (!display_image_ctx_valid(ctx) || block_rows <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t rows = (size_t)block_rows;
    const size_t width = (size_t)ctx->width;
    const size_t alloc_pixels = width * rows;

    uint16_t *block_buf = heap_caps_malloc(alloc_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (block_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (int y = 0; y < (int)ctx->height; y += block_rows) {
        const int cur_rows = (((int)ctx->height - y) < block_rows) ? ((int)ctx->height - y) : block_rows;

        for (int r = 0; r < cur_rows; r++) {
            for (int x = 0; x < (int)ctx->width; x++) {
                const int bar = (x * 8) / (int)ctx->width;
                const int idx = (bar < 0) ? 0 : ((bar > 7) ? 7 : bar);
                block_buf[(size_t)r * width + (size_t)x] = s_color_bars[idx];
            }
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, y, ctx->width, y + cur_rows, block_buf);
        if (err != ESP_OK) {
            free(block_buf);
            return err;
        }
    }

    free(block_buf);
    return ESP_OK;
}
