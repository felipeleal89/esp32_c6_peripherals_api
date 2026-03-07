# wifi_http_api

Wi-Fi configuration component for ESP-IDF with embedded HTTP UI and JSON API.

## Features

- Starts Wi-Fi in `AP`, `STA`, or `APSTA` mode
- Creates AP config portal (`http://192.168.4.1/`)
- Provides JSON endpoints to:
  - query status
  - scan nearby APs
  - change Wi-Fi mode
  - configure AP settings
  - configure/connect STA
  - disconnect STA
- Persists STA credentials in NVS

## Public API

Header: `components/wifi_http_api/include/wifi_http_api.h`

- `esp_err_t wifi_http_api_init(const wifi_http_api_cfg_t *cfg);`
- `void wifi_http_api_deinit(void);`
- `bool wifi_http_api_sta_connected(void);`
- `const char *wifi_http_api_sta_ip(void);`

## Defaults

- AP SSID: `ESP32C6-Setup`
- AP password: `12345678`
- AP channel: `1`
- AP max connections: `4`
- Start mode: `WIFI_MODE_APSTA`

## HTTP Endpoints

- `GET /`  
  Returns a simple HTML configuration page.

- `GET /api/status`  
  Returns mode, AP config, STA state and STA IP.

- `GET /api/scan`  
  Triggers a scan and returns AP list (`ssid`, `rssi`, `authmode`).

- `POST /api/mode`  
  Body: `{"mode":"AP"|"STA"|"APSTA"}`

- `POST /api/ap`  
  Body: `{"ssid":"...", "password":"...", "channel":1..13}`

- `POST /api/sta`  
  Body: `{"ssid":"...", "password":"...", "connect":true|false}`

- `POST /api/sta/disconnect`  
  Body: `{}`

## Example

```c
const wifi_http_api_cfg_t wifi_cfg = {
    .ap_ssid = WIFI_HTTP_API_DEFAULT_AP_SSID,
    .ap_password = WIFI_HTTP_API_DEFAULT_AP_PASS,
    .ap_channel = 1,
    .ap_max_connection = 4,
    .start_mode = WIFI_MODE_APSTA,
};

wifi_http_api_init(&wifi_cfg);
```

