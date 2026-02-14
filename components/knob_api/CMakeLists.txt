# SPDX-License-Identifier: 0BSD

idf_component_register(SRCS "src/knob_api.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
)
