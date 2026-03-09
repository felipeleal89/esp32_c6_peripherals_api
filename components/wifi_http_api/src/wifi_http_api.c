/*
 * SPDX-License-Identifier: 0BSD
 */

#include "wifi_http_api.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "display_api.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sntp_api.h"

#define WIFI_HTTP_API_NVS_NAMESPACE "wifi_http"
#define WIFI_HTTP_API_NVS_STA_SSID "sta_ssid"
#define WIFI_HTTP_API_NVS_STA_PASS "sta_pass"
#define WIFI_HTTP_API_MAX_JSON_BODY 512
#define WIFI_HTTP_API_DEFAULT_BRIGHTNESS 90U
#define WIFI_HTTP_API_MAX_TEXT_SCALE 16U

static const char *TAG = "wifi_http_api";

typedef struct {
    bool initialized;
    bool sta_connected;
    esp_netif_t *netif_sta;
    esp_netif_t *netif_ap;
    httpd_handle_t httpd;
    esp_event_handler_instance_t wifi_evt_inst;
    esp_event_handler_instance_t ip_evt_inst;
    wifi_mode_t mode;
    char sta_ssid[33];
    char sta_pass[65];
    char sta_ip[16];
    char ap_ssid[33];
    char ap_pass[65];
    uint8_t ap_channel;
    uint8_t ap_max_connection;
    uint8_t display_brightness_pct;
    sntp_api_style_t sntp_style_cache;
} wifi_http_api_ctx_t;

static wifi_http_api_ctx_t g_wifi = {0};

static const char *s_web_ui_html =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-C6 Wi-Fi</title><style>body{font-family:monospace;background:#101418;color:#e6edf3;padding:16px}"
    "input,select,button{margin:4px 0;padding:8px;background:#1d2733;color:#e6edf3;border:1px solid #304055;border-radius:6px}"
    "button{cursor:pointer}pre{background:#0b1016;padding:12px;border-radius:8px;overflow:auto}section{border:1px solid #304055;padding:12px;border-radius:10px;margin:10px 0}"
    "</style></head><body><h3>ESP32-C6 Wi-Fi Config</h3>"
    "<section><h4>Status</h4><button onclick='status()'>Refresh</button><pre id='out'>{}</pre></section>"
    "<section><h4>Mode</h4><select id='mode'><option>AP</option><option>STA</option><option>APSTA</option></select>"
    "<button onclick='setMode()'>Apply Mode</button></section>"
    "<section><h4>AP Config</h4>SSID<br><input id='ap_ssid' value='ESP32C6-Setup'><br>Password (blank=open)<br><input id='ap_pass' value='12345678'>"
    "<br>Channel<br><input id='ap_ch' value='1' type='number' min='1' max='13'><br><button onclick='setAp()'>Apply AP</button></section>"
    "<section><h4>STA Config</h4>SSID<br><input id='sta_ssid'><br>Password<br><input id='sta_pass' type='password'>"
    "<br><button onclick='setSta()'>Save+Connect STA</button> <button onclick='disconnectSta()'>Disconnect STA</button>"
    "<br><button onclick='scan()'>Scan APs</button><pre id='scan'></pre></section>"
    "<section><h4>Display / SNTP Bar</h4>"
    "Brightness (0..100)<br><input id='disp_br' value='90' type='number' min='0' max='100'>"
    "<br>Background RGB565 (e.g. 0x0000)<br><input id='bar_bg' value='0x0000'>"
    "<br>Foreground RGB565 (e.g. 0xFFFF)<br><input id='bar_fg' value='0xFFFF'>"
    "<br>Base scale<br><input id='txt_scale' value='1' type='number' min='1' max='16'>"
    "<br>Date scale (0=auto)<br><input id='date_scale' value='0' type='number' min='0' max='16'>"
    "<br>Time scale (0=auto)<br><input id='time_scale' value='0' type='number' min='0' max='16'>"
    "<br>Line gap px (0=auto)<br><input id='line_gap' value='0' type='number' min='0' max='120'>"
    "<br>Date char spacing px<br><input id='date_sp' value='0' type='number' min='0' max='20'>"
    "<br>Time char spacing px<br><input id='time_sp' value='0' type='number' min='0' max='20'>"
    "<br><button onclick='loadDisplay()'>Load Display Config</button> <button onclick='setDisplay()'>Apply Display Config</button>"
    "<pre id='disp'></pre></section>"
    "<script>"
    "const el=id=>document.getElementById(id);"
    "const show=(id,v)=>{el(id).textContent=JSON.stringify(v,null,2);};"
    "const color565=v=>{const n=Number(v)||0;return '0x'+n.toString(16).toUpperCase().padStart(4,'0');};"
    "const j=async(u,m,d)=>{"
    "try{"
    "const r=await fetch(u,{method:m,headers:{'Content-Type':'application/json'},body:d?JSON.stringify(d):undefined});"
    "const t=await r.text();"
    "let o;"
    "try{o=JSON.parse(t);}catch(_){o={ok:false,error:t||r.statusText};}"
    "if(!r.ok&&o.ok===undefined)o.ok=false;"
    "return o;"
    "}catch(e){return {ok:false,error:String(e)};}"
    "};"
    "async function status(){"
    "const s=await j('/api/status','GET');"
    "show('out',s);"
    "if(s&&s.ok){"
    "if(s.mode)el('mode').value=s.mode;"
    "if(s.ap){el('ap_ssid').value=s.ap.ssid||'';el('ap_ch').value=s.ap.channel||1;}"
    "if(s.sta){el('sta_ssid').value=s.sta.ssid||'';}"
    "}"
    "}"
    "async function scan(){show('scan',await j('/api/scan','GET'));}"
    "async function setMode(){show('out',await j('/api/mode','POST',{mode:el('mode').value}));status();}"
    "async function setAp(){show('out',await j('/api/ap','POST',{ssid:el('ap_ssid').value,password:el('ap_pass').value,channel:Number(el('ap_ch').value)}));status();}"
    "async function setSta(){show('out',await j('/api/sta','POST',{ssid:el('sta_ssid').value,password:el('sta_pass').value,connect:true}));status();}"
    "async function disconnectSta(){show('out',await j('/api/sta/disconnect','POST',{}));status();}"
    "async function loadDisplay(){"
    "const d=await j('/api/display','GET');"
    "show('disp',d);"
    "if(!d.ok)return;"
    "el('disp_br').value=d.brightness;"
    "el('bar_bg').value=color565(d.bar_bg_color);"
    "el('bar_fg').value=color565(d.bar_fg_color);"
    "el('txt_scale').value=d.text_scale;"
    "el('date_scale').value=d.date_scale;"
    "el('time_scale').value=d.time_scale;"
    "el('line_gap').value=d.line_gap_px;"
    "el('date_sp').value=d.date_char_spacing_px;"
    "el('time_sp').value=d.time_char_spacing_px;"
    "}"
    "async function setDisplay(){"
    "const d=await j('/api/display','POST',{"
    "brightness:Number(el('disp_br').value),bar_bg_color:el('bar_bg').value,bar_fg_color:el('bar_fg').value,"
    "text_scale:Number(el('txt_scale').value),date_scale:Number(el('date_scale').value),time_scale:Number(el('time_scale').value),"
    "line_gap_px:Number(el('line_gap').value),date_char_spacing_px:Number(el('date_sp').value),time_char_spacing_px:Number(el('time_sp').value),"
    "redraw:true});"
    "show('disp',d);"
    "}"
    "status();"
    "loadDisplay();"
    "</script></body></html>";

static const char *mode_to_str(const wifi_mode_t mode)
{
    switch (mode) {
        case WIFI_MODE_NULL:
            return "NULL";
        case WIFI_MODE_STA:
            return "STA";
        case WIFI_MODE_AP:
            return "AP";
        case WIFI_MODE_APSTA:
            return "APSTA";
        default:
            return "UNKNOWN";
    }
}

static bool mode_from_str(const char *s, wifi_mode_t *mode)
{
    if (s == NULL || mode == NULL) {
        return false;
    }
    if (strcmp(s, "AP") == 0) {
        *mode = WIFI_MODE_AP;
        return true;
    }
    if (strcmp(s, "STA") == 0) {
        *mode = WIFI_MODE_STA;
        return true;
    }
    if (strcmp(s, "APSTA") == 0) {
        *mode = WIFI_MODE_APSTA;
        return true;
    }
    return false;
}

static wifi_auth_mode_t ap_auth_mode_from_pass(const char *pass)
{
    if (pass == NULL || pass[0] == '\0') {
        return WIFI_AUTH_OPEN;
    }
    return WIFI_AUTH_WPA2_PSK;
}

static void sta_ip_to_string(void)
{
    esp_netif_ip_info_t ip_info = {0};
    if (g_wifi.netif_sta != NULL && esp_netif_get_ip_info(g_wifi.netif_sta, &ip_info) == ESP_OK) {
        snprintf(g_wifi.sta_ip, sizeof(g_wifi.sta_ip), IPSTR, IP2STR(&ip_info.ip));
    } else {
        strncpy(g_wifi.sta_ip, "0.0.0.0", sizeof(g_wifi.sta_ip));
        g_wifi.sta_ip[sizeof(g_wifi.sta_ip) - 1] = '\0';
    }
}

static esp_err_t nvs_save_sta_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WIFI_HTTP_API_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, WIFI_HTTP_API_NVS_STA_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_HTTP_API_NVS_STA_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t nvs_load_sta_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WIFI_HTTP_API_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t req_ssid = ssid_len;
    size_t req_pass = pass_len;
    err = nvs_get_str(nvs, WIFI_HTTP_API_NVS_STA_SSID, ssid, &req_ssid);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, WIFI_HTTP_API_NVS_STA_PASS, pass, &req_pass);
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t read_json_body(httpd_req_t *req, char *buf, size_t buf_len, cJSON **out_json)
{
    if (req == NULL || buf == NULL || out_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len <= 0 || req->content_len >= (int)buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_json = root;
    return ESP_OK;
}

static void set_http_status(httpd_req_t *req, int status)
{
    if (status == 200) {
        httpd_resp_set_status(req, "200 OK");
    } else if (status == 400) {
        httpd_resp_set_status(req, "400 Bad Request");
    } else if (status == 500) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
}

static esp_err_t send_json_response(httpd_req_t *req, int status, cJSON *json)
{
    set_http_status(req, status);
    httpd_resp_set_type(req, "application/json");

    char *payload = cJSON_PrintUnformatted(json);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

static esp_err_t send_error_json(httpd_req_t *req, int status, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", (msg != NULL) ? msg : "unknown");
    esp_err_t err = send_json_response(req, status, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t json_read_u8(cJSON *root, const char *key, uint8_t min_v, uint8_t max_v, uint8_t *out, bool *present)
{
    if (root == NULL || key == NULL || out == NULL || present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item == NULL) {
        *present = false;
        return ESP_OK;
    }
    if (!cJSON_IsNumber(item)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int v = item->valueint;
    if (v < (int)min_v || v > (int)max_v) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = (uint8_t)v;
    *present = true;
    return ESP_OK;
}

static esp_err_t json_read_u16_color(cJSON *root, const char *key, uint16_t *out, bool *present)
{
    if (root == NULL || key == NULL || out == NULL || present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item == NULL) {
        *present = false;
        return ESP_OK;
    }

    uint32_t v = 0U;
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0 || item->valuedouble > 65535.0) {
            return ESP_ERR_INVALID_ARG;
        }
        v = (uint32_t)item->valueint;
    } else if (cJSON_IsString(item) && item->valuestring != NULL) {
        char *endptr = NULL;
        errno = 0;
        unsigned long parsed = strtoul(item->valuestring, &endptr, 0);
        if (errno != 0 || endptr == item->valuestring || *endptr != '\0' || parsed > 0xFFFFUL) {
            return ESP_ERR_INVALID_ARG;
        }
        v = (uint32_t)parsed;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    *out = (uint16_t)v;
    *present = true;
    return ESP_OK;
}

static void add_display_cfg_json(cJSON *root, const sntp_api_style_t *style, uint8_t brightness, bool sntp_ready)
{
    if (root == NULL || style == NULL) {
        return;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "sntp_ready", sntp_ready);
    cJSON_AddNumberToObject(root, "brightness", brightness);
    cJSON_AddNumberToObject(root, "bar_bg_color", style->bar_bg_color);
    cJSON_AddNumberToObject(root, "bar_fg_color", style->bar_fg_color);
    cJSON_AddNumberToObject(root, "text_scale", style->text_scale);
    cJSON_AddNumberToObject(root, "date_scale", style->date_scale);
    cJSON_AddNumberToObject(root, "time_scale", style->time_scale);
    cJSON_AddNumberToObject(root, "line_gap_px", style->line_gap_px);
    cJSON_AddNumberToObject(root, "date_char_spacing_px", style->date_char_spacing_px);
    cJSON_AddNumberToObject(root, "time_char_spacing_px", style->time_char_spacing_px);
}

static esp_err_t apply_ap_config(void)
{
    wifi_config_t ap_cfg = {0};
    const size_t ssid_len = strnlen(g_wifi.ap_ssid, sizeof(ap_cfg.ap.ssid));
    memcpy(ap_cfg.ap.ssid, g_wifi.ap_ssid, ssid_len);
    ap_cfg.ap.ssid_len = (uint8_t)ssid_len;
    strlcpy((char *)ap_cfg.ap.password, g_wifi.ap_pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.channel = g_wifi.ap_channel;
    ap_cfg.ap.max_connection = g_wifi.ap_max_connection;
    ap_cfg.ap.authmode = ap_auth_mode_from_pass(g_wifi.ap_pass);
    if (ap_cfg.ap.authmode == WIFI_AUTH_OPEN) {
        ap_cfg.ap.password[0] = '\0';
    }

    return esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

static esp_err_t apply_sta_config_and_optionally_connect(bool connect_now)
{
    wifi_config_t sta_cfg = {0};
    const size_t ssid_len = strnlen(g_wifi.sta_ssid, sizeof(sta_cfg.sta.ssid));
    memcpy(sta_cfg.sta.ssid, g_wifi.sta_ssid, ssid_len);
    strlcpy((char *)sta_cfg.sta.password, g_wifi.sta_pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "esp_wifi_set_config STA failed");

    if (connect_now && g_wifi.sta_ssid[0] != '\0') {
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "esp_wifi_connect failed");
    }

    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi.sta_connected = false;
        sta_ip_to_string();
        ESP_LOGW(TAG, "STA disconnected");
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "STA connected to AP");
    } else if (event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP started SSID=%s", g_wifi.ap_ssid);
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        g_wifi.sta_connected = true;
        snprintf(g_wifi.sta_ip, sizeof(g_wifi.sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: %s", g_wifi.sta_ip);
    }
}

static esp_err_t uri_root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, s_web_ui_html);
}

static esp_err_t uri_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *ap = cJSON_CreateObject();
    cJSON *sta = cJSON_CreateObject();
    if (root == NULL || ap == NULL || sta == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(ap);
        cJSON_Delete(sta);
        return send_error_json(req, 500, "no memory");
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "mode", mode_to_str(mode));

    cJSON_AddStringToObject(ap, "ssid", g_wifi.ap_ssid);
    cJSON_AddNumberToObject(ap, "channel", g_wifi.ap_channel);
    cJSON_AddNumberToObject(ap, "max_connection", g_wifi.ap_max_connection);
    cJSON_AddStringToObject(ap, "ip", "192.168.4.1");

    sta_ip_to_string();
    cJSON_AddBoolToObject(sta, "connected", g_wifi.sta_connected);
    cJSON_AddStringToObject(sta, "ssid", g_wifi.sta_ssid);
    cJSON_AddStringToObject(sta, "ip", g_wifi.sta_ip);

    cJSON_AddItemToObject(root, "ap", ap);
    cJSON_AddItemToObject(root, "sta", sta);

    esp_err_t err = send_json_response(req, 200, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t uri_health_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_error_json(req, 500, "no memory");
    }

    sta_ip_to_string();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "service", "wifi_http_api");
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000LL));
    cJSON_AddBoolToObject(root, "sta_connected", g_wifi.sta_connected);
    cJSON_AddStringToObject(root, "sta_ip", g_wifi.sta_ip);
    cJSON_AddBoolToObject(root, "time_valid", sntp_api_is_time_valid());

    esp_err_t err = send_json_response(req, 200, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t uri_scan_get_handler(httpd_req_t *req)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        return send_error_json(req, 500, "esp_wifi_get_mode failed");
    }

    if (mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            return send_error_json(req, 500, "failed to enable STA for scan");
        }
        mode = WIFI_MODE_APSTA;
    }

    wifi_scan_config_t scan_cfg = {0};
    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        return send_error_json(req, 500, "esp_wifi_scan_start failed");
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        return send_error_json(req, 500, "esp_wifi_scan_get_ap_num failed");
    }

    wifi_ap_record_t *records = NULL;
    if (ap_count > 0) {
        records = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (records == NULL) {
            return send_error_json(req, 500, "no memory");
        }
        err = esp_wifi_scan_get_ap_records(&ap_count, records);
        if (err != ESP_OK) {
            free(records);
            return send_error_json(req, 500, "esp_wifi_scan_get_ap_records failed");
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (root == NULL || items == NULL) {
        free(records);
        cJSON_Delete(root);
        cJSON_Delete(items);
        return send_error_json(req, 500, "no memory");
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "aps", items);

    for (uint16_t i = 0; i < ap_count; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            continue;
        }
        cJSON_AddStringToObject(entry, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(entry, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(entry, "authmode", records[i].authmode);
        cJSON_AddItemToArray(items, entry);
    }

    free(records);
    err = send_json_response(req, 200, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t uri_mode_post_handler(httpd_req_t *req)
{
    char body[WIFI_HTTP_API_MAX_JSON_BODY + 1] = {0};
    cJSON *root = NULL;
    esp_err_t err = read_json_body(req, body, sizeof(body), &root);
    if (err != ESP_OK) {
        return send_error_json(req, 400, "invalid JSON body");
    }

    cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
    wifi_mode_t new_mode = WIFI_MODE_APSTA;
    if (!cJSON_IsString(mode_item) || !mode_from_str(mode_item->valuestring, &new_mode)) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "mode must be AP, STA or APSTA");
    }

    cJSON_Delete(root);

    err = esp_wifi_set_mode(new_mode);
    if (err != ESP_OK) {
        return send_error_json(req, 500, "esp_wifi_set_mode failed");
    }

    g_wifi.mode = new_mode;
    if ((new_mode == WIFI_MODE_STA || new_mode == WIFI_MODE_APSTA) && g_wifi.sta_ssid[0] != '\0') {
        (void)esp_wifi_connect();
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return send_error_json(req, 500, "no memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "mode", mode_to_str(new_mode));
    err = send_json_response(req, 200, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t uri_ap_post_handler(httpd_req_t *req)
{
    char body[WIFI_HTTP_API_MAX_JSON_BODY + 1] = {0};
    cJSON *root = NULL;
    esp_err_t err = read_json_body(req, body, sizeof(body), &root);
    if (err != ESP_OK) {
        return send_error_json(req, 400, "invalid JSON body");
    }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");
    cJSON *chan_item = cJSON_GetObjectItem(root, "channel");

    if (!cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0 || strlen(ssid_item->valuestring) > 32) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "invalid AP ssid");
    }

    const char *new_pass = (cJSON_IsString(pass_item) && pass_item->valuestring != NULL) ? pass_item->valuestring : "";
    if (!(strlen(new_pass) == 0 || strlen(new_pass) >= 8) || strlen(new_pass) > 64) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "AP password must be empty or >=8 chars");
    }

    uint8_t new_channel = g_wifi.ap_channel;
    if (cJSON_IsNumber(chan_item)) {
        int ch = chan_item->valueint;
        if (ch < 1 || ch > 13) {
            cJSON_Delete(root);
            return send_error_json(req, 400, "channel must be between 1 and 13");
        }
        new_channel = (uint8_t)ch;
    }

    snprintf(g_wifi.ap_ssid, sizeof(g_wifi.ap_ssid), "%s", ssid_item->valuestring);
    snprintf(g_wifi.ap_pass, sizeof(g_wifi.ap_pass), "%s", new_pass);
    g_wifi.ap_channel = new_channel;
    cJSON_Delete(root);

    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_STA) {
        (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    err = apply_ap_config();
    if (err != ESP_OK) {
        return send_error_json(req, 500, "failed to apply AP config");
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return send_error_json(req, 500, "no memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "ssid", g_wifi.ap_ssid);
    cJSON_AddNumberToObject(resp, "channel", g_wifi.ap_channel);
    err = send_json_response(req, 200, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t uri_sta_post_handler(httpd_req_t *req)
{
    char body[WIFI_HTTP_API_MAX_JSON_BODY + 1] = {0};
    cJSON *root = NULL;
    esp_err_t err = read_json_body(req, body, sizeof(body), &root);
    if (err != ESP_OK) {
        return send_error_json(req, 400, "invalid JSON body");
    }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");
    cJSON *connect_item = cJSON_GetObjectItem(root, "connect");

    if (!cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0 || strlen(ssid_item->valuestring) > 32) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "invalid STA ssid");
    }

    const char *new_pass = (cJSON_IsString(pass_item) && pass_item->valuestring != NULL) ? pass_item->valuestring : "";
    if (strlen(new_pass) > 64) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "invalid STA password");
    }

    bool connect_now = true;
    if (cJSON_IsBool(connect_item)) {
        connect_now = cJSON_IsTrue(connect_item);
    }

    snprintf(g_wifi.sta_ssid, sizeof(g_wifi.sta_ssid), "%s", ssid_item->valuestring);
    snprintf(g_wifi.sta_pass, sizeof(g_wifi.sta_pass), "%s", new_pass);
    cJSON_Delete(root);

    err = nvs_save_sta_credentials(g_wifi.sta_ssid, g_wifi.sta_pass);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to persist STA credentials: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "STA credentials saved for SSID=%s", g_wifi.sta_ssid);
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    err = apply_sta_config_and_optionally_connect(connect_now);
    if (err != ESP_OK) {
        return send_error_json(req, 500, "failed to apply STA config");
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return send_error_json(req, 500, "no memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "ssid", g_wifi.sta_ssid);
    cJSON_AddBoolToObject(resp, "connecting", connect_now);
    err = send_json_response(req, 200, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t uri_sta_disconnect_post_handler(httpd_req_t *req)
{
    (void)esp_wifi_disconnect();
    g_wifi.sta_connected = false;
    sta_ip_to_string();

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return send_error_json(req, 500, "no memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    esp_err_t err = send_json_response(req, 200, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t uri_display_get_handler(httpd_req_t *req)
{
    sntp_api_style_t style = g_wifi.sntp_style_cache;
    const bool sntp_ready = (sntp_api_get_style(&style) == ESP_OK);
    if (sntp_ready) {
        g_wifi.sntp_style_cache = style;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_error_json(req, 500, "no memory");
    }

    add_display_cfg_json(root, &style, g_wifi.display_brightness_pct, sntp_ready);
    esp_err_t err = send_json_response(req, 200, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t uri_display_post_handler(httpd_req_t *req)
{
    char body[WIFI_HTTP_API_MAX_JSON_BODY + 1] = {0};
    cJSON *root = NULL;
    esp_err_t err = read_json_body(req, body, sizeof(body), &root);
    if (err != ESP_OK) {
        return send_error_json(req, 400, "invalid JSON body");
    }

    sntp_api_style_t style = g_wifi.sntp_style_cache;
    bool sntp_ready = (sntp_api_get_style(&style) == ESP_OK);

    bool has_brightness = false;
    uint8_t brightness = g_wifi.display_brightness_pct;
    err = json_read_u8(root, "brightness", 0U, 100U, &brightness, &has_brightness);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "brightness must be 0..100");
    }

    bool has_bg = false;
    uint16_t bg = style.bar_bg_color;
    err = json_read_u16_color(root, "bar_bg_color", &bg, &has_bg);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "bar_bg_color must be RGB565 (0..65535 or 0xNNNN)");
    }

    bool has_fg = false;
    uint16_t fg = style.bar_fg_color;
    err = json_read_u16_color(root, "bar_fg_color", &fg, &has_fg);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "bar_fg_color must be RGB565 (0..65535 or 0xNNNN)");
    }

    bool has_text_scale = false;
    uint8_t text_scale = style.text_scale;
    err = json_read_u8(root, "text_scale", 1U, WIFI_HTTP_API_MAX_TEXT_SCALE, &text_scale, &has_text_scale);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "text_scale must be 1..16");
    }

    bool has_date_scale = false;
    uint8_t date_scale = style.date_scale;
    err = json_read_u8(root, "date_scale", 0U, WIFI_HTTP_API_MAX_TEXT_SCALE, &date_scale, &has_date_scale);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "date_scale must be 0..16");
    }

    bool has_time_scale = false;
    uint8_t time_scale = style.time_scale;
    err = json_read_u8(root, "time_scale", 0U, WIFI_HTTP_API_MAX_TEXT_SCALE, &time_scale, &has_time_scale);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "time_scale must be 0..16");
    }

    bool has_gap = false;
    uint8_t line_gap = style.line_gap_px;
    err = json_read_u8(root, "line_gap_px", 0U, 120U, &line_gap, &has_gap);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "line_gap_px must be 0..120");
    }

    bool has_date_sp = false;
    uint8_t date_spacing = style.date_char_spacing_px;
    err = json_read_u8(root, "date_char_spacing_px", 0U, 20U, &date_spacing, &has_date_sp);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "date_char_spacing_px must be 0..20");
    }

    bool has_time_sp = false;
    uint8_t time_spacing = style.time_char_spacing_px;
    err = json_read_u8(root, "time_char_spacing_px", 0U, 20U, &time_spacing, &has_time_sp);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(req, 400, "time_char_spacing_px must be 0..20");
    }

    bool redraw = true;
    cJSON *redraw_item = cJSON_GetObjectItem(root, "redraw");
    if (cJSON_IsBool(redraw_item)) {
        redraw = cJSON_IsTrue(redraw_item);
    }
    cJSON_Delete(root);

    if (has_brightness) {
        g_wifi.display_brightness_pct = brightness;
        display_backlight_set(g_wifi.display_brightness_pct);
    }

    const bool has_style_change = has_bg || has_fg || has_text_scale || has_date_scale
                                  || has_time_scale || has_gap || has_date_sp || has_time_sp;
    if (has_style_change) {
        style.bar_bg_color = bg;
        style.bar_fg_color = fg;
        style.text_scale = text_scale;
        style.date_scale = date_scale;
        style.time_scale = time_scale;
        style.line_gap_px = line_gap;
        style.date_char_spacing_px = date_spacing;
        style.time_char_spacing_px = time_spacing;
        g_wifi.sntp_style_cache = style;

        err = sntp_api_set_style(&style);
        if (err == ESP_OK) {
            sntp_ready = true;
        } else {
            sntp_ready = false;
            ESP_LOGW(TAG, "sntp_api_set_style failed: %s", esp_err_to_name(err));
        }
    }

    if (redraw && sntp_ready) {
        sntp_api_status_bar_draw();
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return send_error_json(req, 500, "no memory");
    }

    add_display_cfg_json(resp, &g_wifi.sntp_style_cache, g_wifi.display_brightness_pct, sntp_ready);
    if (!sntp_ready) {
        cJSON_AddStringToObject(resp, "warning", "SNTP style not applied yet (SNTP not initialized)");
    }
    err = send_json_response(req, 200, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&g_wifi.httpd, &cfg), TAG, "httpd_start failed");

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = uri_root_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = uri_status_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t health_uri = {
        .uri = "/api/health",
        .method = HTTP_GET,
        .handler = uri_health_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t scan_uri = {
        .uri = "/api/scan",
        .method = HTTP_GET,
        .handler = uri_scan_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t mode_uri = {
        .uri = "/api/mode",
        .method = HTTP_POST,
        .handler = uri_mode_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t ap_uri = {
        .uri = "/api/ap",
        .method = HTTP_POST,
        .handler = uri_ap_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t sta_uri = {
        .uri = "/api/sta",
        .method = HTTP_POST,
        .handler = uri_sta_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t sta_dis_uri = {
        .uri = "/api/sta/disconnect",
        .method = HTTP_POST,
        .handler = uri_sta_disconnect_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t display_get_uri = {
        .uri = "/api/display",
        .method = HTTP_GET,
        .handler = uri_display_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t display_post_uri = {
        .uri = "/api/display",
        .method = HTTP_POST,
        .handler = uri_display_post_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &root_uri), TAG, "register / failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &status_uri), TAG, "register /api/status failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &health_uri), TAG, "register /api/health failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &scan_uri), TAG, "register /api/scan failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &mode_uri), TAG, "register /api/mode failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &ap_uri), TAG, "register /api/ap failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &sta_uri), TAG, "register /api/sta failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &sta_dis_uri), TAG, "register /api/sta/disconnect failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &display_get_uri), TAG, "register /api/display GET failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &display_post_uri), TAG, "register /api/display POST failed");

    return ESP_OK;
}

esp_err_t wifi_http_api_init(const wifi_http_api_cfg_t *cfg)
{
    if (g_wifi.initialized) {
        return ESP_OK;
    }

    const wifi_http_api_cfg_t defaults = {
        .ap_ssid = WIFI_HTTP_API_DEFAULT_AP_SSID,
        .ap_password = WIFI_HTTP_API_DEFAULT_AP_PASS,
        .ap_channel = 1,
        .ap_max_connection = 4,
        .start_mode = WIFI_MODE_APSTA,
    };
    const wifi_http_api_cfg_t *use_cfg = (cfg != NULL) ? cfg : &defaults;

    snprintf(g_wifi.ap_ssid, sizeof(g_wifi.ap_ssid), "%s", (use_cfg->ap_ssid != NULL) ? use_cfg->ap_ssid : defaults.ap_ssid);
    snprintf(g_wifi.ap_pass, sizeof(g_wifi.ap_pass), "%s", (use_cfg->ap_password != NULL) ? use_cfg->ap_password : defaults.ap_password);
    g_wifi.ap_channel = (use_cfg->ap_channel >= 1 && use_cfg->ap_channel <= 13) ? use_cfg->ap_channel : defaults.ap_channel;
    g_wifi.ap_max_connection = (use_cfg->ap_max_connection > 0) ? use_cfg->ap_max_connection : defaults.ap_max_connection;
    g_wifi.mode = use_cfg->start_mode;
    g_wifi.sta_ssid[0] = '\0';
    g_wifi.sta_pass[0] = '\0';
    strncpy(g_wifi.sta_ip, "0.0.0.0", sizeof(g_wifi.sta_ip));
    g_wifi.display_brightness_pct = WIFI_HTTP_API_DEFAULT_BRIGHTNESS;
    g_wifi.sntp_style_cache = (sntp_api_style_t){
        .bar_bg_color = 0x0000,
        .bar_fg_color = 0xFFFF,
        .text_scale = 1U,
        .date_scale = 0U,
        .time_scale = 0U,
        .line_gap_px = 0U,
        .date_char_spacing_px = 0U,
        .time_char_spacing_px = 0U,
    };

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed");

    err = nvs_load_sta_credentials(g_wifi.sta_ssid, sizeof(g_wifi.sta_ssid), g_wifi.sta_pass, sizeof(g_wifi.sta_pass));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "loaded saved STA credentials for SSID=%s", g_wifi.sta_ssid);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved STA credentials in NVS");
    } else {
        ESP_LOGW(TAG, "failed to load STA credentials from NVS: %s", esp_err_to_name(err));
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &g_wifi.wifi_evt_inst),
                        TAG, "register WIFI_EVENT failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL, &g_wifi.ip_evt_inst),
                        TAG, "register IP_EVENT failed");

    g_wifi.netif_sta = esp_netif_create_default_wifi_sta();
    g_wifi.netif_ap = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(g_wifi.netif_sta != NULL && g_wifi.netif_ap != NULL, ESP_FAIL, TAG, "failed to create netifs");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(g_wifi.mode), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(apply_ap_config(), TAG, "apply AP config failed");

    if (g_wifi.sta_ssid[0] != '\0') {
        ESP_RETURN_ON_ERROR(apply_sta_config_and_optionally_connect(false), TAG, "apply saved STA config failed");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    if ((g_wifi.mode == WIFI_MODE_STA || g_wifi.mode == WIFI_MODE_APSTA) && g_wifi.sta_ssid[0] != '\0') {
        (void)esp_wifi_connect();
    }

    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "start_http_server failed");

    g_wifi.initialized = true;
    ESP_LOGI(TAG, "Wi-Fi HTTP API ready");
    ESP_LOGI(TAG, "AP SSID=%s pass=%s", g_wifi.ap_ssid, (g_wifi.ap_pass[0] != '\0') ? g_wifi.ap_pass : "<open>");
    ESP_LOGI(TAG, "Config page: http://192.168.4.1/");
    return ESP_OK;
}

void wifi_http_api_deinit(void)
{
    if (!g_wifi.initialized) {
        return;
    }

    if (g_wifi.httpd != NULL) {
        httpd_stop(g_wifi.httpd);
        g_wifi.httpd = NULL;
    }

    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, g_wifi.ip_evt_inst);
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, g_wifi.wifi_evt_inst);

    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();

    g_wifi.initialized = false;
    g_wifi.sta_connected = false;
}

bool wifi_http_api_sta_connected(void)
{
    return g_wifi.sta_connected;
}

const char *wifi_http_api_sta_ip(void)
{
    return g_wifi.sta_ip;
}
