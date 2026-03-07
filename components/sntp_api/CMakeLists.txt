# SPDX-License-Identifier: 0BSD

idf_component_register(SRCS "src/sntp_api.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_timer display_api esp_netif
)
