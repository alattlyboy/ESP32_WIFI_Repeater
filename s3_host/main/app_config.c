#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "app_config.h"

static const char *TAG = "CFG";

static uint8_t sane_channel(uint8_t ch)
{
    return (ch >= 1 && ch <= 13) ? ch : 6;
}

void app_cfg_defaults(app_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->ap_ssid, "ESP32-NAT");
    strcpy(cfg->ap_pass, "12345678");
    cfg->ap_channel    = 6;
    cfg->eap_enable    = 0;
    cfg->eap_method    = EAP_METHOD_PEAP;
    cfg->eap_phase2    = EAP_PHASE2_MSCHAPV2;
    cfg->eap_skip_cert = 1;          /* 校园网绝大多数是自签/私有 CA，默认不做校验 */
    cfg->dns_mode      = 0;
    strcpy(cfg->dns1, "223.5.5.5");
    strcpy(cfg->dns2, "119.29.29.29");
}

esp_err_t app_cfg_load(app_cfg_t *cfg)
{
    app_cfg_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(APP_CFG_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no saved config, using defaults");
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = sizeof(cfg->sta_ssid);
    nvs_get_str(h, "sta_ssid", cfg->sta_ssid, &len);
    len = sizeof(cfg->sta_pass);
    nvs_get_str(h, "sta_pass", cfg->sta_pass, &len);
    len = sizeof(cfg->ap_ssid);
    nvs_get_str(h, "ap_ssid", cfg->ap_ssid, &len);
    len = sizeof(cfg->ap_pass);
    nvs_get_str(h, "ap_pass", cfg->ap_pass, &len);
    nvs_get_u8(h, "band", &cfg->band_mode);
    nvs_get_u8(h, "ap_ch", &cfg->ap_channel);

    nvs_get_u8(h, "eap_en", &cfg->eap_enable);
    nvs_get_u8(h, "eap_m", &cfg->eap_method);
    nvs_get_u8(h, "eap_p2", &cfg->eap_phase2);
    nvs_get_u8(h, "eap_skip", &cfg->eap_skip_cert);
    len = sizeof(cfg->eap_identity);
    nvs_get_str(h, "eap_id", cfg->eap_identity, &len);
    len = sizeof(cfg->eap_username);
    nvs_get_str(h, "eap_user", cfg->eap_username, &len);
    len = sizeof(cfg->eap_pass);
    nvs_get_str(h, "eap_pass", cfg->eap_pass, &len);

    nvs_get_u8(h, "dns_m", &cfg->dns_mode);
    len = sizeof(cfg->dns1);
    nvs_get_str(h, "dns1", cfg->dns1, &len);
    len = sizeof(cfg->dns2);
    nvs_get_str(h, "dns2", cfg->dns2, &len);

    nvs_close(h);

    cfg->ap_channel = sane_channel(cfg->ap_channel);
    if (cfg->ap_ssid[0] == 0) {
        strcpy(cfg->ap_ssid, "ESP32-NAT");
    }
    ESP_LOGI(TAG, "loaded: uplink=%s ap=%s ch=%d band=%d auth=%s dns_mode=%d",
             cfg->sta_ssid, cfg->ap_ssid, cfg->ap_channel, cfg->band_mode,
             cfg->eap_enable == 1 ? "802.1X" : (cfg->eap_enable == 2 ? "OPEN" : "PSK"),
             cfg->dns_mode);
    return ESP_OK;
}

esp_err_t app_cfg_save(const app_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(APP_CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "sta_ssid", cfg->sta_ssid);
    nvs_set_str(h, "sta_pass", cfg->sta_pass);
    nvs_set_str(h, "ap_ssid", cfg->ap_ssid);
    nvs_set_str(h, "ap_pass", cfg->ap_pass);
    nvs_set_u8(h, "band", cfg->band_mode);
    nvs_set_u8(h, "ap_ch", sane_channel(cfg->ap_channel));

    nvs_set_u8(h, "eap_en", cfg->eap_enable);
    nvs_set_u8(h, "eap_m", cfg->eap_method);
    nvs_set_u8(h, "eap_p2", cfg->eap_phase2);
    nvs_set_u8(h, "eap_skip", cfg->eap_skip_cert);
    nvs_set_str(h, "eap_id", cfg->eap_identity);
    nvs_set_str(h, "eap_user", cfg->eap_username);
    nvs_set_str(h, "eap_pass", cfg->eap_pass);

    nvs_set_u8(h, "dns_m", cfg->dns_mode);
    nvs_set_str(h, "dns1", cfg->dns1);
    nvs_set_str(h, "dns2", cfg->dns2);

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved (err=%d)", err);
    return err;
}

esp_err_t app_cfg_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(APP_CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "factory reset: config erased (err=%d)", err);
    return err;
}
