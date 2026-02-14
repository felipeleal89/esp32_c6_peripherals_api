/*
 * SPDX-License-Identifier: 0BSD
 */

#include "display_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DISPLAY_SPI_HOST SPI2_HOST
#define DISPLAY_SPI_MODE 0
#define DISPLAY_CMD_BITS 8
#define DISPLAY_PARAM_BITS 8
#define DISPLAY_PIXEL_BITS 16

#define DISPLAY_BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define DISPLAY_BL_LEDC_TIMER LEDC_TIMER_0
#define DISPLAY_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define DISPLAY_BL_LEDC_DUTY_RES LEDC_TIMER_13_BIT
#define DISPLAY_BL_LEDC_FREQ_HZ 5000
#define DISPLAY_BL_MAX_DUTY ((1U << DISPLAY_BL_LEDC_DUTY_RES) - 1U)
#define DISPLAY_LINE_BUF_PIXELS_MAX 320U

#define DISPLAY_COLOR_BLACK 0x0000
#define DISPLAY_COLOR_WHITE 0xFFFF
#define DISPLAY_COLOR_RED 0xF800
#define DISPLAY_COLOR_GREEN 0x07E0
#define DISPLAY_COLOR_BLUE 0x001F
#define DISPLAY_COLOR_YELLOW 0xFFE0

static const char *TAG = "display_api";
static uint16_t s_line_buf[DISPLAY_LINE_BUF_PIXELS_MAX];

#define DISPLAY_LOGI(format, ...) ESP_LOGI(TAG, format, ##__VA_ARGS__)
#define DISPLAY_LOGW(format, ...) ESP_LOGW(TAG, format, ##__VA_ARGS__)

typedef struct {
    bool initialized;
    display_pins_t pins;
    display_cfg_t cfg;
    uint8_t rotation;
    int active_width;
    int active_height;
    int active_x_offset;
    int active_y_offset;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    uint16_t *line_buf;
} display_ctx_t;

static display_ctx_t g_disp = {0};

static int max_i32(const int a, const int b)
{
    return (a > b) ? a : b;
}

static uint8_t clamp_percent(uint8_t percent)
{
    return (percent > 100U) ? 100U : percent;
}

static int clampi(int value, int lo, int hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static void fill_line_buf(uint16_t color, size_t pixels)
{
    for (size_t i = 0; i < pixels; i++) {
        g_disp.line_buf[i] = color;
    }
}

static void calc_viewport_for_rotation(uint8_t rotation, int *width, int *height, int *x_offset, int *y_offset)
{
    const uint8_t rot = rotation % 4U;
    if (rot == DISPLAY_ROTATION_90 || rot == DISPLAY_ROTATION_270) {
        *width = g_disp.cfg.height;
        *height = g_disp.cfg.width;
        *x_offset = g_disp.cfg.y_offset;
        *y_offset = g_disp.cfg.x_offset;
    } else {
        *width = g_disp.cfg.width;
        *height = g_disp.cfg.height;
        *x_offset = g_disp.cfg.x_offset;
        *y_offset = g_disp.cfg.y_offset;
    }
}

static esp_err_t configure_backlight(gpio_num_t bl_pin)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = DISPLAY_BL_LEDC_MODE,
        .timer_num = DISPLAY_BL_LEDC_TIMER,
        .duty_resolution = DISPLAY_BL_LEDC_DUTY_RES,
        .freq_hz = DISPLAY_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc_timer_config failed");

    ledc_channel_config_t ch_cfg = {
        .gpio_num = bl_pin,
        .speed_mode = DISPLAY_BL_LEDC_MODE,
        .channel = DISPLAY_BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = DISPLAY_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags = {.output_invert = 0},
    };
    return ledc_channel_config(&ch_cfg);
}

static const uint8_t *glyph_for_char(char c)
{
    static const uint8_t space[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t pct[5] = {0x62, 0x64, 0x08, 0x13, 0x23};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};

    static const uint8_t n0[5] = {0x3E, 0x51, 0x49, 0x45, 0x3E};
    static const uint8_t n1[5] = {0x00, 0x42, 0x7F, 0x40, 0x00};
    static const uint8_t n2[5] = {0x62, 0x51, 0x49, 0x49, 0x46};
    static const uint8_t n3[5] = {0x22, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t n4[5] = {0x18, 0x14, 0x12, 0x7F, 0x10};
    static const uint8_t n5[5] = {0x2F, 0x49, 0x49, 0x49, 0x31};
    static const uint8_t n6[5] = {0x3E, 0x49, 0x49, 0x49, 0x32};
    static const uint8_t n7[5] = {0x01, 0x01, 0x79, 0x05, 0x03};
    static const uint8_t n8[5] = {0x36, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t n9[5] = {0x26, 0x49, 0x49, 0x49, 0x3E};

    static const uint8_t C_[5] = {0x3E, 0x41, 0x41, 0x41, 0x22};
    static const uint8_t H_[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
    static const uint8_t M_[5] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};
    static const uint8_t P_[5] = {0x7F, 0x09, 0x09, 0x09, 0x06};
    static const uint8_t R_[5] = {0x7F, 0x09, 0x19, 0x29, 0x46};
    static const uint8_t T_[5] = {0x01, 0x01, 0x7F, 0x01, 0x01};
    static const uint8_t E_[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};

    switch (c) {
        case ' ': return space;
        case ':': return colon;
        case '.': return dot;
        case '%': return pct;
        case '-': return dash;
        case '0': return n0;
        case '1': return n1;
        case '2': return n2;
        case '3': return n3;
        case '4': return n4;
        case '5': return n5;
        case '6': return n6;
        case '7': return n7;
        case '8': return n8;
        case '9': return n9;
        case 'C': return C_;
        case 'H': return H_;
        case 'M': return M_;
        case 'P': return P_;
        case 'R': return R_;
        case 'T': return T_;
        case 'E': return E_;
        default: return space;
    }
}

esp_err_t display_init(const display_pins_t *pins, const display_cfg_t *cfg)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(pins != NULL, ESP_ERR_INVALID_ARG, TAG, "pins is null");
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is null");
    ESP_RETURN_ON_FALSE(cfg->width > 0 && cfg->height > 0, ESP_ERR_INVALID_ARG, TAG, "invalid geometry");
    ESP_RETURN_ON_FALSE(max_i32(cfg->width, cfg->height) <= (int)DISPLAY_LINE_BUF_PIXELS_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "geometry exceeds line buffer");
    ESP_RETURN_ON_FALSE(cfg->spi_clock_hz > 0, ESP_ERR_INVALID_ARG, TAG, "invalid spi clock");

    if (g_disp.initialized) {
        DISPLAY_LOGW("display already initialized");
        return ESP_OK;
    }

    g_disp.pins = *pins;
    g_disp.cfg = *cfg;

    spi_bus_config_t buscfg = {
        .sclk_io_num = pins->sck,
        .mosi_io_num = pins->mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = cfg->width * 40 * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .intr_flags = 0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize failed");

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = pins->dc,
        .cs_gpio_num = pins->cs,
        .pclk_hz = cfg->spi_clock_hz,
        .lcd_cmd_bits = DISPLAY_CMD_BITS,
        .lcd_param_bits = DISPLAY_PARAM_BITS,
        .spi_mode = DISPLAY_SPI_MODE,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_config, &g_disp.io), err, TAG, "new_panel_io failed");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = pins->reset,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = DISPLAY_PIXEL_BITS,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(g_disp.io, &panel_cfg, &g_disp.panel), err, TAG, "new_panel_st7789 failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(g_disp.panel), err, TAG, "panel_reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(g_disp.panel), err, TAG, "panel_init failed");

    ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(g_disp.panel, cfg->x_offset, cfg->y_offset), err, TAG, "set_gap failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(g_disp.panel, true), err, TAG, "invert_color failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(g_disp.panel, true), err, TAG, "disp_on failed");

    g_disp.line_buf = s_line_buf;

    g_disp.initialized = true;
    ESP_GOTO_ON_ERROR(configure_backlight(pins->backlight), err, TAG, "configure_backlight failed");
    display_backlight_set(80);
    display_set_rotation(0);

    DISPLAY_LOGI("initialized %dx%d @ %d Hz", cfg->width, cfg->height, cfg->spi_clock_hz);
    return ESP_OK;

err:
    g_disp.initialized = false;
    g_disp.line_buf = NULL;
    if (g_disp.panel != NULL) {
        esp_lcd_panel_del(g_disp.panel);
        g_disp.panel = NULL;
    }
    if (g_disp.io != NULL) {
        esp_lcd_panel_io_del(g_disp.io);
        g_disp.io = NULL;
    }
    spi_bus_free(DISPLAY_SPI_HOST);
    return ret;
}

void display_set_rotation(uint8_t rotation)
{
    if (!g_disp.initialized) {
        return;
    }

    const uint8_t rot = rotation % 4U;
    g_disp.rotation = rot;
    switch (rot) {
        case 0:
            esp_lcd_panel_swap_xy(g_disp.panel, false);
            esp_lcd_panel_mirror(g_disp.panel, false, false);
            break;
        case 1:
            esp_lcd_panel_swap_xy(g_disp.panel, true);
            esp_lcd_panel_mirror(g_disp.panel, true, false);
            break;
        case 2:
            esp_lcd_panel_swap_xy(g_disp.panel, false);
            esp_lcd_panel_mirror(g_disp.panel, true, true);
            break;
        case 3:
            esp_lcd_panel_swap_xy(g_disp.panel, true);
            esp_lcd_panel_mirror(g_disp.panel, false, true);
            break;
        default:
            break;
    }
    calc_viewport_for_rotation(rot, &g_disp.active_width, &g_disp.active_height, &g_disp.active_x_offset, &g_disp.active_y_offset);
    esp_lcd_panel_set_gap(g_disp.panel, g_disp.active_x_offset, g_disp.active_y_offset);
}

int display_get_width(void)
{
    if (!g_disp.initialized) {
        return 0;
    }
    return g_disp.active_width;
}

int display_get_height(void)
{
    if (!g_disp.initialized) {
        return 0;
    }
    return g_disp.active_height;
}

esp_lcd_panel_handle_t display_get_panel_handle(void)
{
    if (!g_disp.initialized) {
        return NULL;
    }
    return g_disp.panel;
}

void display_backlight_set(uint8_t percent)
{
    if (!g_disp.initialized) {
        return;
    }

    const uint8_t p = clamp_percent(percent);
    const uint32_t duty = (DISPLAY_BL_MAX_DUTY * p) / 100U;
    ledc_set_duty(DISPLAY_BL_LEDC_MODE, DISPLAY_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(DISPLAY_BL_LEDC_MODE, DISPLAY_BL_LEDC_CHANNEL);
}

void display_draw_rect(int x, int y, int w, int h, uint16_t rgb565)
{
    if (!g_disp.initialized || w <= 0 || h <= 0) {
        return;
    }

    int x0 = clampi(x, 0, g_disp.active_width);
    int y0 = clampi(y, 0, g_disp.active_height);
    int x1 = clampi(x + w, 0, g_disp.active_width);
    int y1 = clampi(y + h, 0, g_disp.active_height);

    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    const int draw_w = x1 - x0;
    fill_line_buf(rgb565, (size_t)draw_w);

    for (int row = y0; row < y1; row++) {
        esp_lcd_panel_draw_bitmap(g_disp.panel, x0, row, x1, row + 1, g_disp.line_buf);
    }
}

void display_fill_color(uint16_t rgb565)
{
    display_draw_rect(0, 0, g_disp.active_width, g_disp.active_height, rgb565);
}

void display_draw_text_minimal(int x, int y, const char *s, uint16_t rgb565)
{
    if (!g_disp.initialized || s == NULL) {
        return;
    }

    int cursor_x = x;
    const int cursor_y = y;
    while (*s != '\0') {
        const uint8_t *glyph = glyph_for_char(*s);

        for (int col = 0; col < 5; col++) {
            uint8_t col_bits = glyph[col];
            for (int row = 0; row < 7; row++) {
                if (col_bits & (1U << row)) {
                    display_draw_rect(cursor_x + col, cursor_y + row, 1, 1, rgb565);
                }
            }
        }

        cursor_x += 6;
        s++;
    }
}

void display_self_test(void)
{
    if (!g_disp.initialized) {
        return;
    }

    display_backlight_set(100);

    display_fill_color(DISPLAY_COLOR_RED);
    vTaskDelay(pdMS_TO_TICKS(180));
    display_fill_color(DISPLAY_COLOR_GREEN);
    vTaskDelay(pdMS_TO_TICKS(180));
    display_fill_color(DISPLAY_COLOR_BLUE);
    vTaskDelay(pdMS_TO_TICKS(180));
    display_fill_color(DISPLAY_COLOR_WHITE);
    vTaskDelay(pdMS_TO_TICKS(180));
    display_fill_color(DISPLAY_COLOR_BLACK);

    display_draw_rect(20, 30, max_i32(30, g_disp.active_width / 3), max_i32(30, g_disp.active_height / 4), DISPLAY_COLOR_YELLOW);
    display_draw_text_minimal(12, 12, "TEMP", DISPLAY_COLOR_WHITE);
    display_draw_text_minimal(12, 24, "RH", DISPLAY_COLOR_WHITE);
}
