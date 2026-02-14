# SPDX-License-Identifier: 0BSD

idf_component_register(SRCS "src/dht20_api.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
)
