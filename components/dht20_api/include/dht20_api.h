/*
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c.h"
#include "esp_err.h"

/**
 * @file dht20_api.h
 * @brief Reusable DHT20 sensor API over I2C.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief DHT20 device descriptor. */
typedef struct {
    i2c_port_t i2c_port;
    uint8_t i2c_addr;
    uint32_t i2c_timeout_ms;
} dht20_t;

/** @brief One decoded DHT20 sample. */
typedef struct {
    uint64_t timestamp_us;
    uint32_t humidity_raw;
    uint32_t temperature_raw;
    float humidity_rh;
    float temperature_c;
} dht20_sample_t;

/** @brief Exponential moving average state for signal smoothing. */
typedef struct {
    bool initialized;
    float alpha;
    float humidity_rh;
    float temperature_c;
} dht20_filter_t;

#define DHT20_I2C_ADDR_DEFAULT 0x38

/** @brief Initialize DHT20, including optional calibration command sequence. */
esp_err_t dht20_init(dht20_t *dev, i2c_port_t i2c_port, uint8_t i2c_addr, uint32_t i2c_timeout_ms);
/** @brief Trigger DHT20 software reset. */
esp_err_t dht20_soft_reset(const dht20_t *dev);
/** @brief Start one measurement conversion. */
esp_err_t dht20_start_measurement(const dht20_t *dev);
/** @brief Read one completed measurement frame. */
esp_err_t dht20_read_measurement(const dht20_t *dev, dht20_sample_t *sample);
/** @brief Poll until conversion completes or timeout is reached. */
esp_err_t dht20_read_measurement_wait(const dht20_t *dev, dht20_sample_t *sample, uint32_t timeout_ms, uint32_t poll_interval_ms);
/** @brief Start and wait for one conversion in a single call. */
esp_err_t dht20_read_oneshot(const dht20_t *dev, dht20_sample_t *sample, uint32_t timeout_ms, uint32_t poll_interval_ms);
/** @brief Legacy helper: start conversion, delay fixed time, then read. */
esp_err_t dht20_read(const dht20_t *dev, dht20_sample_t *sample, uint32_t conversion_wait_ms);
/** @brief Apply post-processing offsets to a sample. */
esp_err_t dht20_sample_apply_offset(dht20_sample_t *sample, float temperature_offset_c, float humidity_offset_rh);
/** @brief Initialize EMA filter state. */
esp_err_t dht20_filter_init(dht20_filter_t *filter, float alpha);
/** @brief Reset EMA filter state. */
void dht20_filter_reset(dht20_filter_t *filter);
/** @brief Apply EMA filter to one input sample. */
esp_err_t dht20_filter_apply(dht20_filter_t *filter, const dht20_sample_t *input, dht20_sample_t *output);

#ifdef __cplusplus
}
#endif
