<<<<<<< HEAD
# ESP32_WIFI_Repeater
一个关于ESP32_S3和ESP32_C5 的WIFI_Repeater  
=======
# ESP32-S3 + ESP32-C5 双芯片 WiFi 中继器

手机连 S3 的 2.4G 热点上网；C5 负责连上级路由的 5G 或 2.4G（含**校园网：802.1X / 开放网 Portal / 普通密码**）。两者用 7 根杜邦线（SPI）连起来，中间传的是以太网帧。

```
手机 ──2.4G──> [ESP32-S3]  AP + DHCP + NAPT + Web UI + 网关守护
                    │  SPI 7 线（40MHz，实测稳定）
                    ▼
               [ESP32-C5]  STA 连上级 WiFi（5G / 2.4G / WPA2-Enterprise）
                    │
                    ▼
                 上级路由器 / 校园网
```

**为什么不能两块板各自无线级联**：C5 是**单射频**，STA 和 SoftAP 必须同频段；S3 又**只支持 2.4G**，连不上 5G 的 AP。所以 5G 上行只能靠有线（SPI）把流量交给 S3。

**反过来这也带来一个好处**：热点在 S3、上行在 C5，两者信道彻底解耦，2.4G 热点可以钉死在最干净的信道，不会因为上级路由换信道而把客户端全体踢下线。这是单芯片方案做不到的。

---

## 一、交付物索引

| 文档 | 给谁看 | 内容 |
|---|---|---|
| **[docs/使用说明书.html](docs/使用说明书.html)** | 使用者 | 接线、刷机、配网（三种认证方式）、常见问题，浏览器打开带排版 |
| **[docs/使用报告.md](docs/使用报告.md)** | 决策者 | 实测成绩、能做什么 / 不能做什么、运维建议、硬件成本 |
| **[docs/测试报告.md](docs/测试报告.md)** | 验收方 | 20 条用例与结果、5 个已修缺陷、4 项已知限制、复现命令 |
| **[docs/操作手册.md](docs/操作手册.md)** | 维护 / 开发者 | 架构、编译、配置项、HTTP API、日志表、性能调优、802.1X 与 Portal 原理、VPN |
| **[docs/交付清单.md](docs/交付清单.md)** | 要烧录 / 找代码的人 | **bin 的路径、MD5、烧录地址与命令；每个源文件的职责与位置** |
| **[docs/接线图.svg](docs/接线图.svg)** | 接线时 | 引脚级：S3 ⇄ C5 的 7 根线 |
| **[docs/连接路线图.svg](docs/连接路线图.svg)** | 理解数据流 | 一帧数据从手机到互联网的完整路径与地址分配 |

**固件产物**：`dist/s3_host_merged.bin`（烧 0x0）、`dist/c5_slave_merged.bin`（烧 0x0）。
校验和、增量烧录地址、源码目录见 [docs/交付清单.md](docs/交付清单.md)。

## 二、快速开始

```powershell
# 1. 刷机（两块板都刷到 0x0；合并镜像里已经算好各自的 bootloader 偏移）
esptool --chip esp32s3 -p COM15 -b 460800 write-flash 0x0 dist\s3_host_merged.bin
esptool --chip esp32c5  -p COM12 -b 460800 write-flash 0x0 dist\c5_slave_merged.bin
#   刷 C5 时用 tools\flash_c5.py 更省事：它会先按住 S3，避免 GPIO42 把 C5 打断

# 2. 按接线图连 7 根线 + 2 根地（各自 USB 供电，只共地）

# 3. 手机连 ESP32-NAT / 12345678 → 打开 http://192.168.4.1
```

## 三、2.2 相比 2.1 改了什么

| 项 | 2.1 | 2.2 |
|---|---|---|
| **开放网络（Portal）** | 只能选"密码"或"企业版"，连开放校园网必报 reason=210（安全不兼容） | 新增**开放网络**模式（`eap_enable=2`），`threshold.authmode=OPEN`、密码清空。校园网实测连上即用 |
| **能被 Portal 跳转吗** | 不知道 | 新增**设备侧自检**：自己解析域名 + 发 HTTP 探测，日志直接给 `204` / `302→门户地址` / `TCP 不通` 三种结论 |
| **DNS 下发** | 客户端拿网关当 DNS，域名全废 | 在 AP 初始化时预设 `OFFER_DNS`，运行时只改地址不 stop/start dhcps；另起 **53 端口中继**兜住缓存旧配置的终端 |
| **聚合吞吐** | 1.0Mbps（SPI 10MHz + TCP 窗口 5760） | **3.3Mbps**（SPI 40MHz + 窗口 65535 + 收包队列 32/64） |
| **扫描耗时** | 10.5 秒（5G 位图把 DFS 信道也算进来，被动驻留 110ms+） | **2.7 秒**（只扫非 DFS 的 36~48 / 149~165，扫不到再回退全信道） |

## 三之二、2.1 相比 2.0 改了什么

| 项 | 2.0 | 2.1 |
|---|---|---|
| **"连上热点却没网"** | 靠定时 `get_ap_info` 猜链路，一次 RPC 抖动就当掉线，把好连接拆了重建；NAPT/DNS 只挂在一次性事件上，漏了就永远不开 | **事件驱动链路状态** + 2 秒一次的**幂等网关守护**（默认出口/NAPT/DNS 每轮复查，失败重试），漏事件能自愈 |
| 重连 | 固定 10 秒判死，立刻重连，日志刷屏 | **指数退避** 1s→2s→4s→…→30s；`associated but no ip 15s` 才重连；主动断开不再触发自愈循环 |
| **校园网 / 企业版** | 只有 PSK，连校园网报 reason=210 / 202 | **PEAP-MSCHAPv2 / TTLS / TLS**，网页可配（用户名、密码、匿名身份、跳过证书校验、TTLS 内层） |
| 断开原因 | 只有原始数字 | 网页把 reason 翻成人话（15=密码错、210=需要 802.1X、201=找不到…） |
| **扫描速度** | 被动扫全频段，4~6 秒 | **主动扫描 + 每信道 40~80ms + 按频段限定信道 + 8 秒缓存**，1.2~2.5 秒 |
| DNS | 只透传上级，上级不给就解析不了域名 | 上级 DNS 优先，**没有就用 223.5.5.5 兜底**，也可在网页强制指定 |
| C5 固件 | 未开企业版 | 打开 `ESP_HOSTED_CP_FEAT_WIFI_EXT_ENT` + 主机侧 `HOST_FEAT_WIFI_EXT_ENT` |

## 四、关键实现点

- C5 烧的是 **esp-hosted 的协处理器（CP）固件**，本身没有应用逻辑；SSID/密码/频段/802.1X 凭据由 S3 通过 RPC 下发。
- S3 同时跑两套 WiFi 驱动：本地 `esp_wifi_*` 开 AP，`esp_wifi_remote_*` 驱动 C5 的射频。因为 S3 自带 WiFi，组件走 `net2` 路径，不需要 injected 头文件。
- NAT 用 lwIP 自带 NAPT：`CONFIG_LWIP_IPV4_NAPT` + `esp_netif_napt_enable(AP网卡)`，**只有一层 NAT**。
- **判定"能上网"只看 `has_ip`**：`sta_linked` 只代表空口关联成功，拿不到 IP 一样上不了网。
- 企业版的 EAP-TLS 握手跑在 C5 的 supplicant 里，**不占 SPI 带宽**，不会拖慢转发。
- **HTTP handler 里绝不调远端 RPC**。httpd 单任务串行，一次阻塞就是整站白屏。所有远端状态由 `sta_poll` 后台任务缓存。

## 五、性能与限制

- SPI 跑 **40MHz**（`CONFIG_ESP_HOSTED_HOST_SPI_CLK_MHZ=40`），要求线长 ≤10cm、GND ≥2 根。实测聚合 **3.3Mbps**（四流）；单流受校园网按账号限速，直连也只有 1.2Mbps 左右。
- 这个数字 ≠ 家用宽带上限：**瓶颈是上级网络对单账号的限速**，换到不限速的家用路由会明显更快。详见操作手册 §7 的对照实验。
- C5 的 5GHz 在 802.11ax 下只有 **20MHz 带宽**。5G 的价值是抗干扰、低延迟，不是提速。
- 建议客户端 ≤ 8 台（RAM 限制，工程里放到 10）。
- 扫描时上行会断 1~2 秒，C5 单射频物理限制。

## 六、VPN 怎么算

中继器 + 企业版 + Portal 穿透都已经有了。**WireGuard 客户端本版没有内置**：S3 上确实能跑（代价 2~6Mbps），但必须有你自己的 WG 服务端才能验收，社区组件（`trombik/esp_wireguard`）在 IDF 6.1 上还要改兼容代码。与其塞一个半成品进去让你在"到底哪一步坏了"上浪费时间，不如把链路写清楚 —— 详见 [操作手册 §12](docs/操作手册.md)。

现在就能用的两条路：VPN 配在客户端上（本机只当中继），或者把 VPN 跑在上级路由（OpenWrt + WireGuard），本机中继那个已经翻好的热点。

## 七、编译

本机已装 ESP-IDF v6.1（`C:\esp\v6.1\esp-idf`，工具在 `C:\Espressif`）。

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1            # 两个都编
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target s3 -Jobs 1
```

产物在 `dist\`。合并镜像的偏移两片不同，脚本已处理：

| 芯片 | bootloader | 分区表 | app | 烧录地址 |
|---|---|---|---|---|
| ESP32-S3 | 0x0 | 0x8000 | 0x10000 | **0x0** |
| ESP32-C5 | **0x2000** | 0x8000 | 0x10000 | **0x0**（镜像开头补 0x2000 字节 0xFF） |

> 只改应用代码时可以只烧 app，保留 NVS 里的配置：
> `esptool --chip esp32s3 -p COM15 write-flash 0x10000 s3_host\build\esp32_s3_nat_repeater.bin`

## 八、已知问题

**esp-hosted 3.1.0 编译 bug**：`eh_host_port_wifi.h` 判断 `wifi_5g_channel_mask` 是否存在时只看从机、不看主机。S3 是 2.4G-only，结构体没这个成员 → 编译失败。已打补丁（`patches/`），组件被重新下载覆盖后重跑：

```powershell
powershell -ExecutionPolicy Bypass -File patches\apply.ps1
```

其它现象见 [docs/操作手册.md](docs/操作手册.md) §9 排障决策树。
>>>>>>> 1781ead (ESP32_S3和ESP32_C5中继器)
