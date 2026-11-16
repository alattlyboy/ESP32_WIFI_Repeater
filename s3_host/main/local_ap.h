#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

/* 一个已连上本机热点的客户端 */
typedef struct {
    uint8_t mac[6];
    int8_t  rssi;
    uint8_t phy;      /* 0=11b 1=11g 2=11n */
} ap_client_t;

/* channel: 1~13，非法值回落到 6 */
esp_err_t local_ap_start(const char *ssid, const char *pass, uint8_t channel);

/* 已连客户端数量（不阻塞，可被 HTTP 任务调用） */
int local_ap_client_count(void);

/* 取客户端明细，最多 max 条，返回实际条数 */
int local_ap_get_clients(ap_client_t *out, int max);
