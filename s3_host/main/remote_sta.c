/*
 * 远端 STA —— 射频在 ESP32-C5 上，数据经 SPI 回到 S3。
 *
 * 【1.0/2.0 踩过的坑，别改回去】
 *
 * 1) HTTP 任务不能直接调用远端 RPC。esp-hosted 的 RPC 在从机未就绪时会长时间
 *    阻塞，而 IDF 的 httpd 是单任务串行 —— 一个 handler 卡住整站都挂。
 *    所以状态由本文件的后台任务轮询写缓存，handler 只读变量。
 *
 * 2) 初始化 / 连接必须分两步：上电无条件 init+start 让射频可扫描，
 *    否则首次配网会死锁（没 SSID 不 init → 不能扫 → 永远没 SSID）。
 *
 * 【2.1 重写：把“能不能上网”做成确定的状态机】
 *
 * 老版本靠定时 get_ap_info() 猜链路状态，一次 RPC 抖动就被判成“掉线”，
 * 然后无条件 set_config+connect() 把一条好端端的连接拆掉重建 —— 表现就是
 * 手机连着 S3 的热点却一直上不了网（上行在反复重建，DHCP/NAT 来不及收敛）。
 *
 * 现在：
 *   - 链路状态只信事件（STA_CONNECTED / STA_DISCONNECTED / GOT_IP），
 *     RPC 只用来读 RSSI，读失败不影响链路判定。
 *   - 掉线走指数退避重连（1s→2s→…→30s），连通后自动复位。
 *   - 关联上了但 15 秒拿不到 IP，才主动重连（DHCP 失败的唯一判据）。
 *   - 记录 802.11 断开原因码，网页直接显示（密码错/找不到 AP/握手超时…）。
 *
 * 同时加入：
 *   - 企业版 802.1X（PEAP / TTLS / TLS），走 C5 的 EAP RPC
 *   - 快速扫描：主动扫描 + 每信道 40~80ms + 按频段限定信道位图 + 8 秒结果缓存
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "esp_eap_client_remote_api.h"
#include "remote_sta.h"

static const char *TAG = "STA_RMT";

#define POLL_MS            1000
#define RSSI_EVERY_MS      2000
#define NO_IP_TIMEOUT_MS   15000
#define BACKOFF_MIN_MS     1000
#define BACKOFF_MAX_MS     30000
#define SCAN_CACHE_MS      8000

/* 扫描信道位图：bit0=整段跳过，bitN 对应具体信道（见 wifi_2g/5g_channel_bit_t）
 *
 * 【为什么不能直接 0x1FFFFFFE 全扫】那个位图把 DFS 信道（52~64、100~144）也算进去了。
 * DFS 是雷达频段，必须**被动**驻留 110ms 以上才合法，一进一出就是十几秒 ——
 * 实测全位图扫描 10.5 秒，罪魁祸首就是它，而不是我们设的 40/80ms。
 * 家用/校园 5G 绝大多数落在 36~48、149~165，所以默认只扫这 9 个常用信道；
 * 一个都没扫到时再回退全位图（宁可慢一次，也别漏掉 DFS 上的 AP）。 */
#define BM_2G_ALL    0x3FFE      /* 信道 1~13 */
#define BM_5G_FAST   0x3E0001E   /* 36/40/44/48 + 149/153/157/161/165（非 DFS） */
#define BM_5G_ALL    0x1FFFFFFE  /* 信道 36~177，含 DFS，慢但全 */
#define BM_BYPASS    0x0001

static volatile bool s_ready;
static volatile bool s_linked;      /* 已关联（来自事件） */
static volatile bool s_has_ip;      /* 已拿到 IP（来自事件）—— 真能上网的判据 */
static volatile bool s_scanning;
static volatile int8_t s_rssi;
static volatile uint8_t s_chan;
static volatile uint16_t s_reason;
static char s_ip[16] = "0.0.0.0";

/* 已保存的上行参数（自动重连要用） */
static app_cfg_t s_cfg;
static volatile bool s_configured;

static volatile bool s_eap_active;         /* 上一次是否用了企业版，用来决定要不要 disable */
static volatile bool s_own_disc;           /* 是我们自己 disconnect 触发的断开 */
static volatile uint32_t s_connected_at;   /* 关联成功时刻 */
static volatile uint32_t s_connect_req;    /* 最近一次发起 connect 的时刻 */
static uint32_t s_retry_at;                /* 下次允许重连的时刻 */
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static uint32_t s_attempts;

/* 扫描缓存 */
static scan_item_t s_cache[24];
static int s_cache_n;
static uint32_t s_cache_at;
static uint32_t s_cache_ok_at;

/* 信号历史：环形缓冲 */
static volatile int8_t s_hist[STA_HIST_LEN];
static volatile int s_hist_head;
static volatile int s_hist_cnt;

static void hist_push(int8_t v)
{
    s_hist[s_hist_head] = v;
    s_hist_head = (s_hist_head + 1) % STA_HIST_LEN;
    if (s_hist_cnt < STA_HIST_LEN) {
        s_hist_cnt++;
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static wifi_band_mode_t to_band_mode(uint8_t v)
{
    switch (v) {
    case 1:  return WIFI_BAND_MODE_2G_ONLY;
    case 2:  return WIFI_BAND_MODE_5G_ONLY;
    default: return WIFI_BAND_MODE_AUTO;
    }
}

static void ip_reset(void)
{
    s_has_ip = false;
    strcpy(s_ip, "0.0.0.0");
}

/* ---------------- 事件：链路状态的唯一真相来源 ---------------- */

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) {
        return;
    }
    switch (id) {
    case WIFI_EVENT_STA_CONNECTED:
        s_linked     = true;
        s_reason     = 0;
        s_connected_at = now_ms();
        s_attempts   = 0;
        ESP_LOGI(TAG, "uplink associated");
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        s_reason = d ? d->reason : 0;
        s_linked = false;
        ip_reset();
        s_rssi = 0;
        s_chan = 0;

        /* do_connect() 换 AP 前会主动 disconnect，这个事件不是“掉线”，
         * 不能拿它重置退避计时 —— 否则会变成每秒 connect/disconnect 自激循环。 */
        if (s_own_disc) {
            s_own_disc = false;
            break;
        }

        /* 立刻开始退避计时，别让 UI 干等 10 秒 */
        s_backoff_ms = BACKOFF_MIN_MS;
        s_retry_at   = now_ms() + 800;
        if (s_configured) {
            ESP_LOGW(TAG, "uplink down, reason=%u (%s)", s_reason,
                     s_reason == WIFI_REASON_NO_AP_FOUND ? "NO_AP_FOUND" :
                     s_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ? "4WAY_HANDSHAKE_TIMEOUT/密码错" :
                     s_reason == WIFI_REASON_AUTH_FAIL ? "AUTH_FAIL" :
                     s_reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ?
                         "安全模式不匹配：这网络多半是开放的(Portal)，认证方式改选「开放网络」" :
                     s_reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ? "认证方式不匹配(开了企业版却没配EAP?)" :
                     s_reason == WIFI_REASON_ASSOC_FAIL ? "ASSOC_FAIL" : "see reason code");
        }
        break;
    }
    default:
        break;
    }
}

static void ip_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != IP_EVENT) {
        return;
    }
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        if (ev) {
            esp_ip4addr_ntoa(&ev->ip_info.ip, s_ip, sizeof(s_ip));
            s_has_ip = true;
            s_backoff_ms = BACKOFF_MIN_MS;
            s_retry_at = 0;
            ESP_LOGI(TAG, "uplink got ip: %s", s_ip);
        }
    } else if (id == IP_EVENT_STA_LOST_IP) {
        ip_reset();
    }
}

/* ---------------- 企业版 802.1X ---------------- */

static esp_eap_ttls_phase2_types to_phase2(uint8_t v)
{
    switch (v) {
    case EAP_PHASE2_MSCHAP: return ESP_EAP_TTLS_PHASE2_MSCHAP;
    case EAP_PHASE2_PAP:    return ESP_EAP_TTLS_PHASE2_PAP;
    case EAP_PHASE2_CHAP:   return ESP_EAP_TTLS_PHASE2_CHAP;
    default:                return ESP_EAP_TTLS_PHASE2_MSCHAPV2;
    }
}

/* 把企业版参数下发给 C5 的 supplicant。返回非 0 说明 C5 固件没编进
 * ESP_HOSTED_CP_FEAT_WIFI_EXT_ENT / ESP_WIFI_ENTERPRISE_SUPPORT。 */
static esp_err_t eap_apply(const app_cfg_t *c)
{
    const char *user = c->eap_username[0] ? c->eap_username : c->sta_ssid;
    const char *ident = c->eap_identity[0] ? c->eap_identity : user;
    esp_err_t err = ESP_OK, e;

    if ((e = esp_eap_client_remote_set_identity((const unsigned char *)ident, strlen(ident))) != ESP_OK) {
        err = e;
    }
    if ((e = esp_eap_client_remote_set_username((const unsigned char *)user, strlen(user))) != ESP_OK) {
        err = e;
    }
    if (c->eap_pass[0] &&
        (e = esp_eap_client_remote_set_password((const unsigned char *)c->eap_pass,
                                                strlen(c->eap_pass))) != ESP_OK) {
        err = e;
    }

    /* 指定外层方法（老固件没有这个 RPC，失败就当默认 PEAP） */
    if (c->eap_method == EAP_METHOD_TTLS) {
        esp_eap_client_remote_set_eap_methods(ESP_EAP_TYPE_TTLS);
        esp_eap_client_remote_set_ttls_phase2_method(to_phase2(c->eap_phase2));
    } else if (c->eap_method == EAP_METHOD_TLS) {
        esp_eap_client_remote_set_eap_methods(ESP_EAP_TYPE_TLS);
    } else {
        esp_eap_client_remote_set_eap_methods(ESP_EAP_TYPE_PEAP);
    }

    if (c->eap_skip_cert) {
        /* 不校验服务器证书时顺手关掉时间检查：校园网证书过期是常态，
         * 不关的话会卡在 AUTH_FAIL。 */
        esp_eap_client_remote_set_disable_time_check(true);
    }

    e = esp_wifi_remote_sta_enterprise_enable();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "enterprise enable failed: 0x%x —— C5 固件要打开 "
                      "ESP_HOSTED_CP_FEAT_WIFI_EXT_ENT + ESP_WIFI_ENTERPRISE_SUPPORT", e);
        return e;
    }
    ESP_LOGI(TAG, "EAP enabled: method=%d id=%s user=%s skip_cert=%d",
             c->eap_method, ident, user, c->eap_skip_cert);
    return err;
}

/* ---------------- 连接 ---------------- */

static void do_connect(bool count_attempt)
{
    if (!s_ready || !s_configured || s_cfg.sta_ssid[0] == 0) {
        return;
    }

    esp_wifi_remote_set_band_mode(to_band_mode(s_cfg.band_mode));

    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, s_cfg.sta_ssid, sizeof(sta.sta.ssid) - 1);
    if (s_cfg.eap_enable == 1) {
        /* 企业版：password 字段必须留空，凭据走 EAP 通道。
         * 阈值必须是 ENTERPRISE，否则拿 PSK 去和 AP 协商直接 reason=210。 */
        sta.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
    } else if (s_cfg.eap_enable == 2) {
        /* 开放 + 网页认证（Portal）：没有 WiFi 密码。这是绝大多数校园网的真实形态
         * （Windows 里看就是 authentication=open / encryption=none / useOneX=false）。
         * 连上后浏览器会被带到认证页，认证一次，NAT 后所有设备共用出口 IP。 */
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        strncpy((char *)sta.sta.password, s_cfg.sta_pass, sizeof(sta.sta.password) - 1);
        /* 阈值 OPEN：WPA2/WPA3 混合的路由器也能配上 */
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t err = esp_wifi_remote_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: 0x%x", err);
        return;
    }

    if (s_cfg.eap_enable == 1) {
        eap_apply(&s_cfg);
        s_eap_active = true;
    } else if (s_eap_active) {
        esp_wifi_remote_sta_enterprise_disable();
        s_eap_active = false;
    }

    /* 已经连着才需要先断开（换 AP）；没连上时再 disconnect 只会白白触发一次
     * DISCONNECTED 事件，把退避计时重置成 0 —— 老版本就是在这里打成死循环的。 */
    if (s_linked) {
        s_own_disc = true;
        esp_wifi_remote_disconnect();
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    err = esp_wifi_remote_connect();
    s_connect_req = now_ms();

    if (count_attempt) {
        s_attempts++;
        if (s_attempts <= 3 || (s_attempts % 10) == 0) {
            ESP_LOGW(TAG, "connect '%s' (attempt %u, backoff %ums)",
                     s_cfg.sta_ssid, s_attempts, s_backoff_ms);
        }
    }
    if (err != ESP_OK && err != 0x3007 /* ESP_ERR_WIFI_CONN：上一次连接还在飞 */) {
        ESP_LOGE(TAG, "connect failed: 0x%x", err);
    }
}

/* ---------------- 后台任务 ---------------- */

static void link_poll_task(void *arg)
{
    uint32_t last_rssi = 0;

    for (;;) {
        uint32_t now = now_ms();

        if (s_ready) {
            /* RSSI：只在已关联时读，读失败只当没有数据，绝不影响链路状态 */
            if (s_linked && now - last_rssi >= RSSI_EVERY_MS) {
                last_rssi = now;
                wifi_ap_record_t rec = {0};
                if (esp_wifi_remote_sta_get_ap_info(&rec) == ESP_OK) {
                    s_rssi = rec.rssi;
                    s_chan = rec.primary;
                }
            }

            /* 关联上但迟迟没有 IP → DHCP 没成，重连一次（不是链路抖动） */
            if (s_linked && !s_has_ip && !s_scanning && s_connected_at &&
                now - s_connected_at > NO_IP_TIMEOUT_MS) {
                ESP_LOGW(TAG, "associated but no ip in %ds, reconnecting",
                         NO_IP_TIMEOUT_MS / 1000);
                s_connected_at = 0;
                esp_wifi_remote_disconnect();
                s_linked = false;
                s_retry_at = now + 500;
            }

            if (s_linked && !s_has_ip && s_connect_req &&
                now - s_connect_req > NO_IP_TIMEOUT_MS + 5000) {
                /* connect 请求一直没回事件，兜底重发 */
                s_connect_req = now;
                do_connect(true);
            }

            /* 掉线重连：指数退避，扫描期间让路 */
            if (s_configured && !s_linked && !s_scanning && (int32_t)(now - s_retry_at) >= 0) {
                s_backoff_ms = s_backoff_ms * 2;
                if (s_backoff_ms > BACKOFF_MAX_MS) {
                    s_backoff_ms = BACKOFF_MAX_MS;
                }
                s_retry_at = now + s_backoff_ms;
                do_connect(true);
            }
        }

        hist_push(s_has_ip ? s_rssi : 0);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t remote_sta_begin_poll(void)
{
    BaseType_t ok = xTaskCreate(link_poll_task, "sta_poll", 4096, NULL, 3, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* 第一步：把 C5 的 WiFi 栈拉起来（不连接任何 AP），使扫描可用。 */
esp_err_t remote_sta_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register wifi evt failed: 0x%x", err);
    }
    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_evt, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register ip evt failed: 0x%x", err);
    }

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_remote_init(&ic);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "remote init failed: 0x%x —— 检查 SPI 接线 / C5 供电 / C5 是否烧了 CP 固件", err);
        return err;
    }

    /* 这里不能用 ESP_ERROR_CHECK：远端异常时 panic 重启会形成重启死循环，
     * 网页就彻底打不开了。任何一步失败只返回错误码。 */
    if ((err = esp_wifi_remote_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        ESP_LOGE(TAG, "remote set_mode failed: 0x%x", err);
        return err;
    }
    if ((err = esp_wifi_remote_start()) != ESP_OK) {
        ESP_LOGE(TAG, "remote start failed: 0x%x", err);
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "remote wifi ready: STA started, scan available");
    return ESP_OK;
}

/* 第二步：保存参数并连接。用户保存配置后调用。 */
esp_err_t remote_sta_apply(const app_cfg_t *cfg)
{
    if (!cfg || cfg->sta_ssid[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        ESP_LOGE(TAG, "apply called before remote_sta_init");
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = *cfg;
    s_cfg.sta_ssid[sizeof(s_cfg.sta_ssid) - 1] = 0;
    s_cfg.sta_pass[sizeof(s_cfg.sta_pass) - 1] = 0;
    s_configured  = true;
    s_attempts    = 0;
    s_backoff_ms  = BACKOFF_MIN_MS;
    s_retry_at    = 0;
    s_connected_at = 0;

    do_connect(false);
    ESP_LOGI(TAG, "applied '%s' (auth=%s band=%d)",
             s_cfg.sta_ssid, s_cfg.eap_enable ? "EAP" : "PSK", s_cfg.band_mode);
    return ESP_OK;
}

void remote_sta_forget(void)
{
    s_configured = false;
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_linked = false;
    s_rssi = 0;
    s_chan = 0;
    ip_reset();
    s_attempts = 0;
    s_backoff_ms = BACKOFF_MIN_MS;
    s_retry_at = 0;
    if (s_ready) {
        esp_wifi_remote_disconnect();
    }
}

/* ---------------- 只读接口 ---------------- */

bool remote_sta_linked(void)      { return s_ready && s_linked; }
bool remote_sta_has_ip(void)      { return s_ready && s_has_ip; }
bool remote_sta_ready(void)       { return s_ready; }
bool remote_sta_configured(void)  { return s_configured; }
int8_t remote_sta_rssi(void)      { return s_ready ? s_rssi : 0; }
uint8_t remote_sta_channel(void)  { return s_ready ? s_chan : 0; }
uint16_t remote_sta_last_reason(void) { return s_reason; }

void remote_sta_ip_str(char *buf, int n)
{
    if (buf && n > 0) {
        strncpy(buf, s_ip, n - 1);
        buf[n - 1] = 0;
    }
}

int remote_sta_rssi_hist(int8_t *out, int max)
{
    if (!out || max <= 0) {
        return 0;
    }
    int n = s_hist_cnt;
    if (n > max) {
        n = max;
    }
    int start = (s_hist_head - n + STA_HIST_LEN) % STA_HIST_LEN;
    for (int i = 0; i < n; i++) {
        out[i] = s_hist[(start + i) % STA_HIST_LEN];
    }
    return n;
}

/* ---------------- 扫描 ---------------- */

static int scan_copy(scan_item_t *out, int max)
{
    int n = s_cache_n < max ? s_cache_n : max;
    memcpy(out, s_cache, (size_t)n * sizeof(scan_item_t));
    return n;
}

static int scan_run(scan_item_t *out, int max)
{
    wifi_scan_config_t sc = {0};
    sc.show_hidden = false;
    sc.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 30;    /* 每信道最少等 30ms */
    sc.scan_time.active.max = 50;    /* 最多 50ms —— 默认是 120~1500ms，慢在这里 */
    sc.home_chan_dwell_time = 20;

    /* 按用户选的频段限定信道：只扫一半信道，直接快一倍 */
    if (s_cfg.band_mode == 1) {
        sc.channel_bitmap.ghz_2_channels = BM_2G_ALL;
        sc.channel_bitmap.ghz_5_channels = BM_BYPASS;
    } else if (s_cfg.band_mode == 2) {
        sc.channel_bitmap.ghz_2_channels = BM_BYPASS;
        sc.channel_bitmap.ghz_5_channels = BM_5G_FAST;
    } else {
        sc.channel_bitmap.ghz_2_channels = BM_2G_ALL;
        sc.channel_bitmap.ghz_5_channels = BM_5G_FAST;   /* 先扫常用非 DFS 信道 */
    }

    uint32_t t0 = now_ms();
    esp_err_t err = ESP_FAIL;
    /* 常见失败原因：刚好有一次 connect 在飞（ESP_ERR_WIFI_STATE）。
     * 等它落地再扫，别直接把“扫描失败”丢给用户。 */
    for (int i = 0; i < 4; i++) {
        err = esp_wifi_remote_scan_start(&sc, true);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "scan_start failed (0x%x), retry %d/4", err, i + 1);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start give up: 0x%x", err);
        return -1;
    }

    uint16_t num = 0;
    esp_wifi_remote_scan_get_ap_num(&num);

    /* 常用信道一个 AP 都没扫到 -> 可能目标就在 DFS 上（52~64 / 100~144），
     * 回退含 DFS 的全位图再扫一次。宁可慢这一次，也别让用户以为"扫不到"。 */
    if (num == 0 && sc.channel_bitmap.ghz_5_channels == BM_5G_FAST) {
        ESP_LOGW(TAG, "non-DFS 扫描 0 个 AP，回退全信道再扫一次");
        sc.channel_bitmap.ghz_5_channels = BM_5G_ALL;
        if (esp_wifi_remote_scan_start(&sc, true) == ESP_OK) {
            esp_wifi_remote_scan_get_ap_num(&num);
        }
    }

    if (num == 0) {
        ESP_LOGW(TAG, "scan done in %ums, 0 ap", now_ms() - t0);
        s_cache_n = 0;
        s_cache_at = now_ms();
        s_cache_ok_at = s_cache_at;
        return 0;
    }
    if (num > 32) {
        num = 32;
    }

    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (!recs) {
        return -1;
    }
    if (esp_wifi_remote_scan_get_ap_records(&num, recs) != ESP_OK) {
        free(recs);
        return -1;
    }

    int n = 0;
    for (int i = 0; i < num && n < (int)(sizeof(s_cache) / sizeof(s_cache[0])); i++) {
        if (recs[i].ssid[0] == 0) {
            continue;
        }
        strncpy(s_cache[n].ssid, (const char *)recs[i].ssid, sizeof(s_cache[n].ssid) - 1);
        s_cache[n].ssid[sizeof(s_cache[n].ssid) - 1] = 0;
        s_cache[n].rssi     = recs[i].rssi;
        s_cache[n].channel  = recs[i].primary;
        s_cache[n].authmode = recs[i].authmode;
        n++;
    }
    free(recs);

    s_cache_n  = n;
    s_cache_at = now_ms();
    s_cache_ok_at = s_cache_at;
    ESP_LOGI(TAG, "scan done in %ums, %d ap(s)", s_cache_at - t0, n);
    return scan_copy(out, max);
}

int remote_sta_scan(scan_item_t *out, int max, bool force)
{
    if (!out || max <= 0) {
        return 0;
    }
    if (!s_ready) {
        ESP_LOGW(TAG, "remote not ready, skip scan");
        return -1;
    }

    /* 8 秒内重复点“扫描”直接用缓存，秒回 */
    if (!force && s_cache_ok_at && now_ms() - s_cache_ok_at < SCAN_CACHE_MS) {
        return scan_copy(out, max);
    }

    s_scanning = true;
    int n = scan_run(out, max);
    s_scanning = false;
    return n;
}
