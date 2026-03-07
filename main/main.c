/*
 * SPDX-License-Identifier: 0BSD
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dht20_api.h"
#include "display_api.h"
#include "display_image.h"
#include "knob_api.h"
#include "sntp_api.h"
#include "wifi_http_api.h"
#include "driver/i2c.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define APP_ENABLE_DHT20 0
#define APP_ENABLE_DISPLAY 1
#define APP_ENABLE_KNOB 0
#define APP_ENABLE_RGB_LED 0
#define APP_ENABLE_SNTP 1
#define APP_ENABLE_WIFI_HTTP 1
#define APP_RUN_DISPLAY_BENCHMARK 0

#if (APP_ENABLE_DHT20 != 0) && (APP_ENABLE_DHT20 != 1)
#error "APP_ENABLE_DHT20 must be 0 or 1"
#endif

#if (APP_ENABLE_DISPLAY != 0) && (APP_ENABLE_DISPLAY != 1)
#error "APP_ENABLE_DISPLAY must be 0 or 1"
#endif

#if (APP_ENABLE_KNOB != 0) && (APP_ENABLE_KNOB != 1)
#error "APP_ENABLE_KNOB must be 0 or 1"
#endif

#if (APP_ENABLE_RGB_LED != 0) && (APP_ENABLE_RGB_LED != 1)
#error "APP_ENABLE_RGB_LED must be 0 or 1"
#endif

#if (APP_ENABLE_SNTP != 0) && (APP_ENABLE_SNTP != 1)
#error "APP_ENABLE_SNTP must be 0 or 1"
#endif

#if (APP_ENABLE_WIFI_HTTP != 0) && (APP_ENABLE_WIFI_HTTP != 1)
#error "APP_ENABLE_WIFI_HTTP must be 0 or 1"
#endif

#if (APP_RUN_DISPLAY_BENCHMARK != 0) && (APP_RUN_DISPLAY_BENCHMARK != 1)
#error "APP_RUN_DISPLAY_BENCHMARK must be 0 or 1"
#endif

#define DHT20_I2C_PORT I2C_NUM_0
#define DHT20_I2C_SDA_GPIO GPIO_NUM_6
#define DHT20_I2C_SCL_GPIO GPIO_NUM_7
#define DHT20_I2C_FREQ_HZ 400000

#define DHT20_I2C_TIMEOUT_MS 20
#define DHT20_READY_TIMEOUT_MS 120
#define DHT20_POLL_INTERVAL_MS 2
#define DHT20_PRINT_PERIOD_MS 2000

#define DHT20_TEMP_OFFSET_C 0.0f
#define DHT20_HUM_OFFSET_RH 0.0f

#define TFT_WIDTH 170
#define TFT_HEIGHT 320
#define DISPLAY_X_OFFSET 35
#define DISPLAY_Y_OFFSET 0
#define DISPLAY_SPI_CLOCK_HZ (26 * 1000 * 1000)
#define DISPLAY_ROTATION DISPLAY_ROTATION_0
#define DISPLAY_TEXT_SCALE 2
/*
 * Board/panel-specific color mapping:
 * on this hardware, RGB565 0xF81F is rendered as cyan on screen.
 * If your panel uses a different channel order, tune this constant.
 */
#define DISPLAY_TEXT_COLOR 0xFFFF
#define DISPLAY_TEXT_LINE_GAP (8 * DISPLAY_TEXT_SCALE)

#define SNTP_SERVER_NAME SNTP_API_DEFAULT_SERVER
#define SNTP_GMT_OFFSET_HOURS 0
#define SNTP_SYNC_INTERVAL_MS (60U * 60U * 1000U)
#define SNTP_STATUS_REFRESH_MS 1000U
#define SNTP_BAR_BG_COLOR 0x0000
#define SNTP_BAR_FG_COLOR 0xFFFF
#define SNTP_BASE_TEXT_SCALE 4U
/* 0 = auto (date: base+1, time: date*2) */
#define SNTP_DATE_TEXT_SCALE 3U
#define SNTP_TIME_TEXT_SCALE 4U
/* 0 = auto gap */
#define SNTP_LINE_GAP_PX 20U
/* extra pixels between characters */
#define SNTP_DATE_CHAR_SPACING_PX 3U
#define SNTP_TIME_CHAR_SPACING_PX 5U

#define WIFI_HTTP_AP_SSID WIFI_HTTP_API_DEFAULT_AP_SSID
#define WIFI_HTTP_AP_PASS WIFI_HTTP_API_DEFAULT_AP_PASS
#define WIFI_HTTP_AP_CHANNEL 1

/* Safe defaults for ESP32-C6: avoid GPIO6/GPIO7 (reserved here for DHT20). */
#define TFT_PIN_SCK GPIO_NUM_2
#define TFT_PIN_MOSI GPIO_NUM_3
#define TFT_PIN_CS GPIO_NUM_10
#define TFT_PIN_DC GPIO_NUM_11
#define TFT_PIN_RST GPIO_NUM_4
#define TFT_PIN_BLK GPIO_NUM_5

/* Rotary encoder pins: CLK, DT, SW */
#define KNOB_PIN_CLK GPIO_NUM_21
#define KNOB_PIN_DT GPIO_NUM_9
#define KNOB_PIN_SW GPIO_NUM_20
#define KNOB_DELTA_POS_STEP 5

/* Onboard/addressable RGB LED (WS2812-style single data pin). */
#define RGB_LED_PIN GPIO_NUM_8
#define RGB_LOG_THROTTLE_MS 100
#define RGB_RMT_RESOLUTION_HZ 10000000

static const char *TAG = "dht20_app";

#define UART_PRINT_INFO(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define UART_PRINT_WARN(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define UART_PRINT_ERR(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)

#if APP_ENABLE_KNOB
typedef struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    uint8_t rgb[3];
    uint8_t selected_channel;
    TickType_t last_log_tick;
} rgb_ctrl_t;

static esp_err_t rgb_ctrl_apply(const rgb_ctrl_t *ctrl)
{
    ESP_RETURN_ON_FALSE(ctrl != NULL, ESP_ERR_INVALID_ARG, TAG, "ctrl is null");
    ESP_RETURN_ON_FALSE(ctrl->channel != NULL, ESP_ERR_INVALID_STATE, TAG, "channel is null");
    ESP_RETURN_ON_FALSE(ctrl->encoder != NULL, ESP_ERR_INVALID_STATE, TAG, "encoder is null");

    const uint8_t grb[3] = {ctrl->rgb[1], ctrl->rgb[0], ctrl->rgb[2]};
    const rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags = {
            .eot_level = 0,
        },
    };

    esp_err_t err = rmt_transmit(ctrl->channel, ctrl->encoder, grb, sizeof(grb), &tx_cfg);
    if ((err == ESP_ERR_TIMEOUT) || (err == ESP_ERR_INVALID_STATE)) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "rmt_transmit failed");
    return ESP_OK;
}

static char rgb_channel_name(const uint8_t channel)
{
    static const char names[3] = {'R', 'G', 'B'};
    return (channel < 3U) ? names[channel] : '?';
}

static void knob_process(knob_t *knob, rgb_ctrl_t *rgb_ctrl)
{
    knob_event_t event = {0};
    if (knob_poll(knob, &event) != ESP_OK) {
        return;
    }

    if ((event.delta != 0) && (rgb_ctrl != NULL) && (rgb_ctrl->channel != NULL)) {
        int32_t value = (int32_t)rgb_ctrl->rgb[rgb_ctrl->selected_channel] + (event.delta * KNOB_DELTA_POS_STEP);
        if (value < 0) {
            value = 0;
        } else if (value > 255) {
            value = 255;
        }

        if ((uint8_t)value != rgb_ctrl->rgb[rgb_ctrl->selected_channel]) {
            rgb_ctrl->rgb[rgb_ctrl->selected_channel] = (uint8_t)value;
            (void)knob_set_position(knob, value);
            (void)rgb_ctrl_apply(rgb_ctrl);

            const TickType_t now_tick = xTaskGetTickCount();
            if ((now_tick - rgb_ctrl->last_log_tick) >= pdMS_TO_TICKS(RGB_LOG_THROTTLE_MS)) {
                UART_PRINT_INFO("RGB %c=%d  [R=%d G=%d B=%d]",
                                rgb_channel_name(rgb_ctrl->selected_channel),
                                (int)rgb_ctrl->rgb[rgb_ctrl->selected_channel],
                                (int)rgb_ctrl->rgb[0], (int)rgb_ctrl->rgb[1], (int)rgb_ctrl->rgb[2]);
                rgb_ctrl->last_log_tick = now_tick;
            }
        }
    }

    if (event.clicked && (rgb_ctrl != NULL)) {
        rgb_ctrl->selected_channel = (uint8_t)((rgb_ctrl->selected_channel + 1U) % 3U);
        (void)knob_set_position(knob, rgb_ctrl->rgb[rgb_ctrl->selected_channel]);
        UART_PRINT_INFO("Selected channel: %c", rgb_channel_name(rgb_ctrl->selected_channel));
    }
}
#endif

#if APP_ENABLE_DISPLAY
static int display_text_width_px(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    return (int)strlen(s) * 6 * DISPLAY_TEXT_SCALE;
}

static void display_draw_two_lines_centered(const char *line1, const char *line2)
{
    /* 5x7 font, scaled by DISPLAY_TEXT_SCALE, centered as a two-line block. */
    const int display_w = display_get_width();
    const int display_h = display_get_height();
    const int line_h = 7 * DISPLAY_TEXT_SCALE;
    const int block_h = (2 * line_h) + DISPLAY_TEXT_LINE_GAP;
    const int line1_y = (display_h - block_h) / 2;
    const int line2_y = line1_y + line_h + DISPLAY_TEXT_LINE_GAP;
    const int x1 = (display_w - display_text_width_px(line1)) / 2;
    const int x2 = (display_w - display_text_width_px(line2)) / 2;

    display_draw_rect(0, line1_y - DISPLAY_TEXT_SCALE, display_w, block_h + (2 * DISPLAY_TEXT_SCALE), 0x0000);
    display_draw_text_minimal_scaled(x1 > 0 ? x1 : 0, line1_y, line1, DISPLAY_TEXT_COLOR, DISPLAY_TEXT_SCALE);
    display_draw_text_minimal_scaled(x2 > 0 ? x2 : 0, line2_y, line2, DISPLAY_TEXT_COLOR, DISPLAY_TEXT_SCALE);
}

#if APP_ENABLE_DHT20
static void display_show_avg(float temp_c, float rh)
{
    static bool has_last = false;
    static char last_line1[32];
    static char last_line2[32];
    char line1[32];
    char line2[32];

    snprintf(line1, sizeof(line1), "TEMP: %.1f C", temp_c);
    snprintf(line2, sizeof(line2), "RH: %.1f %%", rh);

    if (has_last && strcmp(line1, last_line1) == 0 && strcmp(line2, last_line2) == 0) {
        return;
    }

    strncpy(last_line1, line1, sizeof(last_line1));
    last_line1[sizeof(last_line1) - 1] = '\0';
    strncpy(last_line2, line2, sizeof(last_line2));
    last_line2[sizeof(last_line2) - 1] = '\0';
    has_last = true;

    display_draw_two_lines_centered(line1, line2);
}
#endif

typedef struct {
    const char *name;
    uint32_t elapsed_ms;
    uint32_t ops;
} display_bench_result_t;

static display_bench_result_t display_bench_full_fill(const uint16_t *colors, size_t color_count)
{
    const uint32_t frames = 24U;
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t i = 0; i < frames; i++) {
        display_fill_color(colors[i % color_count]);
    }
    const int64_t dt_us = esp_timer_get_time() - t0;
    return (display_bench_result_t){
        .name = "FILL",
        .elapsed_ms = (uint32_t)(dt_us / 1000LL),
        .ops = frames,
    };
}

static display_bench_result_t display_bench_rects(const uint16_t *colors, size_t color_count)
{
    const int w = display_get_width();
    const int h = display_get_height();
    const int max_rw = (w > 20) ? (w / 2) : 10;
    const int max_rh = (h > 20) ? (h / 2) : 10;
    const uint32_t draws = 150U;

    display_fill_color(0x0000);
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t i = 0; i < draws; i++) {
        int rw = 8 + (int)((i * 11U) % (uint32_t)max_rw);
        int rh = 8 + (int)((i * 7U) % (uint32_t)max_rh);
        if (rw > w) {
            rw = w;
        }
        if (rh > h) {
            rh = h;
        }
        const int x = (int)((i * 13U) % (uint32_t)((w - rw) > 0 ? (w - rw) : 1));
        const int y = (int)((i * 9U) % (uint32_t)((h - rh) > 0 ? (h - rh) : 1));
        display_draw_rect(x, y, rw, rh, colors[i % color_count]);
    }
    const int64_t dt_us = esp_timer_get_time() - t0;
    return (display_bench_result_t){
        .name = "RECT",
        .elapsed_ms = (uint32_t)(dt_us / 1000LL),
        .ops = draws,
    };
}

static display_bench_result_t display_bench_text(void)
{
    const int w = display_get_width();
    const int h = display_get_height();
    const uint32_t draws = 120U;
    char line[24];

    display_fill_color(0x0000);
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t i = 0; i < draws; i++) {
        const int y = (int)((i * 10U) % (uint32_t)((h > 10) ? (h - 10) : 1));
        const int x = (int)((i * 17U) % (uint32_t)((w > 42) ? (w - 42) : 1));
        const uint16_t color = (uint16_t)(0x001F + ((i * 97U) & 0xFFE0U));
        snprintf(line, sizeof(line), "T%03" PRIu32, i);
        display_draw_text_minimal(x, y, line, color);
    }
    const int64_t dt_us = esp_timer_get_time() - t0;
    return (display_bench_result_t){
        .name = "TEXT",
        .elapsed_ms = (uint32_t)(dt_us / 1000LL),
        .ops = draws,
    };
}

static void display_bench_plot_results(const display_bench_result_t *results, size_t count)
{
    const int w = display_get_width();
    const int h = display_get_height();
    const int chart_x = 64;
    const int chart_y = 24;
    const int row_h = 32;
    const int bar_h = 18;
    const int bar_max_w = (w - chart_x - 10 > 10) ? (w - chart_x - 10) : 10;
    uint32_t max_ms = 1;
    char line[32];

    for (size_t i = 0; i < count; i++) {
        if (results[i].elapsed_ms > max_ms) {
            max_ms = results[i].elapsed_ms;
        }
    }

    display_fill_color(0x0000);
    display_draw_text_minimal(6, 6, "DISPLAY BENCH", 0xFFFF);
    for (size_t i = 0; i < count; i++) {
        const int y = chart_y + ((int)i * row_h);
        int bar_w = (int)(((uint64_t)results[i].elapsed_ms * (uint64_t)bar_max_w) / (uint64_t)max_ms);
        if (bar_w < 1) {
            bar_w = 1;
        }
        if (bar_w > bar_max_w) {
            bar_w = bar_max_w;
        }

        display_draw_text_minimal(6, y + 4, results[i].name, 0xFFFF);
        display_draw_rect(chart_x, y, bar_max_w, bar_h, 0x2104);
        display_draw_rect(chart_x, y, bar_w, bar_h, (i == 0) ? 0x07E0 : ((i == 1) ? 0xFFE0 : 0xF81F));
        snprintf(line, sizeof(line), "%" PRIu32 "ms", results[i].elapsed_ms);
        display_draw_text_minimal(chart_x + 4, y + 4, line, 0x0000);
    }

    if (h >= 158) {
        display_draw_text_minimal(6, h - 12, "Lower ms is better", 0xFFFF);
    }
}

static void display_run_benchmark(void)
{
    static const uint16_t k_colors[] = {
        0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F
    };
    display_bench_result_t results[3];

    results[0] = display_bench_full_fill(k_colors, sizeof(k_colors) / sizeof(k_colors[0]));
    results[1] = display_bench_rects(k_colors, sizeof(k_colors) / sizeof(k_colors[0]));
    results[2] = display_bench_text();
    display_bench_plot_results(results, sizeof(results) / sizeof(results[0]));

    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        uint32_t ops_per_s = 0;
        if (results[i].elapsed_ms > 0U) {
            ops_per_s = (results[i].ops * 1000U) / results[i].elapsed_ms;
        }
        UART_PRINT_INFO("display bench %-4s -> %" PRIu32 " ms | ops=%" PRIu32 " | ~%" PRIu32 " ops/s",
                        results[i].name, results[i].elapsed_ms, results[i].ops, ops_per_s);
    }
}
#endif

#if APP_ENABLE_DHT20
static esp_err_t i2c_bus_init(void)
{
    const i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DHT20_I2C_SDA_GPIO,
        .scl_io_num = DHT20_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = DHT20_I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(DHT20_I2C_PORT, &i2c_cfg), TAG, "i2c_param_config failed");
    return i2c_driver_install(DHT20_I2C_PORT, i2c_cfg.mode, 0, 0, 0);
}
#endif

static bool app_check_and_log(const char *step, const esp_err_t err)
{
    if (err == ESP_OK) {
        return true;
    }
    UART_PRINT_ERR("%s failed: %s (0x%" PRIx32 ")", step, esp_err_to_name(err), (uint32_t)err);
    return false;
}

void app_main(void)
{
#if APP_ENABLE_KNOB
    knob_t knob = {0};
    bool knob_ready = false;
#if APP_ENABLE_RGB_LED
    rgb_ctrl_t rgb_ctrl = {0};
    bool rgb_ready = false;
#endif
#endif

#if APP_ENABLE_DISPLAY && APP_ENABLE_DHT20
    bool display_ready = false;
#endif

#if APP_ENABLE_DHT20
    dht20_t dht20 = {0};
    dht20_sample_t sample = {0};
    bool dht20_ready = false;
    int64_t window_start_us = esp_timer_get_time();
    float temperature_sum = 0.0f;
    float humidity_sum = 0.0f;
    uint32_t valid_samples = 0;
    uint32_t error_samples = 0;
#endif
    TickType_t last_idle_log_tick = xTaskGetTickCount();
    TickType_t loop_wake_tick = xTaskGetTickCount();

#if APP_ENABLE_KNOB

#if APP_ENABLE_RGB_LED
    rmt_tx_channel_config_t tx_chan_cfg = {
        .gpio_num = RGB_LED_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RGB_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .flags = {
            .invert_out = false,
            .with_dma = false,
        },
    };
    rmt_bytes_encoder_config_t bytes_enc_cfg = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 4, /* 0.4 us @ 10 MHz */
            .level1 = 0,
            .duration1 = 8, /* 0.8 us @ 10 MHz */
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 8, /* 0.8 us @ 10 MHz */
            .level1 = 0,
            .duration1 = 4, /* 0.4 us @ 10 MHz */
        },
        .flags = {
            .msb_first = 1,
        },
    };

    if (app_check_and_log("rmt_new_tx_channel", rmt_new_tx_channel(&tx_chan_cfg, &rgb_ctrl.channel))
        && app_check_and_log("rmt_new_bytes_encoder", rmt_new_bytes_encoder(&bytes_enc_cfg, &rgb_ctrl.encoder))
        && app_check_and_log("rmt_enable", rmt_enable(rgb_ctrl.channel))) {
        rgb_ctrl.rgb[0] = 0;  /* RED */
        rgb_ctrl.rgb[1] = 0;  /* GREEN */
        rgb_ctrl.rgb[2] = 0;  /* BLUE */
        rgb_ctrl.selected_channel = 0; /* start in RED as requested */
        rgb_ctrl.last_log_tick = xTaskGetTickCount();
        (void)app_check_and_log("rgb_ctrl_apply", rgb_ctrl_apply(&rgb_ctrl));
        UART_PRINT_INFO("Selected channel: R");
        rgb_ready = true;
    } else {
        UART_PRINT_WARN("RGB LED disabled due to initialization error");
    }
#endif

    const knob_pins_t knob_pins = {
        .clk = KNOB_PIN_CLK,
        .dt = KNOB_PIN_DT,
        .sw = KNOB_PIN_SW,
    };
    const knob_cfg_t knob_cfg = {
        .enable_pullup = true,
        .button_active_low = true,
        .button_debounce_ms = 30U,
    };
    knob_ready = app_check_and_log("knob_init", knob_init(&knob, &knob_pins, &knob_cfg));
#if APP_ENABLE_RGB_LED
    if (knob_ready && rgb_ready) {
        (void)app_check_and_log("knob_set_position", knob_set_position(&knob, rgb_ctrl.rgb[rgb_ctrl.selected_channel]));
    }
#endif
#endif

#if APP_ENABLE_WIFI_HTTP
    const wifi_http_api_cfg_t wifi_http_cfg = {
        .ap_ssid = WIFI_HTTP_AP_SSID,
        .ap_password = WIFI_HTTP_AP_PASS,
        .ap_channel = WIFI_HTTP_AP_CHANNEL,
        .ap_max_connection = 4,
        .start_mode = WIFI_MODE_APSTA,
    };
    if (!app_check_and_log("wifi_http_api_init", wifi_http_api_init(&wifi_http_cfg))) {
        UART_PRINT_WARN("Wi-Fi HTTP API disabled due to initialization error");
    }
#endif

#if APP_ENABLE_DHT20
    if (app_check_and_log("i2c_bus_init", i2c_bus_init())
        && app_check_and_log("dht20_init", dht20_init(&dht20, DHT20_I2C_PORT, DHT20_I2C_ADDR_DEFAULT, DHT20_I2C_TIMEOUT_MS))
        && app_check_and_log("dht20_start_measurement", dht20_start_measurement(&dht20))) {
        dht20_ready = true;
    } else {
        UART_PRINT_WARN("DHT20 acquisition disabled; remaining peripherals will keep running");
    }
#else
    UART_PRINT_WARN("DHT20 disabled by APP_ENABLE_DHT20=0");
#endif

#if APP_ENABLE_DISPLAY
    const display_pins_t display_pins = {
        .sck = TFT_PIN_SCK,
        .mosi = TFT_PIN_MOSI,
        .cs = TFT_PIN_CS,
        .dc = TFT_PIN_DC,
        .reset = TFT_PIN_RST,
        .backlight = TFT_PIN_BLK,
    };

    const display_cfg_t display_cfg = {
        .width = TFT_WIDTH,
        .height = TFT_HEIGHT,
        .x_offset = DISPLAY_X_OFFSET,
        .y_offset = DISPLAY_Y_OFFSET,
        .spi_clock_hz = DISPLAY_SPI_CLOCK_HZ,
    };

    if (app_check_and_log("display_init", display_init(&display_pins, &display_cfg))) {
#if APP_ENABLE_DHT20
        display_ready = true;
#endif
        display_set_rotation(DISPLAY_ROTATION);
        display_backlight_set(90);
        display_self_test();
        display_image_t img = {0};
        display_image_init(&img, display_get_panel_handle(), (uint16_t)display_get_width(), (uint16_t)display_get_height());
        (void)app_check_and_log("display_image_draw_test_pattern_streaming", display_image_draw_test_pattern_streaming(&img, 20));
#if APP_RUN_DISPLAY_BENCHMARK
        display_run_benchmark();
#else
        display_fill_color(0x0000);
#endif
#if APP_ENABLE_DHT20
        if (dht20_ready) {
            display_show_avg(0.0f, 0.0f);
        } else if (!APP_RUN_DISPLAY_BENCHMARK) {
            display_draw_two_lines_centered("TEMP: --.- C", "RH: --.- %");
        }
#else
        if (!APP_RUN_DISPLAY_BENCHMARK) {
            display_draw_two_lines_centered("TEMP: --.- C", "RH: --.- %");
        }
#endif
#if APP_ENABLE_SNTP
        const sntp_api_cfg_t sntp_cfg = {
            .server_name = SNTP_SERVER_NAME,
            .gmt_offset_hours = SNTP_GMT_OFFSET_HOURS,
            .sync_interval_ms = SNTP_SYNC_INTERVAL_MS,
            .bar_bg_color = SNTP_BAR_BG_COLOR,
            .bar_fg_color = SNTP_BAR_FG_COLOR,
            .text_scale = SNTP_BASE_TEXT_SCALE,
            .date_scale = SNTP_DATE_TEXT_SCALE,
            .time_scale = SNTP_TIME_TEXT_SCALE,
            .line_gap_px = SNTP_LINE_GAP_PX,
            .date_char_spacing_px = SNTP_DATE_CHAR_SPACING_PX,
            .time_char_spacing_px = SNTP_TIME_CHAR_SPACING_PX,
        };
        if (app_check_and_log("sntp_api_init", sntp_api_init(&sntp_cfg))) {
            sntp_api_status_bar_draw();
        } else {
            UART_PRINT_WARN("SNTP status bar disabled due to initialization error");
        }
#endif
    } else {
        UART_PRINT_WARN("Display disabled due to initialization error");
    }
#else
    UART_PRINT_WARN("Display disabled by APP_ENABLE_DISPLAY=0");
#endif

    while (true) {
#if APP_ENABLE_DHT20
        if (dht20_ready) {
        esp_err_t err = dht20_read_measurement_wait(&dht20, &sample, DHT20_READY_TIMEOUT_MS, DHT20_POLL_INTERVAL_MS);

        if (err == ESP_OK) {
                if (app_check_and_log("dht20_sample_apply_offset",
                                      dht20_sample_apply_offset(&sample, DHT20_TEMP_OFFSET_C, DHT20_HUM_OFFSET_RH))) {
                    temperature_sum += sample.temperature_c;
                    humidity_sum += sample.humidity_rh;
                    valid_samples++;
                } else {
                    error_samples++;
                }
        } else {
            error_samples++;
        }

        err = dht20_start_measurement(&dht20);
        if (err != ESP_OK) {
            UART_PRINT_ERR("start measurement failed: %s", esp_err_to_name(err));
                dht20_ready = false;
        }

        const int64_t now_us = esp_timer_get_time();
        if ((now_us - window_start_us) >= ((int64_t)DHT20_PRINT_PERIOD_MS * 1000LL)) {
            if (valid_samples > 0) {
                const float avg_temp_c = temperature_sum / (float)valid_samples;
                const float avg_humidity_rh = humidity_sum / (float)valid_samples;

                UART_PRINT_INFO("2s avg -> T=%.2f C | RH=%.2f %% | valid=%" PRIu32 " | errors=%" PRIu32,
                                avg_temp_c, avg_humidity_rh, valid_samples, error_samples);
#if APP_ENABLE_DISPLAY
                    if (display_ready) {
                        display_show_avg(avg_temp_c, avg_humidity_rh);
                    }
#endif
            } else {
                UART_PRINT_WARN("2s avg -> no valid sample | errors=%" PRIu32, error_samples);
            }

            window_start_us = now_us;
            temperature_sum = 0.0f;
            humidity_sum = 0.0f;
            valid_samples = 0;
            error_samples = 0;
        }
        } else {
            const TickType_t now_tick = xTaskGetTickCount();
            if ((now_tick - last_idle_log_tick) >= pdMS_TO_TICKS(DHT20_PRINT_PERIOD_MS)) {
                UART_PRINT_WARN("DHT20 unavailable; running remaining peripherals");
                last_idle_log_tick = now_tick;
            }
        }
#else
        const TickType_t now_tick = xTaskGetTickCount();
        if ((now_tick - last_idle_log_tick) >= pdMS_TO_TICKS(DHT20_PRINT_PERIOD_MS)) {
            UART_PRINT_INFO("DHT20 disabled; no sensor acquisition running");
            last_idle_log_tick = now_tick;
        }
#endif

#if APP_ENABLE_KNOB
        if (knob_ready) {
            knob_process(&knob,
#if APP_ENABLE_RGB_LED
                         rgb_ready ? &rgb_ctrl : NULL
#else
                         NULL
#endif
            );
        }
#endif
#if APP_ENABLE_DISPLAY && APP_ENABLE_SNTP
        sntp_api_status_bar_update_if_due(SNTP_STATUS_REFRESH_MS);
#endif
        vTaskDelayUntil(&loop_wake_tick, pdMS_TO_TICKS(10));
    }
}
