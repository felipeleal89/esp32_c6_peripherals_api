/*
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

/**
 * @file knob_api.h
 * @brief Reusable rotary encoder (CLK/DT/SW) API for ESP-IDF.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Knob module status code alias. */
typedef esp_err_t knob_status_t;

/** @brief Physical GPIO mapping for a standard rotary encoder. */
typedef struct {
    gpio_num_t clk;
    gpio_num_t dt;
    gpio_num_t sw;
} knob_pins_t;

/** @brief Runtime configuration for encoder sampling and button behavior. */
typedef struct {
    bool enable_pullup;
    bool button_active_low;
    uint32_t button_debounce_ms;
} knob_cfg_t;

/** @brief Event returned by knob polling. */
typedef struct {
    int8_t delta;
    bool pressed;
    bool released;
    bool clicked;
    int32_t position;
} knob_event_t;

/** @brief Knob instance state. */
typedef struct {
    knob_pins_t pins;
    knob_cfg_t cfg;
    int32_t position;
    int8_t step_acc;
    uint8_t last_ab;
    uint8_t last_sw_level;
    uint32_t last_sw_change_ms;
    bool pressed_latched;
    bool initialized;
} knob_t;

/** @brief Initialize a knob instance and GPIOs (input-only). */
knob_status_t knob_init(knob_t *knob, const knob_pins_t *pins, const knob_cfg_t *cfg);
/** @brief Poll encoder/button and return incremental events. */
knob_status_t knob_poll(knob_t *knob, knob_event_t *event_out);
/** @brief Get current logical knob position. */
int32_t knob_get_position(const knob_t *knob);
/** @brief Set current logical knob position. */
knob_status_t knob_set_position(knob_t *knob, int32_t position);

#ifdef __cplusplus
}
#endif
