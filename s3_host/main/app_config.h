#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_CFG_NS "natcfg"

/* 上行认证方式
 *   0 = WPA2/WPA3 个人版（普通密码）
 *   1 = 企业版 802.1X（校园网 / 公司网，PEAP-MSCHAPv2 最常见）
 */
enum {
    STA_AUTH_PSK = 0,
    STA_AUTH_EAP = 1,
};

/* 企业版 EAP 外层方法 */
enum {
    EAP_METHOD_PEAP = 0,   /* PEAP-MSCHAPv2：90% 的校园网/公司网 */
    EAP_METHOD_TTLS = 1,   /* TTLS-PAP/MSCHAPv2：部分高校 */
    EAP_METHOD_TLS  = 2,   /* 客户端证书（需要预置证书，暂未开放上传） */
};

/* TTLS 内层认证 */
enum {
    EAP_PHASE2_MSCHAPV2 = 0,
    EAP_PHASE2_MSCHAP   = 1,
    EAP_PHASE2_PAP      = 2,
    EAP_PHASE2_CHAP     = 3,
};

/* band_mode: 0=自动 1=只用2.4G 2=只用5G（作用在 C5 的射频上）
 * ap_channel: 1~13，本地 2.4G 热点固定信道（与 C5 的上行信道无关，这正是双芯片的好处）
 */
typedef struct {
    char sta_ssid[33];
    /* 家庭/普通热点用 WPA2-PSK 密码；校园网/企业网用下面的 eap_* 字段 */
    char sta_pass[65];
    char ap_ssid[33];
    char ap_pass[65];
    uint8_t band_mode;
    uint8_t ap_channel;

    /* ---- 企业版 802.1X ---- */
    uint8_t eap_enable;     /* 0=PSK 1=EAP，见 STA_AUTH_* */
    uint8_t eap_method;     /* EAP_METHOD_* */
    uint8_t eap_phase2;     /* EAP_PHASE2_*，仅 TTLS 用 */
    uint8_t eap_skip_cert;  /* 1=不校验服务器证书（校园网自签证书的常规做法） */
    char eap_identity[65];  /* 匿名外层身份，留空则用 username */
    char eap_username[65];
    char eap_pass[65];

    /* ---- DNS ---- */
    uint8_t dns_mode;       /* 0=跟随上级（默认） 1=强制自定义 */
    char dns1[16];
    char dns2[16];
} app_cfg_t;

void app_cfg_defaults(app_cfg_t *cfg);
esp_err_t app_cfg_load(app_cfg_t *cfg);
esp_err_t app_cfg_save(const app_cfg_t *cfg);

/* 擦除全部配置（恢复出厂）。成功返回 ESP_OK，调用方自行重启。 */
esp_err_t app_cfg_erase(void);
