/*
 * ESP32-C5 协处理器（从机）固件
 *
 * 这一侧不跑任何应用逻辑：esp_hosted 的 CP 模式会自动初始化 WiFi，
 * 把整颗 C5 变成一个「SPI 网卡」，由 S3 通过 RPC 下发 SSID/密码/频段。
 *
 * 【必须做的一件事：初始化 NVS】
 * esp_hosted 的 CP 侧会在收到 host 的 init RPC 时调用 esp_wifi_init()，
 * 而 esp_wifi_init() 需要把 PHY 校准数据等写进 NVS 分区。如果这里的
 * app_main 没有先 nvs_flash_init()，esp_wifi_init() 会直接返回
 * ESP_ERR_NVS_NOT_INITIALIZED (0x1101)，这个错误码会被原样透传给 S3，
 * 主机侧表现就是"C5 未就绪"——看起来像接线问题，实际是软件没初始化。
 */
#include <stdio.h>
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "C5_CP";

void app_main(void)
{
    /* 第一件事：NVS。失败要显式打出来，否则只会看到主机侧莫名其妙的 0x1101 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建 (%s)，擦除后重试", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s (0x%x) —— 主机侧会看到 0x1101", esp_err_to_name(err), err);
    } else {
        ESP_LOGI(TAG, "NVS ready");
    }

    esp_event_loop_create_default();
    ESP_LOGI(TAG, "ESP32-C5 coprocessor ready, waiting for host over SPI");
}
