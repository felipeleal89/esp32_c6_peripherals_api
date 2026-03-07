/*
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file sntp_api.h
 * @brief SNTP helper + status bar renderer for display.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define SNTP_API_DEFAULT_SERVER "pool.ntp.org"
#define SNTP_API_MIN_SYNC_INTERVAL_MS 15000U

typedef struct {
    const char *server_name;
    int8_t gmt_offset_hours;
    uint32_t sync_interval_ms;
    uint16_t bar_bg_color;
    uint16_t bar_fg_color;
    uint8_t text_scale; /* base scale (fallback) */
    uint8_t date_scale; /* 0 = auto from text_scale */
    uint8_t time_scale; /* 0 = auto from date_scale */
    uint8_t line_gap_px; /* 0 = auto */
    uint8_t date_char_spacing_px; /* extra spacing between chars */
    uint8_t time_char_spacing_px; /* extra spacing between chars */
} sntp_api_cfg_t;

/** @brief Start SNTP client and configure status bar rendering. */
esp_err_t sntp_api_init(const sntp_api_cfg_t *cfg);
/** @brief Stop SNTP client. */
void sntp_api_deinit(void);
/** @brief Return true when system time looks valid (already synced at least once). */
bool sntp_api_is_time_valid(void);
/** @brief Format status text as "GMT±XX DD.MM.AAAA HH:MM". */
esp_err_t sntp_api_format_status(char *out, size_t out_len);
/** @brief Draw status bar immediately on top area of display. */
void sntp_api_status_bar_draw(void);
/** @brief Draw status bar only if min_period_ms elapsed since last draw. */
void sntp_api_status_bar_update_if_due(uint32_t min_period_ms);

#ifdef __cplusplus
}
#endif
