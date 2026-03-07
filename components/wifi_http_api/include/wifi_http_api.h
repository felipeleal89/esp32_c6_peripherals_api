/*
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types_generic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_HTTP_API_DEFAULT_AP_SSID "ESP32C6-Setup"
#define WIFI_HTTP_API_DEFAULT_AP_PASS "12345678"

typedef struct {
    const char *ap_ssid;
    const char *ap_password;
    uint8_t ap_channel;
    uint8_t ap_max_connection;
    wifi_mode_t start_mode;
} wifi_http_api_cfg_t;

esp_err_t wifi_http_api_init(const wifi_http_api_cfg_t *cfg);
void wifi_http_api_deinit(void);
bool wifi_http_api_sta_connected(void);
const char *wifi_http_api_sta_ip(void);

#ifdef __cplusplus
}
#endif
