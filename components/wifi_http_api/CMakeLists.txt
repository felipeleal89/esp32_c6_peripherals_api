# SPDX-License-Identifier: 0BSD

idf_component_register(SRCS "src/wifi_http_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_http_server nvs_flash json display_api sntp_api
)
