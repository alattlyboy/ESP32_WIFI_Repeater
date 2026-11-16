#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_config.h"

#define STA_HIST_LEN 60

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
} scan_item_t;

/* 后台任务：刷新 RSSI / 断线重连。必须在 remote_sta_init 之前或之后尽快启动。 */
esp_err_t remote_sta_begin_poll(void);

/* 上电无条件调用：把 C5 的 WiFi 栈拉起来（不连接任何 AP），使扫描可用 */
esp_err_t remote_sta_init(void);

/* 应用配置并发起连接（PSK 或企业版 802.1X，按 cfg->eap_enable 决定） */
esp_err_t remote_sta_apply(const app_cfg_t *cfg);

void remote_sta_forget(void);

/* 以下只读内存缓存，绝不阻塞 —— 会被 HTTP 任务直接调用 */
bool     remote_sta_linked(void);      /* 已关联到上级 AP */
bool     remote_sta_has_ip(void);      /* 已拿到上行 IP —— 只有它为真才真的能上网 */
bool     remote_sta_ready(void);       /* C5 远端栈就绪 */
bool     remote_sta_configured(void);  /* 用户填过上行参数 */
int8_t   remote_sta_rssi(void);
uint8_t  remote_sta_channel(void);
uint16_t remote_sta_last_reason(void); /* 最近一次断开原因码（802.11 reason） */
void     remote_sta_ip_str(char *buf, int n);
int      remote_sta_rssi_hist(int8_t *out, int max);

/* 扫描：结果带 8 秒缓存，force=true 强制重扫 */
int remote_sta_scan(scan_item_t *out, int max, bool force);
