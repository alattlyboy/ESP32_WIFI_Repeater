/*
 * ESP32-S3 + ESP32-C5 双芯片中继路由器（固件 2.1）
 *
 *   手机 --2.4G--> [S3: SoftAP + NAT + Web UI] --SPI--> [C5: STA 5G/2.4G] --> 上级路由
 *
 * 本文件负责：初始化、网卡创建、NAPT 开关、DNS 下发、BOOT 键长按恢复出厂。
 * WiFi 具体操作分别在 local_ap.c（本地射频）和 remote_sta.c（C5 射频）。
 *
 * 【2.1 关键改动：为什么“连着热点却上不了网”】
 *   旧版把 NAPT/DNS 只挂在一次性的 IP_EVENT 上，事件漏了就永远不开 NAT；
 *   而且 DNS 只透传上级，上级不给 DNS（企业网/校园网常见）时客户端就解析不了域名。
 *   现在改成“网关守护”：每 2 秒对照 remote_sta_has_ip() 复查一次
 *     - 默认出口是否指向 STA 网卡
 *     - NAPT 是否开着（失败会重试）
 *     - DHCP 下发的 DNS 是否有效（没有就用 223.5.5.5 / 119.29.29.29 兜底）
 *   幂等，随便漏事件都能自愈。
 */
#include <string.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "app_config.h"
#include "local_ap.h"
#include "remote_sta.h"
#include "web_server.h"

static const char *TAG = "MAIN";

/* 长按恢复出厂用的按键：DevKitC 上的 BOOT 键 */
#define BOOT_GPIO       GPIO_NUM_0
#define HOLD_TICKS      50      /* 50 × 100ms = 5 秒 */

/* S3 自带 WiFi 时，esp_wifi_remote 把远端事件放在独立的 WIFI_REMOTE_EVENT 事件基上，
 * 但组件里没有提供它的定义（net2 路径的符号漏洞），这里补一个。 */
#if defined(CONFIG_ESP_WIFI_ENABLED)
ESP_EVENT_DEFINE_BASE(WIFI_REMOTE_EVENT);
#endif

/* esp-hosted 把远端 WiFi 事件投递到真实的 WIFI_EVENT 基上；而 esp_wifi_remote 的
 * net2 默认处理器（负责 netif up / DHCP / rx 回调注册）注册在 WIFI_REMOTE_EVENT 上。
 * 本地 WiFi 也启用时（CONFIG_ESP_WIFI_ENABLED），这两个是不同的 ESP_EVENT 基，
 * 于是那些处理器永远收不到事件 —— 症状：netif 不 up、input 为空、DHCP 拿不到 IP。
 * 这里把远端事件桥接一份过去（本地 AP 的事件传过去也会被 NULL 检查挡掉，无害）。 */
static void remote_evt_bridge(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    size_t sz = 0;
    if (data) {
        if (id == WIFI_EVENT_STA_CONNECTED) {
            sz = sizeof(wifi_event_sta_connected_t);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            sz = sizeof(wifi_event_sta_disconnected_t);
        }
    }
    esp_event_post(WIFI_REMOTE_EVENT, id, data, sz, 0);
}

static esp_netif_t *s_ap;
static esp_netif_t *s_sta;
static app_cfg_t s_cfg;

static bool s_napt_on;
static bool s_default_set;
static uint32_t s_dns_applied;      /* 已下发的 DNS（网络序），0=还没设过 */
static int s_uplink_was_up;

/* esp_wifi_remote_net2.c 提供但未在头文件里声明的公开 API（链接器可解析） */
extern esp_netif_t *esp_netif_create_wifi_remote(wifi_interface_t wifi_if,
                                                 const esp_netif_inherent_config_t *cfg);
extern esp_err_t esp_wifi_set_default_wifi_remote_sta_handlers(void);

/* 把 DNS 下发给 AP 的 DHCP 服务器 + 起一个 53 端口中继。
 *
 * 【根因】dhcpserver.c 里 `dhcps->dhcps_dns` 默认是 0，DHCP Offer 的
 *   "Add DNS option" 分支 (`if (dhcps_dns_enabled(...) && ...)`) 直接不进，
 *   于是 Offer 里**没有 option 6**。客户端只好退化成"网关当 DNS"(192.168.4.1)，
 *   而本机根本不监听 53 —— 症状就是：ping 通、按 IP 能开网页，但域名全解析不了。
 *   只调 esp_netif_set_dns_info() 也没用：地址存进去了，但 Offer 不带这个选项。
 *
 * 所以要三件事一起做：
 *   ① dhcps 停掉（SET 选项要求 STOPPED，否则返回 ALREADY_STARTED）
 *   ② 打开 OFFER_DNS 位
 *   ③ 写 MAIN / BACKUP 两个 DNS，再启 dhcps
 * 另外再起 dns_relay()，让 192.168.4.1 自己也能回答查询（兜住缓存了旧配置的终端）。
 */
static uint32_t parse_ipv4(const char *s);

static volatile uint32_t s_dns_up;      /* 中继上游，网络序 */

static void apply_dhcp_dns(uint32_t dns_addr, uint32_t dns2_addr)
{
    if (dns_addr == 0) {
        return;
    }
    s_dns_up = dns_addr;                /* 中继上游始终跟最新值 */

    if (dns_addr == s_dns_applied) {
        return;
    }

    esp_netif_dns_info_t d = {0};
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4.addr = dns_addr;
    esp_err_t e1 = esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &d);

    if (dns2_addr) {
        esp_netif_dns_info_t d2 = {0};
        d2.ip.type = ESP_IPADDR_TYPE_V4;
        d2.ip.u_addr.ip4.addr = dns2_addr;
        esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_BACKUP, &d2);
    }

    s_dns_applied = dns_addr;
    ESP_LOGI(TAG, "dhcp dns -> " IPSTR " (err=0x%x)", IP2STR(&d.ip.u_addr.ip4), e1);
}

/* DNS 中继：53 端口收查询，原样转给上游，把答案原样回给客户端。
 * 为什么需要：① 手机/Windows 会缓存 "网关=DNS"；② 部分终端无视 DHCP 的 option 6。
 * 每个查询单独开一个上游 socket，逻辑最简单（家用场景每秒几十个包，够用）。 */
static void dns_relay_task(void *arg)
{
    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(53);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns relay: socket fail (errno=%d)", errno);
        vTaskDelete(NULL);
        return;
    }
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGE(TAG, "dns relay: bind :53 fail (errno=%d)", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "dns relay listening on :53");

    uint8_t *buf = malloc(800);
    if (!buf) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, 800, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            continue;
        }

        uint32_t up = s_dns_up ? s_dns_up : parse_ipv4("223.5.5.5");

        int us = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (us < 0) {
            continue;
        }
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(us, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in to = {0};
        to.sin_family      = AF_INET;
        to.sin_port        = htons(53);
        to.sin_addr.s_addr = up;

        if (sendto(us, buf, n, 0, (struct sockaddr *)&to, sizeof(to)) >= 0) {
            int m = recv(us, buf, 800, 0);
            if (m > 0) {
                sendto(sock, buf, m, 0, (struct sockaddr *)&from, fl);
            }
        }
        close(us);
    }
}

/* 解析一个点分十进制字符串到网络序地址，失败返回 0 */
static uint32_t parse_ipv4(const char *s)
{
    if (!s || !s[0]) {
        return 0;
    }
    esp_ip4_addr_t a = {0};
    if (esp_netif_str_to_ip4(s, &a) != ESP_OK) {
        return 0;
    }
    return a.addr;
}

/* 网关守护：幂等，每 2 秒跑一次，漏事件也能自愈 */
static void gateway_tick(void)
{
    if (!remote_sta_has_ip()) {
        if (s_uplink_was_up) {
            s_uplink_was_up = 0;
            ESP_LOGW(TAG, "uplink lost —— 客户端暂时无法上网（原因码 %u）",
                     remote_sta_last_reason());
        }
        return;
    }

    if (!s_uplink_was_up) {
        s_uplink_was_up = 1;
        char ip[16];
        remote_sta_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG, "uplink up: %s", ip);
    }

    /* 1) 默认出口指向 STA —— 不设的话 AP 转发的包会从 AP 自己出去，全丢 */
    if (!s_default_set) {
        esp_netif_set_default_netif(s_sta);
        s_default_set = true;
        ESP_LOGI(TAG, "default netif -> STA");
    }

    /* 2) NAPT：失败就每轮重试，直到开起来 */
    if (!s_napt_on) {
        esp_err_t err = esp_netif_napt_enable(s_ap);
        if (err == ESP_OK) {
            s_napt_on = true;
            ESP_LOGI(TAG, "NAPT enabled on AP (客户端可以上网了)");
        } else {
            ESP_LOGE(TAG, "NAPT enable failed: 0x%x (需要 CONFIG_LWIP_IPV4_NAPT/IP_FORWARD)", err);
            return;
        }
    }

    /* 3) DNS：优先用上级下发的；上级不给就按配置兜底 */
    uint32_t dns2 = parse_ipv4(s_cfg.dns2);
    uint32_t dns  = parse_ipv4(s_cfg.dns1);
    if (s_cfg.dns_mode == 0) {
        esp_netif_dns_info_t info = {0};
        if (esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &info) == ESP_OK &&
            info.ip.u_addr.ip4.addr != 0) {
            dns = info.ip.u_addr.ip4.addr;
        }
    }
    if (dns == 0) {
        dns = parse_ipv4("223.5.5.5");
    }
    apply_dhcp_dns(dns, dns2);
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_STA_LOST_IP)) {
        s_default_set = false;   /* 让下一次 tick 重新确认默认出口 */
        gateway_tick();          /* 事件来了就立刻跑一遍，不等 2 秒 */
    }
}

/* 设备侧上网自检：自己解析域名 + 发一个 HTTP 探测，把结果打在日志里。
 *
 * 为什么要在设备上做：把电脑切到 ESP32 热点才能测，一测就断掉远程连接；
 * 而下面这个探测直接走设备自己的上行，能明确区分三种情况：
 *   204          → 真能上网
 *   302/200 带门户地址 → 被校园网 Portal 拦截，需要网页登录
 *   dns/tcp 失败 → 上行 IP 拿到了但出口不通
 */
static void netprobe_once(void)
{
    const char *host = "connect.rom.miui.com";

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo(host, "80", &hints, &res);
    if (g != 0 || res == NULL) {
        ESP_LOGW(TAG, "[net] DNS 失败: %s (err=%d) —— 校园网多半屏蔽了公共 DNS", host, g);
        return;
    }

    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    char ipbuf[16] = {0};
    inet_ntoa_r(sa->sin_addr, ipbuf, sizeof(ipbuf));
    ESP_LOGW(TAG, "[net] DNS ok: %s -> %s", host, ipbuf);

    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) {
        freeaddrinfo(res);
        return;
    }
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(80);
    dst.sin_addr   = sa->sin_addr;
    freeaddrinfo(res);

    if (connect(s, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        ESP_LOGW(TAG, "[net] TCP 80 连不上 (errno=%d) —— 有 IP 但出不去", errno);
        close(s);
        return;
    }

    const char *req =
        "GET /generate_204 HTTP/1.1\r\n"
        "Host: connect.rom.miui.com\r\n"
        "User-Agent: esp32-nat/2.2\r\n"
        "Connection: close\r\n\r\n";
    if (send(s, req, strlen(req), 0) < 0) {
        close(s);
        return;
    }

    char buf[768] = {0};
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    close(s);
    if (n <= 0) {
        ESP_LOGW(TAG, "[net] 没有收到响应");
        return;
    }

    int code = 0;
    if (sscanf(buf, "HTTP/%*d.%*d %d", &code) != 1) {
        ESP_LOGW(TAG, "[net] 响应不合法: %.40s", buf);
        return;
    }

    if (code == 204) {
        ESP_LOGW(TAG, "[net] HTTP 204 —— 上行直接可以上网（无需门户认证）");
        return;
    }

    /* 找 Location: 判断是不是被门户劫持 */
    char *loc = strcasestr(buf, "\r\nLocation:");
    if (loc) {
        loc += 11;
        while (*loc == ' ') loc++;
        char *e = strstr(loc, "\r\n");
        if (e) *e = 0;
        ESP_LOGW(TAG, "[net] HTTP %d 被门户拦截 -> %s", code, loc);
    } else {
        ESP_LOGW(TAG, "[net] HTTP %d（非 204，可能被劫持）", code);
    }
}

static void netprobe_task(void *arg)
{
    /* 等上行拿到 IP 再测，最多等 60 秒 */
    for (int i = 0; i < 60 && !remote_sta_has_ip(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    for (int round = 0; round < 3; round++) {
        if (remote_sta_has_ip()) {
            netprobe_once();
        }
        vTaskDelay(pdMS_TO_TICKS(20000));
    }
    vTaskDelete(NULL);
}

/* 一次性自检：把收帧守卫依赖的条件和扫描能力打出来，便于定位 */
static void diag_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(8000));

    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    ESP_LOGW(TAG, "[diag] ifkey WIFI_STA_DEF -> %p ; s_sta=%p", n, s_sta);
    if (n) {
        ESP_LOGW(TAG, "[diag] netif up=%d", esp_netif_is_netif_up(n));
        struct netif *impl = (struct netif *)esp_netif_get_netif_impl(n);
        ESP_LOGW(TAG, "[diag] impl=%p input=%p", impl, impl ? impl->input : NULL);
    }
    ESP_LOGW(TAG, "[diag] linked=%d has_ip=%d napt=%d reason=%u",
             remote_sta_linked(), remote_sta_has_ip(), s_napt_on, remote_sta_last_reason());

    /* AP 网卡自身的地址：客户端能不能打开网页全看这个 */
    for (int k = 0; k < 6; k++) {
        esp_netif_ip_info_t aip = {0};
        esp_netif_get_ip_info(s_ap, &aip);
        ESP_LOGW(TAG, "[diag] AP up=%d ip=" IPSTR " gw=" IPSTR " mask=" IPSTR " clients=%d",
                 esp_netif_is_netif_up(s_ap),
                 IP2STR(&aip.ip), IP2STR(&aip.gw), IP2STR(&aip.netmask),
                 local_ap_client_count());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    /* 扫一次，验证信道位图没把信道全滤掉（这也是最快的端到端扫描自检） */
    static scan_item_t items[32];
    int cnt = remote_sta_scan(items, 32, true);
    ESP_LOGW(TAG, "[diag] scan -> %d ap(s)", cnt);
    for (int i = 0; i < cnt && i < 5; i++) {
        ESP_LOGW(TAG, "[diag]   %s ch%d %ddBm", items[i].ssid, items[i].channel, items[i].rssi);
    }
    vTaskDelete(NULL);
}

/* 长按 BOOT 5 秒 = 恢复出厂（清 NVS + 重启）。短按无副作用，因为它也是下载模式键。 */
static void boot_button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int held = 0;
    bool warned = false;
    for (;;) {
        if (gpio_get_level(BOOT_GPIO) == 0) {
            held++;
            if (held == 30 && !warned) {
                ESP_LOGW(TAG, "继续按住 2 秒将恢复出厂设置…");
                warned = true;
            }
            if (held >= HOLD_TICKS) {
                ESP_LOGW(TAG, "factory reset by button");
                remote_sta_forget();
                app_cfg_erase();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            held = 0;
            warned = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void watchdog_cb(void *arg)
{
    gateway_tick();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    app_cfg_load(&s_cfg);

    /* 两块网卡：本地 AP + 远端（C5）STA。
     * 注意 if_key 必须是 "WIFI_STA_DEF"：esp-hosted 的收帧守卫按这个名字查找 netif，
     * 用 esp_wifi_remote_create_default_sta() 默认的 "WIFI_STA_RMT" 会查不到，
     * 导致上行帧全部被扣住、DHCP 永远拿不到 IP（症状：sta_linked 但 sta_ip=0.0.0.0）。 */
    esp_netif_inherent_config_t sta_nc = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    sta_nc.if_key  = "WIFI_STA_DEF";
    sta_nc.if_desc = "wifi_sta_remote";
    s_ap  = esp_netif_create_default_wifi_ap();

    /* 必须在 dhcps 启动**之前**设：esp_netif_dhcps_option(SET) 要求 STOPPED。
     * dhcps->dhcps_dns 默认 0 → DHCP Offer 里没有 option 6 → 客户端拿网关当 DNS。
     * 这里设一次，后面运行时只改地址，再也不 stop/start（stop 会把 AP 的
     * 192.168.4.1 一起弄丢，表现为：DHCP 能给地址、公网也能通，但 192.168.4.1 失联）。 */
    {
        uint8_t offer = 0x02;   /* OFFER_DNS */
        esp_err_t e = esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET,
                                            ESP_NETIF_DOMAIN_NAME_SERVER,
                                            &offer, sizeof(offer));
        ESP_LOGI(TAG, "dhcp offer-dns preset: 0x%x", e);
    }
    s_sta = esp_netif_create_wifi_remote(WIFI_IF_STA, &sta_nc);
    esp_wifi_set_default_wifi_remote_sta_handlers();
    if (!s_ap || !s_sta) {
        ESP_LOGE(TAG, "netif create failed");
        return;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               ip_event_handler, NULL));
    /* 必须在下发 STA_START 之前注册，否则 netif 永远不会 up */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               remote_evt_bridge, NULL));

    ESP_ERROR_CHECK(local_ap_start(s_cfg.ap_ssid, s_cfg.ap_pass, s_cfg.ap_channel));
    ESP_ERROR_CHECK(web_server_start(&s_cfg));

    /* 状态轮询任务先起来：即使远端未初始化，HTTP 也能立刻拿到（未连接的）状态，
     * 不会因为 RPC 阻塞而让网页卡在“读取中”。 */
    remote_sta_begin_poll();

    /* 无条件初始化远端 WiFi 栈 —— 首次配网时 NVS 里还没有 SSID，
     * 但扫描恰恰需要一个已经 start 的射频，顺序反了就会死锁。 */
    esp_err_t rerr = remote_sta_init();
    if (rerr != ESP_OK) {
        ESP_LOGE(TAG, "C5 未就绪：网页仍可打开，但扫描和联网不可用（查 SPI 接线 / C5 供电 / C5 固件）");
    }

    if (s_cfg.sta_ssid[0]) {
        if (rerr == ESP_OK) {
            remote_sta_apply(&s_cfg);
        }
    } else {
        ESP_LOGW(TAG, "还没配置上行 WiFi，连上 '%s' 后打开 http://192.168.4.1", s_cfg.ap_ssid);
    }

    const esp_timer_create_args_t targs = { .callback = watchdog_cb, .name = "gw" };
    esp_timer_handle_t t;
    esp_timer_create(&targs, &t);
    esp_timer_start_periodic(t, 2 * 1000 * 1000);

    xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 2, NULL);
    xTaskCreate(diag_task, "diag", 4096, NULL, 2, NULL);
    xTaskCreate(netprobe_task, "netprobe", 4096, NULL, 2, NULL);
    /* DNS 中继要在 AP 起来之后绑 :53，且必须早于第一批客户端连上来 */
    xTaskCreate(dns_relay_task, "dns_relay", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "boot done (fw %s), free heap: %u", WEB_FW_VER,
             (unsigned)esp_get_free_heap_size());
}
