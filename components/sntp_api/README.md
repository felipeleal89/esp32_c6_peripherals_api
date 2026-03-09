# sntp_api

SNTP helper component for ESP-IDF with a top status bar renderer for ST7789 displays.

## Features

- Starts/stops SNTP client (`esp_sntp`)
- Configurable NTP server and sync interval
- Configurable GMT offset (hours)
- Status formatting: `GMT+/-XX DD.MM.YYYY HH:MM`
- 2-line status bar rendering:
  - line 1: `GMTxx DD.MM.YYYY`
  - line 2: centered `HH:MM`
- Visual controls:
  - text color/background color
  - independent date/time scale
  - gap between lines
  - extra spacing between characters (date/time independently)
- Date line auto-fit for narrow displays (reduces date scale if needed)

## Public API

Header: `components/sntp_api/include/sntp_api.h`

- `esp_err_t sntp_api_init(const sntp_api_cfg_t *cfg);`
- `void sntp_api_deinit(void);`
- `bool sntp_api_is_time_valid(void);`
- `esp_err_t sntp_api_format_status(char *out, size_t out_len);`
- `void sntp_api_status_bar_draw(void);`
- `void sntp_api_status_bar_update_if_due(uint32_t min_period_ms);`
- `esp_err_t sntp_api_get_style(sntp_api_style_t *out_style);`
- `esp_err_t sntp_api_set_style(const sntp_api_style_t *style);`

## Config Struct

`sntp_api_cfg_t` fields:

- `server_name`
- `gmt_offset_hours`
- `sync_interval_ms`
- `bar_bg_color`
- `bar_fg_color`
- `text_scale` (base fallback)
- `date_scale` (`0` = auto from base)
- `time_scale` (`0` = auto from date scale)
- `line_gap_px` (`0` = auto)
- `date_char_spacing_px`
- `time_char_spacing_px`

## Example

```c
const sntp_api_cfg_t sntp_cfg = {
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

sntp_api_init(&sntp_cfg);
sntp_api_status_bar_draw();
```
