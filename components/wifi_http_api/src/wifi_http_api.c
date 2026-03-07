/*
 * SPDX-License-Identifier: 0BSD
 */

#include "wifi_http_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_HTTP_API_NVS_NAMESPACE "wifi_http"
#define WIFI_HTTP_API_NVS_STA_SSID "sta_ssid"
#define WIFI_HTTP_API_NVS_STA_PASS "sta_pass"
#define WIFI_HTTP_API_MAX_JSON_BODY 512

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
    "<script>"
    "const j=(u,m,d)=>fetch(u,{method:m,headers:{'Content-Type':'application/json'},body:d?JSON.stringify(d):undefined}).then(r=>r.json());"
    "function show(id,v){document.getElementById(id).textContent=JSON.stringify(v,null,2)}"
    "async function status(){show('out',await j('/api/status','GET'));}"
    "async function scan(){show('scan',await j('/api/scan','GET'));}"
    "async function setMode(){await j('/api/mode','POST',{mode:document.getElementById('mode').value});status();}"
    "async function setAp(){await j('/api/ap','POST',{ssid:ap_ssid.value,password:ap_pass.value,channel:Number(ap_ch.value)});status();}"
    "async function setSta(){await j('/api/sta','POST',{ssid:sta_ssid.value,password:sta_pass.value,connect:true});status();}"
    "async function disconnectSta(){await j('/api/sta/disconnect','POST',{});status();}"
    "status();"
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

static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
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

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &root_uri), TAG, "register / failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &status_uri), TAG, "register /api/status failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &scan_uri), TAG, "register /api/scan failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &mode_uri), TAG, "register /api/mode failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &ap_uri), TAG, "register /api/ap failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &sta_uri), TAG, "register /api/sta failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(g_wifi.httpd, &sta_dis_uri), TAG, "register /api/sta/disconnect failed");

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

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed");

    err = nvs_load_sta_credentials(g_wifi.sta_ssid, sizeof(g_wifi.sta_ssid), g_wifi.sta_pass, sizeof(g_wifi.sta_pass));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "loaded saved STA credentials for SSID=%s", g_wifi.sta_ssid);
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
