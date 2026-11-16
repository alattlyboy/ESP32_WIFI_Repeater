/*
 * 本地 2.4GHz SoftAP —— 跑在 ESP32-S3 自己的射频上。
 * 这里只 include 本地 esp_wifi.h，远端（C5）的 API 在 remote_sta.c 里调用。
 *
 * 2.0 变更：
 *   - 信道可由配置指定（1~13）。因为上行在 C5、热点在 S3，两者信道互不影响，
 *     可以放心把热点钉死在最干净的信道上。
 *   - 暴露客户端明细（MAC / RSSI / PHY 模式），供仪表盘显示。
 *   - 连接数上限 8 → 10。
 */
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "local_ap.h"

static const char *TAG = "AP";
static bool s_started;

esp_err_t local_ap_start(const char *ssid, const char *pass, uint8_t channel)
{
    if (channel < 1 || channel > 13) {
        channel = 6;
    }

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(ssid);
    ap.ap.channel = channel;
    ap.ap.max_connection = 10;
    ap.ap.beacon_interval = 100;

    if (pass && strlen(pass) >= 8) {
        strncpy((char *)ap.ap.password, pass, sizeof(ap.ap.password) - 1);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;   /* 密码太短就开放，配网时方便 */
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_started = true;
    ESP_LOGI(TAG, "SoftAP up: %s (ch%d, auth=%d)", ssid, ap.ap.channel, ap.ap.authmode);
    return ESP_OK;
}

int local_ap_client_count(void)
{
    if (!s_started) {
        return 0;
    }
    wifi_sta_list_t list = {0};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        return 0;
    }
    return list.num;
}

int local_ap_get_clients(ap_client_t *out, int max)
{
    if (!s_started || !out || max <= 0) {
        return 0;
    }
    wifi_sta_list_t list = {0};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        return 0;
    }
    int n = list.num < max ? list.num : max;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].mac, list.sta[i].mac, 6);
        out[i].rssi = list.sta[i].rssi;
        out[i].phy  = list.sta[i].phy_11n ? 2 : (list.sta[i].phy_11g ? 1 : 0);
    }
    return n;
}
