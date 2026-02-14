/*
 * SPDX-License-Identifier: 0BSD
 */

#include "knob_api.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define KNOB_TAG "knob_api"
#define KNOB_DEFAULT_DEBOUNCE_MS 30U

static uint32_t knob_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint8_t knob_read_ab(const knob_t *knob)
{
    const uint8_t clk = (uint8_t)gpio_get_level(knob->pins.clk) & 0x01U;
    const uint8_t dt = (uint8_t)gpio_get_level(knob->pins.dt) & 0x01U;
    return (uint8_t)((clk << 1) | dt);
}

static uint8_t knob_read_sw(const knob_t *knob)
{
    return (uint8_t)(gpio_get_level(knob->pins.sw) & 0x01U);
}

knob_status_t knob_init(knob_t *knob, const knob_pins_t *pins, const knob_cfg_t *cfg)
{
    ESP_RETURN_ON_FALSE(knob != NULL, ESP_ERR_INVALID_ARG, KNOB_TAG, "knob is null");
    ESP_RETURN_ON_FALSE(pins != NULL, ESP_ERR_INVALID_ARG, KNOB_TAG, "pins is null");
    ESP_RETURN_ON_FALSE(pins->clk >= 0, ESP_ERR_INVALID_ARG, KNOB_TAG, "invalid clk pin");
    ESP_RETURN_ON_FALSE(pins->dt >= 0, ESP_ERR_INVALID_ARG, KNOB_TAG, "invalid dt pin");
    ESP_RETURN_ON_FALSE(pins->sw >= 0, ESP_ERR_INVALID_ARG, KNOB_TAG, "invalid sw pin");

    memset(knob, 0, sizeof(*knob));
    knob->pins = *pins;

    knob_cfg_t local_cfg = {
        .enable_pullup = true,
        .button_active_low = true,
        .button_debounce_ms = KNOB_DEFAULT_DEBOUNCE_MS,
    };
    if (cfg != NULL) {
        local_cfg = *cfg;
        if (local_cfg.button_debounce_ms == 0U) {
            local_cfg.button_debounce_ms = KNOB_DEFAULT_DEBOUNCE_MS;
        }
    }
    knob->cfg = local_cfg;

    const gpio_pullup_t pullup = local_cfg.enable_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << pins->clk) | (1ULL << pins->dt) | (1ULL << pins->sw),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pullup,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), KNOB_TAG, "gpio_config failed");

    knob->last_ab = knob_read_ab(knob);
    knob->last_sw_level = knob_read_sw(knob);
    knob->last_sw_change_ms = knob_now_ms();
    knob->initialized = true;

    return ESP_OK;
}

knob_status_t knob_poll(knob_t *knob, knob_event_t *event_out)
{
    static const int8_t transition_table[16] = {
        0, -1, 1, 0,
        1, 0, 0, -1,
        -1, 0, 0, 1,
        0, 1, -1, 0,
    };

    ESP_RETURN_ON_FALSE(knob != NULL, ESP_ERR_INVALID_ARG, KNOB_TAG, "knob is null");
    ESP_RETURN_ON_FALSE(event_out != NULL, ESP_ERR_INVALID_ARG, KNOB_TAG, "event_out is null");
    ESP_RETURN_ON_FALSE(knob->initialized, ESP_ERR_INVALID_STATE, KNOB_TAG, "knob not initialized");

    memset(event_out, 0, sizeof(*event_out));

    const uint8_t ab = knob_read_ab(knob);
    const uint8_t idx = (uint8_t)((knob->last_ab << 2) | ab);
    knob->step_acc = (int8_t)(knob->step_acc + transition_table[idx]);
    knob->last_ab = ab;

    if (knob->step_acc >= 4) {
        knob->position++;
        event_out->delta = 1;
        knob->step_acc = 0;
    } else if (knob->step_acc <= -4) {
        knob->position--;
        event_out->delta = -1;
        knob->step_acc = 0;
    }

    const uint8_t raw_sw = knob_read_sw(knob);
    const uint32_t now_ms = knob_now_ms();

    if (raw_sw != knob->last_sw_level) {
        if ((now_ms - knob->last_sw_change_ms) >= knob->cfg.button_debounce_ms) {
            knob->last_sw_level = raw_sw;
            knob->last_sw_change_ms = now_ms;

            const bool is_pressed = knob->cfg.button_active_low ? (raw_sw == 0U) : (raw_sw != 0U);
            if (is_pressed) {
                knob->pressed_latched = true;
                event_out->pressed = true;
            } else {
                event_out->released = true;
                event_out->clicked = knob->pressed_latched;
                knob->pressed_latched = false;
            }
        }
    }

    event_out->position = knob->position;
    return ESP_OK;
}

int32_t knob_get_position(const knob_t *knob)
{
    if ((knob == NULL) || (!knob->initialized)) {
        return 0;
    }
    return knob->position;
}

knob_status_t knob_set_position(knob_t *knob, int32_t position)
{
    ESP_RETURN_ON_FALSE(knob != NULL, ESP_ERR_INVALID_ARG, KNOB_TAG, "knob is null");
    ESP_RETURN_ON_FALSE(knob->initialized, ESP_ERR_INVALID_STATE, KNOB_TAG, "knob not initialized");

    knob->position = position;
    return ESP_OK;
}
