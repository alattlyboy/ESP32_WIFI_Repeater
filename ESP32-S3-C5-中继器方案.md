# ESP32-C5 + ESP32-S3 双芯片中继路由方案

> 目标：C5 收上级 WiFi（5GHz 或 2.4GHz），S3 开 AP 让手机连上就能上网。
> 连接方式：两块板用排针/杜邦线（SPI）连接，靠有线把数据从 C5 传到 S3。

---

## 一、先纠正一个硬件事实（很重要）

你描述的「C5 收 WiFi → S3 无线连 C5 的 WiFi → S3 再发 WiFi」这条**纯无线级联**链路，在两颗芯片上都做不到：

| 约束 | 后果 |
|---|---|
| ESP32-C5 是**单射频**双频芯片，2.4G 和 5G 只能二选一，STA 和 SoftAP 必须**同频段、同信道** | C5 无法「一边收 5G 一边发 2.4G」 |
| ESP32-S3 **只支持 2.4GHz** | S3 根本连不上 5G 的 AP |
| S3 也是单射频，做中继时 STA 和 AP 同信道 | 吞吐直接腰斩（这就是经典 esp32_nat_router 的 15Mbps） |

所以：**针脚连接不是多余的，它就是这条链路唯一的物理通路。**
C5 只干「5G 网卡」的活（WAN），S3 只干「路由 + AP」的活（LAN），中间用 SPI 传以太网帧。

```
        ┌───────── 2.4GHz ────────┐        ┌──────── 5GHz ────────┐
 手机 ──┤ AP(2.4G)          S3    ├─ SPI ─┤ STA(5G)         C5   ├── 上级路由 ── Internet
        └─────────────────────────┘  7根线 └──────────────────────┘
              NAT + DHCP + Web UI                只做网卡，不跑业务
```

收益：上行 5G 避干扰、下行 2.4G 兼容所有 IoT 设备；两颗芯片各管一个频段，不用抢射频时间片。

代价：多一块板、多 7 根线；C5 的 5GHz 只有 20MHz 带宽（WiFi6 20MHz-only non-AP），5G 的价值是**抗干扰和低延迟**，不是翻倍速率。

---

## 二、接线（SPI 全双工，官方 esp-hosted 默认引脚）

两块板都 3.3V 电平，**不需要电平转换**。SPI 是「同名相接」，不要交叉。

| 信号 | 方向 | ESP32-S3（主机 Host） | ESP32-C5（从机 Slave） | 说明 |
|---|---|---|---|---|
| CLK | S3 → C5 | GPIO12 | GPIO3 | 时钟，先跑 5MHz，通了再往上加 |
| MOSI | S3 → C5 | GPIO11 | GPIO7 | 主出从入 |
| MISO | C5 → S3 | GPIO13 | GPIO2 | 主入从出 |
| CS | S3 → C5 | GPIO10 | GPIO10 | 片选 |
| Handshake | C5 → S3 | GPIO17 | GPIO1 | 「我准备好收数据了」 |
| Data Ready | C5 → S3 | GPIO4 | GPIO4 | 「我有数据要发」 |
| Reset | S3 → C5 | GPIO42 | EN / RST | S3 启动时复位 C5 |
| GND | — | GND | GND | **至少接 2~3 根**，信号线和地线交替走 |

> 上表是 esp-hosted Kconfig 的默认值（C5 作为协处理器时 CLK 默认 3、Handshake 默认 1）。不同开发板丝印可能有出入，**接线前对照你板子的原理图**确认这些脚没被 flash/PSRAM/USB 占用；`idf.py menuconfig` 里可以任意改，两边改一致就行。

### 接线注意
1. **线长 ≤10cm**，越短越好；时钟线和数据线分开走，中间夹一根地线。
2. **不要用一块板的 3V3 给另一块供电**（WiFi 发射峰值 300~500mA，LDO 顶不住会随机重启）。两块板各自 USB 供电，只共地。
3. 先接 GND，再接信号线；上电前用万用表确认 3V3 和 GND 没短路。
4. C5 的 GPIO2/3/4 里有 strapping 引脚，上电瞬间电平会影响启动模式。如果 C5 上电后不进 app、串口一直刷下载模式日志，优先怀疑这几根线，把引脚改到别的空闲 GPIO。
5. 调试阶段串口要留着，别占用 UART0（GPIO43/44 on S3）。

---

## 三、软件方案（官方组件，不用自己写协议）

用乐鑫的 **ESP-Hosted-MCU** + **esp_wifi_remote**：
- C5 烧 esp-hosted 的 **slave 固件**，变成一个「SPI 网卡」；
- S3 烧 **host 固件**，通过 `esp_wifi_remote` 调用 C5 的射频（看起来像本地 WiFi），同时用**本地 WiFi 驱动开 SoftAP**；
- S3 上开 lwIP 的 IP 转发 + NAPT，就完成了路由。

关键点：`esp_wifi_remote` 官方支持「本地 WiFi 和远端 WiFi 在同一颗芯片上共存」（官方示例 `two_stations`），所以 S3 可以一边用本地射频发 AP、一边用 C5 的射频做 STA。**但本地和远端代码必须放在不同的 .c 文件里**，否则 `wifi_config_t` 类型会冲突。

### 版本要求
- ESP-IDF **v5.5 起**才支持 ESP32-C5（当前稳定版 v6.0.x）。
- esp-hosted 对 C5 从机是 **BETA**（`--preview`），如果卡住，先用 ESP32-C6 当从机把链路跑通，再换 C5。

### 步骤

**1) C5 从机固件**

```bash
git clone https://github.com/espressif/esp-hosted-mcu.git
cd esp-hosted-mcu/slave
idf.py --preview set-target esp32c5
idf.py menuconfig
#   Example Configuration → Bus Config → Transport layer = SPI Full Duplex
#   引脚按第二节的表填：CLK 3 / MOSI 7 / MISO 2 / CS 10 / Handshake 1 / DataReady 4
idf.py build flash monitor
```

频段选择在从机侧固定（C5 单射频，别指望它自动切）：

```c
// slave 的 app_main 里，WiFi 初始化之后加：
esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);   // 或 5G_ONLY / 2G_ONLY
```

**2) S3 主机工程**

```bash
idf.py set-target esp32s3
idf.py add-dependency "espressif/esp_hosted^2"
idf.py add-dependency "espressif/esp_wifi_remote^1"
idf.py menuconfig
#   Component config → ESP-Hosted → Transport = SPI Full Duplex
#   引脚：CLK 12 / MOSI 11 / MISO 13 / CS 10 / Handshake 17 / DataReady 4 / Reset 42
```

**3) 打开 NAT 所需的 lwIP 选项**

```
CONFIG_LWIP_IP_FORWARD=y
CONFIG_LWIP_IPV4_NAPT=y
CONFIG_LWIP_L2_TO_L3_COPY=y
```

**4) 代码骨架（三个文件，千万别合并）**

`remote_sta.c` —— 远端（C5）WiFi，只放这一个文件里：

```c
#include "injected/esp_wifi.h"          // 必须在 esp_wifi_remote.h 之前
#include "esp_wifi_remote.h"
#include <string.h>

void remote_sta_connect(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_remote_init(NULL);
    esp_wifi_remote_set_mode(WIFI_MODE_STA);
    esp_wifi_remote_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_remote_start();
    esp_wifi_remote_connect();
}
```

`local_ap.c` —— 本地（S3）SoftAP：

```c
#include "esp_wifi.h"
#include <string.h>

void local_ap_start(void)
{
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&ic);

    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, "ESP32-NAT");
    strcpy((char *)ap.ap.password, "12345678");
    ap.ap.ssid_len      = strlen("ESP32-NAT");
    ap.ap.channel       = 6;                 // 2.4G 选 1/6/11
    ap.ap.max_connection = 8;
    ap.ap.authmode      = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
}
```

`main.c` —— 串起来 + 开 NAT：

```c
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif_napt.h"

extern void remote_sta_connect(const char *, const char *);
extern void local_ap_start(void);

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap) {
        esp_netif_napt_enable(ap);       // 在 AP 侧开 NAT
    }
}

void app_main(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();   // S3 本地 AP
    esp_netif_create_default_wifi_sta();  // 远端 STA（C5）

    local_ap_start();
    remote_sta_connect("上级WiFi名", "上级密码");

    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
}
```

**5) 验证顺序**

1. 只看 C5 串口：能不能连上 5G 上级路由、拿到 IP。
2. 看 S3 串口：hosted 初始化是否成功（`esp_hosted_init` 返回 0，能看到 C5 的 MAC）。
3. 手机连 `ESP32-NAT`，看是否分到 `192.168.4.x`。
4. `ping 192.168.4.1` 通 → `ping 8.8.8.8` 通 → `ping www.baidu.com` 通（DNS）。

---

## 四、更省事的替代方案（建议先花 10 分钟验证）

如果**你的手机/IoT 设备都支持 5GHz**，那根本不需要 S3：

> 只买一块 ESP32-C5，刷官方 esp32_nat_router 的 C5 固件（已内置双频支持），
> 在 Web 界面把频段设成 5GHz，STA 和 AP 都跑 5G，一块板就是一个 5G 中继。

- 预编译固件：<https://github.com/martin-ger/esp32_nat_router> 的 `firmware_esp32c5/`
- 网页一键刷机：<https://martin-ger.github.io/esp32_nat_router/>
- 首次开机连 `ESP32_NAT_Router`（开放网络），浏览器开 `192.168.4.1` 配置。

**什么时候才需要双芯片：** 你必须「5G 上行 + 2.4G 下行」的时候（比如一堆只支持 2.4G 的智能家居设备）。这也是你现在这个需求。

---

## 五、常见故障对照

| 现象 | 原因 | 处理 |
|---|---|---|
| S3 启动卡在 hosted init | EN 复位线没接 / Handshake 和 DataReady 接反 | 核对第 2 节表格，SPI 时钟先降到 5MHz |
| 偶发丢包、iperf 跑不动 | 杜邦线太长、共地不足 | 线剪到 5cm，多接地线，时钟降到 20MHz 再试 |
| C5 上电进下载模式 | strapping 引脚被拉 | 换 GPIO |
| 手机连上但上不了网 | 没开 NAPT / IP_FORWARD | 检查三个 lwIP 选项 + `esp_netif_napt_enable` |
| 能通 IP 不能通域名 | DNS 没下发 | AP 的 DHCP 默认带 DNS，确认 STA 侧拿到了上级 DNS |
| 吞吐只有 3~5Mbps | C5 从机 BETA 阶段、或 web 日志太多 | 关日志、开 PSRAM、SPI 提到 40MHz |

**性能预期：** SPI 全双工理论上限约 24Mbps（UDP），端到端实际 **8~15Mbps**。够刷视频、挂 IoT，别指望跑满千兆。

---

## 六、关于 frontend1.0

这个目录现在是空的。如果你的计划是把路由器的 Web 配置界面放在这里，那它最终会编译进 **S3 的固件**（`esp_http_server` + 静态文件打包进 flash 或 LittleFS），不是单独跑的服务。

需要的话我可以直接搭一套：SPI 主机工程骨架 + Web UI（状态页/上行配置/客户端列表/端口转发）+ 构建脚本。
