/*
 * SPDX-License-Identifier: 0BSD
 */

#include "sntp_api.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "apps/esp_sntp.h"
#include "display_api.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"

#define SNTP_API_MIN_VALID_UNIX_TS 1577836800LL /* 2020-01-01 00:00:00 UTC */

static const char *TAG = "sntp_api";

typedef struct {
    bool initialized;
    int8_t gmt_offset_hours;
    uint32_t sync_interval_ms;
    uint16_t bar_bg_color;
    uint16_t bar_fg_color;
    uint8_t text_scale;
    uint8_t date_scale;
    uint8_t time_scale;
    uint8_t line_gap_px;
    uint8_t date_char_spacing_px;
    uint8_t time_char_spacing_px;
    int64_t last_draw_us;
    char last_line[40];
} sntp_api_ctx_t;

static sntp_api_ctx_t g_sntp = {0};

static void style_from_ctx(sntp_api_style_t *style)
{
    if (style == NULL) {
        return;
    }
    style->bar_bg_color = g_sntp.bar_bg_color;
    style->bar_fg_color = g_sntp.bar_fg_color;
    style->text_scale = g_sntp.text_scale;
    style->date_scale = g_sntp.date_scale;
    style->time_scale = g_sntp.time_scale;
    style->line_gap_px = g_sntp.line_gap_px;
    style->date_char_spacing_px = g_sntp.date_char_spacing_px;
    style->time_char_spacing_px = g_sntp.time_char_spacing_px;
}

static int text_width_px(const char *s, const uint8_t scale, const uint8_t char_spacing_px)
{
    if (s == NULL) {
        return 0;
    }
    const int len = (int)strlen(s);
    if (len <= 0) {
        return 0;
    }

    const int glyph_w = 6 * (int)scale;
    const int extra = (len > 1) ? ((len - 1) * (int)char_spacing_px) : 0;
    return (len * glyph_w) + extra;
}

static void draw_text_scaled_spaced(int x, int y, const char *s, uint16_t rgb565, uint8_t scale, uint8_t char_spacing_px)
{
    if (s == NULL || scale == 0U) {
        return;
    }

    char one[2] = {0};
    int cursor_x = x;
    while (*s != '\0') {
        one[0] = *s;
        display_draw_text_minimal_scaled(cursor_x, y, one, rgb565, scale);
        cursor_x += (6 * (int)scale) + (int)char_spacing_px;
        s++;
    }
}

esp_err_t sntp_api_init(const sntp_api_cfg_t *cfg)
{
    const sntp_api_cfg_t defaults = {
        .server_name = SNTP_API_DEFAULT_SERVER,
        .gmt_offset_hours = 0,
        .sync_interval_ms = 3600000U,
        .bar_bg_color = 0x0000,
        .bar_fg_color = 0xFFFF,
        .text_scale = 1U,
        .date_scale = 0U,
        .time_scale = 0U,
        .line_gap_px = 0U,
        .date_char_spacing_px = 0U,
        .time_char_spacing_px = 0U,
    };
    const sntp_api_cfg_t *use_cfg = (cfg != NULL) ? cfg : &defaults;

    ESP_RETURN_ON_FALSE(use_cfg->server_name != NULL, ESP_ERR_INVALID_ARG, TAG, "server_name is null");
    ESP_RETURN_ON_FALSE(use_cfg->gmt_offset_hours >= -12 && use_cfg->gmt_offset_hours <= 14,
                        ESP_ERR_INVALID_ARG, TAG, "gmt_offset_hours out of range");
    ESP_RETURN_ON_FALSE(use_cfg->text_scale > 0, ESP_ERR_INVALID_ARG, TAG, "text_scale must be >= 1");

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, use_cfg->server_name);

    if (use_cfg->sync_interval_ms >= SNTP_API_MIN_SYNC_INTERVAL_MS) {
        esp_sntp_set_sync_interval(use_cfg->sync_interval_ms);
    }
    esp_sntp_init();

    g_sntp.initialized = true;
    g_sntp.gmt_offset_hours = use_cfg->gmt_offset_hours;
    g_sntp.sync_interval_ms = use_cfg->sync_interval_ms;
    g_sntp.bar_bg_color = use_cfg->bar_bg_color;
    g_sntp.bar_fg_color = use_cfg->bar_fg_color;
    g_sntp.text_scale = use_cfg->text_scale;
    g_sntp.date_scale = use_cfg->date_scale;
    g_sntp.time_scale = use_cfg->time_scale;
    g_sntp.line_gap_px = use_cfg->line_gap_px;
    g_sntp.date_char_spacing_px = use_cfg->date_char_spacing_px;
    g_sntp.time_char_spacing_px = use_cfg->time_char_spacing_px;
    g_sntp.last_draw_us = 0;
    g_sntp.last_line[0] = '\0';

    ESP_LOGI(TAG, "SNTP started server=%s gmt_offset=%+d sync_interval_ms=%" PRIu32,
             use_cfg->server_name, use_cfg->gmt_offset_hours, use_cfg->sync_interval_ms);
    return ESP_OK;
}

void sntp_api_deinit(void)
{
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    g_sntp.initialized = false;
    g_sntp.last_draw_us = 0;
    g_sntp.last_line[0] = '\0';
}

bool sntp_api_is_time_valid(void)
{
    time_t now = 0;
    time(&now);
    return (int64_t)now >= SNTP_API_MIN_VALID_UNIX_TS;
}

esp_err_t sntp_api_format_status(char *out, size_t out_len)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is null");
    ESP_RETURN_ON_FALSE(out_len > 0, ESP_ERR_INVALID_ARG, TAG, "out_len is zero");

    const int offset_h = g_sntp.initialized ? g_sntp.gmt_offset_hours : 0;
    int offset = offset_h;
    if (offset < -99) {
        offset = -99;
    } else if (offset > 99) {
        offset = 99;
    }

    time_t now = 0;
    time(&now);
    now += (time_t)offset * 3600;

    struct tm tm_local = {0};
    gmtime_r(&now, &tm_local);
    int year = tm_local.tm_year + 1900;
    int day = tm_local.tm_mday;
    int month = tm_local.tm_mon + 1;
    if (day < 0) {
        day = 0;
    } else if (day > 99) {
        day = 99;
    }
    if (month < 0) {
        month = 0;
    } else if (month > 99) {
        month = 99;
    }
    if (year < 0) {
        year = 0;
    } else if (year > 9999) {
        year = 9999;
    }

    int n = snprintf(out, out_len,
                     "GMT%+03d %02d.%02d.%04d %02d:%02d",
                     offset,
                     day,
                     month,
                     year,
                     tm_local.tm_hour,
                     tm_local.tm_min);
    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

void sntp_api_status_bar_draw(void)
{
    if (!g_sntp.initialized) {
        return;
    }

    const int w = display_get_width();
    if (w <= 0) {
        return;
    }

    const int offset_h = g_sntp.initialized ? g_sntp.gmt_offset_hours : 0;
    int offset = offset_h;
    if (offset < -99) {
        offset = -99;
    } else if (offset > 99) {
        offset = 99;
    }
    time_t now = 0;
    time(&now);
    now += (time_t)offset * 3600;

    struct tm tm_local = {0};
    gmtime_r(&now, &tm_local);
    int year = tm_local.tm_year + 1900;
    int day = tm_local.tm_mday;
    int month = tm_local.tm_mon + 1;
    if (day < 0) {
        day = 0;
    } else if (day > 99) {
        day = 99;
    }
    if (month < 0) {
        month = 0;
    } else if (month > 99) {
        month = 99;
    }
    if (year < 0) {
        year = 0;
    } else if (year > 9999) {
        year = 9999;
    }

    char line1[24] = {0};
    char line2[8] = {0};
    char key[40] = {0};

    snprintf(line1, sizeof(line1), "GMT%+03d %02d.%02d.%04d",
             offset, day, month, year);
    snprintf(line2, sizeof(line2), "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
    snprintf(key, sizeof(key), "%s|%s", line1, line2);

    if (strcmp(key, g_sntp.last_line) == 0) {
        g_sntp.last_draw_us = esp_timer_get_time();
        return;
    }

    int date_scale = (g_sntp.date_scale > 0U) ? (int)g_sntp.date_scale : ((int)g_sntp.text_scale + 1);
    if (date_scale < 1) {
        date_scale = 1;
    }
    while (date_scale > 1
           && text_width_px(line1, (uint8_t)date_scale, g_sntp.date_char_spacing_px) > (w - 4)) {
        date_scale--;
    }

    int time_scale = (g_sntp.time_scale > 0U) ? (int)g_sntp.time_scale : (date_scale * 2);
    if (time_scale < 1) {
        time_scale = 1;
    }

    const int line1_h = 7 * date_scale;
    const int line2_h = 7 * time_scale;
    const int pad = 2;
    const int gap = (g_sntp.line_gap_px > 0U) ? (int)g_sntp.line_gap_px : (2 + line1_h);
    const int bar_h = pad + line1_h + gap + line2_h + pad;
    int x1 = (w - text_width_px(line1, (uint8_t)date_scale, g_sntp.date_char_spacing_px)) / 2;
    int x2 = (w - text_width_px(line2, (uint8_t)time_scale, g_sntp.time_char_spacing_px)) / 2;
    if (x1 < 0) {
        x1 = 0;
    }
    if (x2 < 0) {
        x2 = 0;
    }
    display_draw_rect(0, 0, w, bar_h, g_sntp.bar_bg_color);
    draw_text_scaled_spaced(x1, pad, line1, g_sntp.bar_fg_color, (uint8_t)date_scale, g_sntp.date_char_spacing_px);
    draw_text_scaled_spaced(x2, pad + line1_h + gap, line2, g_sntp.bar_fg_color, (uint8_t)time_scale, g_sntp.time_char_spacing_px);

    strncpy(g_sntp.last_line, key, sizeof(g_sntp.last_line));
    g_sntp.last_line[sizeof(g_sntp.last_line) - 1] = '\0';
    g_sntp.last_draw_us = esp_timer_get_time();
}

void sntp_api_status_bar_update_if_due(uint32_t min_period_ms)
{
    if (!g_sntp.initialized) {
        return;
    }

    const uint32_t period_ms = (min_period_ms == 0U) ? 1000U : min_period_ms;
    const int64_t now_us = esp_timer_get_time();
    if (g_sntp.last_draw_us != 0 && (now_us - g_sntp.last_draw_us) < ((int64_t)period_ms * 1000LL)) {
        return;
    }

    sntp_api_status_bar_draw();
}

esp_err_t sntp_api_get_style(sntp_api_style_t *out_style)
{
    ESP_RETURN_ON_FALSE(out_style != NULL, ESP_ERR_INVALID_ARG, TAG, "out_style is null");
    ESP_RETURN_ON_FALSE(g_sntp.initialized, ESP_ERR_INVALID_STATE, TAG, "SNTP not initialized");

    style_from_ctx(out_style);
    return ESP_OK;
}

esp_err_t sntp_api_set_style(const sntp_api_style_t *style)
{
    ESP_RETURN_ON_FALSE(style != NULL, ESP_ERR_INVALID_ARG, TAG, "style is null");
    ESP_RETURN_ON_FALSE(g_sntp.initialized, ESP_ERR_INVALID_STATE, TAG, "SNTP not initialized");
    ESP_RETURN_ON_FALSE(style->text_scale > 0U, ESP_ERR_INVALID_ARG, TAG, "text_scale must be >= 1");

    g_sntp.bar_bg_color = style->bar_bg_color;
    g_sntp.bar_fg_color = style->bar_fg_color;
    g_sntp.text_scale = style->text_scale;
    g_sntp.date_scale = style->date_scale;
    g_sntp.time_scale = style->time_scale;
    g_sntp.line_gap_px = style->line_gap_px;
    g_sntp.date_char_spacing_px = style->date_char_spacing_px;
    g_sntp.time_char_spacing_px = style->time_char_spacing_px;

    /* Force redraw even if time text did not change. */
    g_sntp.last_line[0] = '\0';
    g_sntp.last_draw_us = 0;
    return ESP_OK;
}
