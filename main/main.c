/*
 * SPDX-License-Identifier: 0BSD
 */

#include <inttypes.h>
#include <stdio.h>

#include "dht20_api.h"
#include "display_api.h"
#include "display_image.h"
#include "knob_api.h"
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
#define APP_ENABLE_DISPLAY 0
#define APP_ENABLE_KNOB 1
#define APP_ENABLE_RGB_LED 1

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
#define DISPLAY_ROTATION DISPLAY_ROTATION_90

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
static void display_show_avg(float temp_c, float rh)
{
    char line1[32];
    char line2[32];
    const int display_w = display_get_width();

    snprintf(line1, sizeof(line1), "TEMP: %.1f C", temp_c);
    snprintf(line2, sizeof(line2), "RH: %.1f %%", rh);

    display_draw_rect(0, 0, display_w, 42, 0x0000);
    display_draw_text_minimal(8, 8, line1, 0xFFFF);
    display_draw_text_minimal(8, 22, line2, 0xFFFF);
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

void app_main(void)
{
#if APP_ENABLE_KNOB
    knob_t knob = {0};
    rgb_ctrl_t rgb_ctrl = {0};

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

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_cfg, &rgb_ctrl.channel));
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&bytes_enc_cfg, &rgb_ctrl.encoder));
    ESP_ERROR_CHECK(rmt_enable(rgb_ctrl.channel));

    rgb_ctrl.rgb[0] = 0;  /* RED */
    rgb_ctrl.rgb[1] = 0;  /* GREEN */
    rgb_ctrl.rgb[2] = 0;  /* BLUE */
    rgb_ctrl.selected_channel = 0; /* start in RED as requested */
    rgb_ctrl.last_log_tick = xTaskGetTickCount();
    ESP_ERROR_CHECK(rgb_ctrl_apply(&rgb_ctrl));
    UART_PRINT_INFO("Selected channel: R");
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
    ESP_ERROR_CHECK(knob_init(&knob, &knob_pins, &knob_cfg));
#if APP_ENABLE_RGB_LED
    ESP_ERROR_CHECK(knob_set_position(&knob, rgb_ctrl.rgb[rgb_ctrl.selected_channel]));
#endif
#endif

#if APP_ENABLE_DHT20
    ESP_ERROR_CHECK(i2c_bus_init());
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

    ESP_ERROR_CHECK(display_init(&display_pins, &display_cfg));
    display_set_rotation(DISPLAY_ROTATION);
    display_backlight_set(90);
    display_self_test();
    display_image_t img = {0};
    display_image_init(&img, display_get_panel_handle(), (uint16_t)display_get_width(), (uint16_t)display_get_height());
    ESP_ERROR_CHECK(display_image_draw_test_pattern_streaming(&img, 20));
    display_fill_color(0x0000);
    if (APP_ENABLE_DHT20) {
        display_show_avg(0.0f, 0.0f);
    } else {
        display_draw_text_minimal(8, 8, "TEMP: --.- C", 0xFFFF);
        display_draw_text_minimal(8, 22, "RH: --.- %", 0xFFFF);
    }
#else
    UART_PRINT_WARN("Display disabled by APP_ENABLE_DISPLAY=0");
#endif

#if APP_ENABLE_DHT20
    dht20_t dht20 = {0};
    dht20_sample_t sample = {0};

    ESP_ERROR_CHECK(dht20_init(&dht20, DHT20_I2C_PORT, DHT20_I2C_ADDR_DEFAULT, DHT20_I2C_TIMEOUT_MS));
    ESP_ERROR_CHECK(dht20_start_measurement(&dht20));

    int64_t window_start_us = esp_timer_get_time();
    float temperature_sum = 0.0f;
    float humidity_sum = 0.0f;
    uint32_t valid_samples = 0;
    uint32_t error_samples = 0;

    while (true) {
        esp_err_t err = dht20_read_measurement_wait(&dht20, &sample, DHT20_READY_TIMEOUT_MS, DHT20_POLL_INTERVAL_MS);

        if (err == ESP_OK) {
            ESP_ERROR_CHECK(dht20_sample_apply_offset(&sample, DHT20_TEMP_OFFSET_C, DHT20_HUM_OFFSET_RH));
            temperature_sum += sample.temperature_c;
            humidity_sum += sample.humidity_rh;
            valid_samples++;
        } else {
            error_samples++;
        }

        err = dht20_start_measurement(&dht20);
        if (err != ESP_OK) {
            UART_PRINT_ERR("start measurement failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        const int64_t now_us = esp_timer_get_time();
        if ((now_us - window_start_us) >= ((int64_t)DHT20_PRINT_PERIOD_MS * 1000LL)) {
            if (valid_samples > 0) {
                const float avg_temp_c = temperature_sum / (float)valid_samples;
                const float avg_humidity_rh = humidity_sum / (float)valid_samples;

                UART_PRINT_INFO("2s avg -> T=%.2f C | RH=%.2f %% | valid=%" PRIu32 " | errors=%" PRIu32,
                                avg_temp_c, avg_humidity_rh, valid_samples, error_samples);
#if APP_ENABLE_DISPLAY
                display_show_avg(avg_temp_c, avg_humidity_rh);
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

#if APP_ENABLE_KNOB
        knob_process(&knob,
#if APP_ENABLE_RGB_LED
                     &rgb_ctrl
#else
                     NULL
#endif
        );
#endif
        vTaskDelay(pdMS_TO_TICKS(1));
    }
#else
    TickType_t last_idle_log_tick = xTaskGetTickCount();
    TickType_t loop_wake_tick = xTaskGetTickCount();
    while (true) {
#if APP_ENABLE_KNOB
        knob_process(&knob,
#if APP_ENABLE_RGB_LED
                     &rgb_ctrl
#else
                     NULL
#endif
        );
#endif
        const TickType_t now_tick = xTaskGetTickCount();
        if ((now_tick - last_idle_log_tick) >= pdMS_TO_TICKS(DHT20_PRINT_PERIOD_MS)) {
            UART_PRINT_INFO("DHT20 disabled; no sensor acquisition running");
            last_idle_log_tick = now_tick;
        }
        vTaskDelayUntil(&loop_wake_tick, pdMS_TO_TICKS(10));
    }
#endif
}
