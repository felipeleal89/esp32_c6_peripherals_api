/*
 * SPDX-License-Identifier: 0BSD
 */

#include "dht20_api.h"

#include <math.h>
#include <stddef.h>

#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DHT20_CMD_SOFT_RESET 0xBA
#define DHT20_CMD_STATUS 0x71
#define DHT20_CMD_TRIGGER 0xAC
#define DHT20_ARG_TRIGGER_1 0x33
#define DHT20_ARG_TRIGGER_2 0x00

#define DHT20_STATUS_BUSY_MASK (1U << 7)
#define DHT20_STATUS_CAL_MASK (1U << 3)

#define DHT20_CMD_INIT 0xBE
#define DHT20_ARG_INIT_1 0x08
#define DHT20_ARG_INIT_2 0x00

#define DHT20_DATA_LEN 7
#define DHT20_STATUS_READY_DELAY_MS 10
#define DHT20_SOFT_RESET_DELAY_MS 20
#define DHT20_POWER_ON_DELAY_MS 100

static uint8_t dht20_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static esp_err_t dht20_write(const dht20_t *dev, const uint8_t *tx, size_t tx_len)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(dev->i2c_timeout_ms);
    return i2c_master_write_to_device(dev->i2c_port, dev->i2c_addr, tx, tx_len, timeout_ticks);
}

static esp_err_t dht20_read_raw(const dht20_t *dev, uint8_t *rx, size_t rx_len)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(dev->i2c_timeout_ms);
    return i2c_master_read_from_device(dev->i2c_port, dev->i2c_addr, rx, rx_len, timeout_ticks);
}

static esp_err_t dht20_read_status(const dht20_t *dev, uint8_t *status)
{
    esp_err_t ret = dht20_write(dev, (const uint8_t[]){DHT20_CMD_STATUS}, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    return dht20_read_raw(dev, status, 1);
}

static esp_err_t dht20_parse_sample(const uint8_t raw[DHT20_DATA_LEN], dht20_sample_t *sample)
{
    if (raw[0] & DHT20_STATUS_BUSY_MASK) {
        return ESP_ERR_INVALID_STATE;
    }

    if (dht20_crc8(raw, DHT20_DATA_LEN - 1) != raw[DHT20_DATA_LEN - 1]) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint32_t humidity_raw = ((uint32_t)raw[1] << 12) | ((uint32_t)raw[2] << 4) | ((uint32_t)raw[3] >> 4);
    const uint32_t temperature_raw = (((uint32_t)raw[3] & 0x0F) << 16) | ((uint32_t)raw[4] << 8) | raw[5];

    sample->timestamp_us = (uint64_t)esp_timer_get_time();
    sample->humidity_raw = humidity_raw;
    sample->temperature_raw = temperature_raw;

    sample->humidity_rh = ((float)humidity_raw * 100.0f) / 1048576.0f;
    sample->temperature_c = ((float)temperature_raw * 200.0f) / 1048576.0f - 50.0f;

    return ESP_OK;
}

static float dht20_clampf(float value, float min_v, float max_v)
{
    if (value < min_v) {
        return min_v;
    }
    if (value > max_v) {
        return max_v;
    }
    return value;
}

esp_err_t dht20_soft_reset(const dht20_t *dev)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "dht20", "dev is null");

    esp_err_t ret = dht20_write(dev, (const uint8_t[]){DHT20_CMD_SOFT_RESET}, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(DHT20_SOFT_RESET_DELAY_MS));
    return ESP_OK;
}

esp_err_t dht20_init(dht20_t *dev, i2c_port_t i2c_port, uint8_t i2c_addr, uint32_t i2c_timeout_ms)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "dht20", "dev is null");

    dev->i2c_port = i2c_port;
    dev->i2c_addr = i2c_addr;
    dev->i2c_timeout_ms = i2c_timeout_ms;

    vTaskDelay(pdMS_TO_TICKS(DHT20_POWER_ON_DELAY_MS));

    esp_err_t ret = dht20_soft_reset(dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t status = 0;
    ret = dht20_read_status(dev, &status);
    if (ret != ESP_OK) {
        return ret;
    }

    if ((status & DHT20_STATUS_CAL_MASK) == 0) {
        const uint8_t init_cmd[] = {DHT20_CMD_INIT, DHT20_ARG_INIT_1, DHT20_ARG_INIT_2};
        ret = dht20_write(dev, init_cmd, sizeof(init_cmd));
        if (ret != ESP_OK) {
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(DHT20_STATUS_READY_DELAY_MS));
    }

    return ESP_OK;
}

esp_err_t dht20_start_measurement(const dht20_t *dev)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "dht20", "dev is null");

    const uint8_t trigger_cmd[] = {DHT20_CMD_TRIGGER, DHT20_ARG_TRIGGER_1, DHT20_ARG_TRIGGER_2};
    return dht20_write(dev, trigger_cmd, sizeof(trigger_cmd));
}

esp_err_t dht20_read_measurement(const dht20_t *dev, dht20_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "dht20", "dev is null");
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, "dht20", "sample is null");

    uint8_t raw[DHT20_DATA_LEN] = {0};
    esp_err_t ret = dht20_read_raw(dev, raw, sizeof(raw));
    if (ret != ESP_OK) {
        return ret;
    }

    return dht20_parse_sample(raw, sample);
}

esp_err_t dht20_read_measurement_wait(const dht20_t *dev, dht20_sample_t *sample, uint32_t timeout_ms, uint32_t poll_interval_ms)
{
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_INVALID_ARG, "dht20", "dev is null");
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, "dht20", "sample is null");

    const int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)timeout_ms * 1000LL;
    const TickType_t poll_ticks = pdMS_TO_TICKS((poll_interval_ms == 0) ? 1 : poll_interval_ms);

    while (true) {
        esp_err_t err = dht20_read_measurement(dev, sample);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_INVALID_STATE) {
            return err;
        }

        const int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (elapsed_us >= timeout_us) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(poll_ticks);
    }
}

esp_err_t dht20_read_oneshot(const dht20_t *dev, dht20_sample_t *sample, uint32_t timeout_ms, uint32_t poll_interval_ms)
{
    ESP_RETURN_ON_ERROR(dht20_start_measurement(dev), "dht20", "start_measurement failed");
    return dht20_read_measurement_wait(dev, sample, timeout_ms, poll_interval_ms);
}

esp_err_t dht20_read(const dht20_t *dev, dht20_sample_t *sample, uint32_t conversion_wait_ms)
{
    ESP_RETURN_ON_ERROR(dht20_start_measurement(dev), "dht20", "start_measurement failed");

    if (conversion_wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(conversion_wait_ms));
    }

    return dht20_read_measurement(dev, sample);
}

esp_err_t dht20_sample_apply_offset(dht20_sample_t *sample, float temperature_offset_c, float humidity_offset_rh)
{
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, "dht20", "sample is null");

    sample->temperature_c += temperature_offset_c;
    sample->humidity_rh = dht20_clampf(sample->humidity_rh + humidity_offset_rh, 0.0f, 100.0f);
    return ESP_OK;
}

esp_err_t dht20_filter_init(dht20_filter_t *filter, float alpha)
{
    ESP_RETURN_ON_FALSE(filter != NULL, ESP_ERR_INVALID_ARG, "dht20", "filter is null");
    ESP_RETURN_ON_FALSE(isfinite(alpha), ESP_ERR_INVALID_ARG, "dht20", "alpha is not finite");
    ESP_RETURN_ON_FALSE(alpha > 0.0f && alpha <= 1.0f, ESP_ERR_INVALID_ARG, "dht20", "alpha out of range");

    filter->initialized = false;
    filter->alpha = alpha;
    filter->humidity_rh = 0.0f;
    filter->temperature_c = 0.0f;
    return ESP_OK;
}

void dht20_filter_reset(dht20_filter_t *filter)
{
    if (filter == NULL) {
        return;
    }
    filter->initialized = false;
}

esp_err_t dht20_filter_apply(dht20_filter_t *filter, const dht20_sample_t *input, dht20_sample_t *output)
{
    ESP_RETURN_ON_FALSE(filter != NULL, ESP_ERR_INVALID_ARG, "dht20", "filter is null");
    ESP_RETURN_ON_FALSE(input != NULL, ESP_ERR_INVALID_ARG, "dht20", "input is null");
    ESP_RETURN_ON_FALSE(output != NULL, ESP_ERR_INVALID_ARG, "dht20", "output is null");
    ESP_RETURN_ON_FALSE(filter->alpha > 0.0f && filter->alpha <= 1.0f, ESP_ERR_INVALID_STATE, "dht20", "filter not initialized");

    *output = *input;

    if (!filter->initialized) {
        filter->temperature_c = input->temperature_c;
        filter->humidity_rh = input->humidity_rh;
        filter->initialized = true;
    } else {
        const float one_minus_alpha = 1.0f - filter->alpha;
        filter->temperature_c = (filter->alpha * input->temperature_c) + (one_minus_alpha * filter->temperature_c);
        filter->humidity_rh = (filter->alpha * input->humidity_rh) + (one_minus_alpha * filter->humidity_rh);
    }

    output->temperature_c = filter->temperature_c;
    output->humidity_rh = dht20_clampf(filter->humidity_rh, 0.0f, 100.0f);
    return ESP_OK;
}
