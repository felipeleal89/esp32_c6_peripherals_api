# SPDX-License-Identifier: 0BSD

idf_component_register(SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES dht20_api display_api knob_api
)
