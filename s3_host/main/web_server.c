/*
 * HTTP 服务 + JSON API（2.1）
 *
 * 设计红线（1.0 踩过坑，注释保留）：
 *   handler 里绝不调用远端 RPC。httpd 默认单任务串行，一个 handler 卡住
 *   整站（含 css/js）都会无响应，页面就停在“读取中”。
 *   所有远端信息都由 remote_sta.c 的后台任务写缓存，这里只读内存。
 *
 * 2.1：
 *   - /api/config 增加企业版 802.1X 与自定义 DNS 字段
 *   - /api/status 增加 has_ip / reason / eap 状态（前端据此给出人话提示）
 *   - /api/scan 支持 ?force=1 强制重扫（默认走 8 秒缓存，点一下就出结果）
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_wifi.h"          /* 只为扫描结果里的 WIFI_AUTH_*_ENTERPRISE 判断 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "app_config.h"
#include "local_ap.h"
#include "remote_sta.h"
#include "web_server.h"

static const char *TAG = "HTTP";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");

/* EMBED_FILES 会把嵌入段补齐到对齐边界，end 符号把补位的 0x00 也算进去。
 * HTML/CSS 尾部多个 \0 无害，但 JS 尾部多个 \0 是 SyntaxError（整站脚本不执行，
 * 页面就永远停在“连接中”、所有按钮失效）。发送前必须把尾部补位剥掉。
 * 【2.0.1 修复：app.js:367 Invalid or unexpected token】 */
static size_t asset_len(const uint8_t *start, const uint8_t *end)
{
    size_t n = (size_t)(end - start);
    while (n > 0 && start[n - 1] == 0) {
        n--;
    }
    return n;
}

static app_cfg_t s_cfg;

/* ---- 异步扫描状态机（只在本文件内用） ---- */
#define SCAN_MAX 24
enum { SCAN_IDLE = 0, SCAN_RUNNING, SCAN_DONE, SCAN_ERR, SCAN_NOTREADY };

static scan_item_t s_scan_items[SCAN_MAX];
static volatile int s_scan_n;
static volatile int s_scan_state = SCAN_IDLE;
static volatile bool s_scan_busy;
static volatile bool s_scan_force;

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt ? txt : "{}");
    free(txt);
    cJSON_Delete(root);
    return err;
}

static esp_err_t h_index(httpd_req_t *req)
{
    /* 必须禁缓存：否则浏览器拿旧 HTML/JS，固件修好了页面表现却不变 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, asset_len(index_html_start, index_html_end));
}

static esp_err_t h_css(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, (const char *)style_css_start, asset_len(style_css_start, style_css_end));
}

static esp_err_t h_js(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req, (const char *)app_js_start, asset_len(app_js_start, app_js_end));
}

static void add_clients(cJSON *root)
{
    ap_client_t list[10];
    int n = local_ap_get_clients(list, 10);
    cJSON_AddNumberToObject(root, "clients", n);

    cJSON *arr = cJSON_AddArrayToObject(root, "client_list");
    for (int i = 0; i < n; i++) {
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 list[i].mac[0], list[i].mac[1], list[i].mac[2],
                 list[i].mac[3], list[i].mac[4], list[i].mac[5]);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mac", mac);
        cJSON_AddNumberToObject(o, "rssi", list[i].rssi);
        cJSON_AddStringToObject(o, "phy",
                                list[i].phy == 2 ? "11n" : (list[i].phy == 1 ? "11g" : "11b"));
        cJSON_AddItemToArray(arr, o);
    }
}

static esp_err_t h_status(httpd_req_t *req)
{
    /* 这里所有调用都是读内存缓存，不允许出现任何远端 RPC / 阻塞操作 */
    char ip[16];
    remote_sta_ip_str(ip, sizeof(ip));

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "fw", WEB_FW_VER);

    cJSON_AddStringToObject(r, "ap_ssid", s_cfg.ap_ssid);
    cJSON_AddNumberToObject(r, "ap_channel", s_cfg.ap_channel);
    add_clients(r);

    cJSON_AddStringToObject(r, "sta_ssid", s_cfg.sta_ssid);
    cJSON_AddBoolToObject(r, "sta_configured", remote_sta_configured());
    cJSON_AddBoolToObject(r, "sta_linked", remote_sta_linked());
    cJSON_AddBoolToObject(r, "has_ip", remote_sta_has_ip());
    cJSON_AddBoolToObject(r, "remote_ready", remote_sta_ready());
    cJSON_AddNumberToObject(r, "reason", remote_sta_last_reason());
    cJSON_AddNumberToObject(r, "rssi", remote_sta_rssi());
    cJSON_AddNumberToObject(r, "channel", remote_sta_channel());
    cJSON_AddStringToObject(r, "sta_ip", ip);
    cJSON_AddNumberToObject(r, "band_mode", s_cfg.band_mode);

    cJSON_AddNumberToObject(r, "eap_enable", s_cfg.eap_enable);   /* 0=PSK 1=802.1X 2=开放 */
    cJSON_AddNumberToObject(r, "eap_method", s_cfg.eap_method);
    cJSON_AddStringToObject(r, "eap_username", s_cfg.eap_username);
    cJSON_AddStringToObject(r, "eap_identity", s_cfg.eap_identity);
    cJSON_AddNumberToObject(r, "eap_phase2", s_cfg.eap_phase2);
    cJSON_AddBoolToObject(r, "eap_skip_cert", s_cfg.eap_skip_cert ? true : false);

    cJSON_AddNumberToObject(r, "dns_mode", s_cfg.dns_mode);
    cJSON_AddStringToObject(r, "dns1", s_cfg.dns1);
    cJSON_AddStringToObject(r, "dns2", s_cfg.dns2);

    int8_t hist[STA_HIST_LEN];
    int n = remote_sta_rssi_hist(hist, STA_HIST_LEN);
    cJSON *ha = cJSON_AddArrayToObject(r, "hist");
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(ha, cJSON_CreateNumber(hist[i]));
    }

    cJSON_AddNumberToObject(r, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(r, "free_heap", esp_get_free_heap_size());
    return send_json(req, r);
}

static void scan_task(void *arg)
{
    bool force = s_scan_force;
    int n = remote_sta_scan(s_scan_items, SCAN_MAX, force);
    s_scan_n = (n > 0) ? n : 0;
    s_scan_state = (n >= 0) ? SCAN_DONE : SCAN_ERR;
    s_scan_busy = false;
    vTaskDelete(NULL);
}

/* 取 query 里的 force=1（httpd 的 query 解析在 URI 里有 ?，需要自己看 uri） */
static bool want_force(httpd_req_t *req)
{
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    char v[8] = {0};
    if (httpd_query_key_value(q, "force", v, sizeof(v)) == ESP_OK && v[0] == '1') {
        return true;
    }
    return false;
}

static esp_err_t h_scan(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();

    if (!remote_sta_ready()) {
        s_scan_state = SCAN_NOTREADY;
        cJSON_AddNumberToObject(r, "code", -2);
        cJSON_AddStringToObject(r, "state", "notready");
        return send_json(req, r);
    }
    if (s_scan_busy) {
        cJSON_AddNumberToObject(r, "code", 0);
        cJSON_AddStringToObject(r, "state", "running");
        return send_json(req, r);
    }

    s_scan_force = want_force(req);
    s_scan_busy = true;
    s_scan_n = 0;
    s_scan_state = SCAN_RUNNING;
    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 4, NULL) != pdPASS) {
        s_scan_busy = false;
        s_scan_state = SCAN_ERR;
        cJSON_AddNumberToObject(r, "code", -1);
        cJSON_AddStringToObject(r, "state", "err");
        return send_json(req, r);
    }

    cJSON_AddNumberToObject(r, "code", 0);
    cJSON_AddStringToObject(r, "state", "running");
    return send_json(req, r);
}

static esp_err_t h_scan_result(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    const char *st = "idle";
    switch (s_scan_state) {
    case SCAN_RUNNING:   st = "running";  break;
    case SCAN_DONE:      st = "done";     break;
    case SCAN_ERR:       st = "err";      break;
    case SCAN_NOTREADY:  st = "notready"; break;
    default:             st = "idle";     break;
    }
    cJSON_AddStringToObject(r, "state", st);
    cJSON_AddNumberToObject(r, "code", s_scan_state == SCAN_DONE ? 0 : -1);

    cJSON *arr = cJSON_AddArrayToObject(r, "list");
    if (s_scan_state == SCAN_DONE) {
        for (int i = 0; i < s_scan_n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", s_scan_items[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", s_scan_items[i].rssi);
            cJSON_AddNumberToObject(o, "ch", s_scan_items[i].channel);
            cJSON_AddBoolToObject(o, "secure", s_scan_items[i].authmode != 0);
            cJSON_AddBoolToObject(o, "eap", s_scan_items[i].authmode == WIFI_AUTH_WPA2_ENTERPRISE ||
                                              s_scan_items[i].authmode == WIFI_AUTH_WPA_ENTERPRISE ||
                                              s_scan_items[i].authmode == WIFI_AUTH_WPA3_ENTERPRISE ||
                                              s_scan_items[i].authmode == WIFI_AUTH_WPA2_WPA3_ENTERPRISE);
            cJSON_AddItemToArray(arr, o);
        }
    }
    return send_json(req, r);
}

static void reboot_cb(void *arg)
{
    esp_restart();
}

static void schedule_reboot(uint32_t delay_ms)
{
    esp_timer_handle_t t;
    esp_timer_create_args_t args = { .callback = reboot_cb, .name = "reboot" };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)delay_ms * 1000);
    }
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap) {
        return ESP_FAIL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    buf[got] = 0;
    return ESP_OK;
}

static void copy_field(cJSON *root, const char *key, char *dst, size_t dst_sz)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, dst_sz - 1);
        dst[dst_sz - 1] = 0;
    }
}

static void copy_num(cJSON *root, const char *key, uint8_t *dst, int lo, int hi)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(v)) {
        int x = v->valueint;
        *dst = (x >= lo && x <= hi) ? (uint8_t)x : (uint8_t)lo;
    }
}

static esp_err_t h_config(httpd_req_t *req)
{
    char body[896];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    app_cfg_t next = s_cfg;
    copy_field(root, "sta_ssid", next.sta_ssid, sizeof(next.sta_ssid));
    copy_field(root, "sta_pass", next.sta_pass, sizeof(next.sta_pass));
    copy_field(root, "ap_ssid",  next.ap_ssid,  sizeof(next.ap_ssid));
    copy_field(root, "ap_pass",  next.ap_pass,  sizeof(next.ap_pass));

    copy_num(root, "band_mode",  &next.band_mode,  0, 2);
    copy_num(root, "ap_channel", &next.ap_channel, 1, 13);

    /* 企业版 802.1X */
    copy_num(root, "eap_enable",    &next.eap_enable,    0, 2);   /* 0=PSK 1=802.1X 2=开放 */
    copy_num(root, "eap_method",    &next.eap_method,    0, 2);
    copy_num(root, "eap_phase2",    &next.eap_phase2,    0, 3);
    copy_num(root, "eap_skip_cert", &next.eap_skip_cert, 0, 1);
    copy_field(root, "eap_identity", next.eap_identity, sizeof(next.eap_identity));
    copy_field(root, "eap_username", next.eap_username, sizeof(next.eap_username));
    copy_field(root, "eap_pass",     next.eap_pass,     sizeof(next.eap_pass));

    /* DNS */
    copy_num(root, "dns_mode", &next.dns_mode, 0, 1);
    copy_field(root, "dns1", next.dns1, sizeof(next.dns1));
    copy_field(root, "dns2", next.dns2, sizeof(next.dns2));

    cJSON_Delete(root);

    if (next.ap_ssid[0] == 0) {
        strcpy(next.ap_ssid, "ESP32-NAT");
    }
    /* 企业版必须有用户名，留空就退回 PSK，避免用户填一半把自己锁在外面 */
    /* 只有真选了 802.1X 才要求用户名；开放/密码模式不需要 */
    if (next.eap_enable == 1 && next.eap_username[0] == 0) {
        ESP_LOGW(TAG, "802.1X 未填用户名，拒绝保存");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"state\":\"err\",\"msg\":\"802.1X 需要用户名\"}");
    }

    esp_err_t err = app_cfg_save(&next);
    if (err == ESP_OK) {
        s_cfg = next;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "code", err == ESP_OK ? 0 : -1);
    send_json(req, r);

    /* 给 HTTP 响应留时间发出去，再重启生效 */
    schedule_reboot(800);
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "code", 0);
    send_json(req, r);
    schedule_reboot(500);
    return ESP_OK;
}

static esp_err_t h_factory(httpd_req_t *req)
{
    /* 先把配置清掉，再回响应，最后重启 */
    remote_sta_forget();
    esp_err_t err = app_cfg_erase();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "code", err == ESP_OK ? 0 : -1);
    send_json(req, r);

    schedule_reboot(800);
    return ESP_OK;
}

static const httpd_uri_t uris[] = {
    { .uri = "/",                 .method = HTTP_GET,  .handler = h_index       },
    { .uri = "/index.html",       .method = HTTP_GET,  .handler = h_index       },
    { .uri = "/style.css",        .method = HTTP_GET,  .handler = h_css         },
    { .uri = "/app.js",           .method = HTTP_GET,  .handler = h_js          },
    { .uri = "/api/status",       .method = HTTP_GET,  .handler = h_status      },
    { .uri = "/api/scan",         .method = HTTP_GET,  .handler = h_scan        },
    { .uri = "/api/scan_result",  .method = HTTP_GET,  .handler = h_scan_result },
    { .uri = "/api/config",       .method = HTTP_POST, .handler = h_config      },
    { .uri = "/api/reboot",       .method = HTTP_POST, .handler = h_reboot      },
    { .uri = "/api/factory",      .method = HTTP_POST, .handler = h_factory     },
};

esp_err_t web_server_start(const app_cfg_t *cfg)
{
    s_cfg = *cfg;

    httpd_handle_t server = NULL;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.max_uri_handlers = sizeof(uris) / sizeof(uris[0]) + 2;
    c.lru_purge_enable = true;
    c.stack_size = 8192;

    esp_err_t err = httpd_start(&server, &c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: 0x%x", err);
        return err;
    }
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "web UI on http://192.168.4.1/  (fw %s)", WEB_FW_VER);
    return ESP_OK;
}
