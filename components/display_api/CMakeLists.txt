# SPDX-License-Identifier: 0BSD

idf_component_register(SRCS "src/display_api.c"
                            "src/display_image.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd driver freertos
)
