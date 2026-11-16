# 修复 esp-hosted 3.1.0 在「2.4G-only 主机 + 5G 协处理器」下的编译错误
#
# 问题：port/os/include/eh_host_port_wifi.h 判断 wifi_5g_channel_mask 字段是否存在时，
#       只看了从机能力(CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G)，没看主机自己的结构体有没有这个成员。
#       ESP32-S3 的 wifi_country_t 没有该字段 -> 编译报 has no member named 'wifi_5g_channel_mask'。
#
# 用法： powershell -ExecutionPolicy Bypass -File patches\apply.ps1

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$file = Join-Path $Root "s3_host\managed_components\espressif__esp_hosted\port\os\include\eh_host_port_wifi.h"

if (-not (Test-Path $file)) {
    Write-Host "跳过：找不到 $file（还没拉取组件，先跑一次 build）"
    exit 0
}

$text = Get-Content $file -Raw

$old = @'
#ifdef CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G
  #define EH_HOST_WIFI_GOT_5G_CHANNEL_MASK      1
#else
  #define EH_HOST_WIFI_GOT_5G_CHANNEL_MASK      0
#endif
'@

$new = @'
/* PATCH(local): 该字段是「主机自己的」wifi_country_t 成员。主机若是 2.4G-only 的
 * ESP32/S2/S3，结构体里没有这个成员，仅凭从机(C5)能力去赋值会编译失败。
 * 因此要求「从机有 5G」且「主机是 5G 芯片(C5)」两个条件同时成立。 */
#if defined(CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G) && defined(CONFIG_IDF_TARGET_ESP32C5)
  #define EH_HOST_WIFI_GOT_5G_CHANNEL_MASK      1
#else
  #define EH_HOST_WIFI_GOT_5G_CHANNEL_MASK      0
#endif
'@

$oldNorm = $old -replace "`r`n", "`n"
$textNorm = $text -replace "`r`n", "`n"

if ($textNorm.Contains("CONFIG_IDF_TARGET_ESP32C5) && defined") -or $textNorm.Contains("PATCH(local)")) {
    Write-Host "已经打过补丁，无需处理。"
    exit 0
}

if (-not $textNorm.Contains($oldNorm)) {
    Write-Host "警告：文件内容和预期不一致，可能组件版本变了。请手动检查："
    Write-Host "  $file"
    exit 1
}

($textNorm.Replace($oldNorm, $new)) | Set-Content $file -NoNewline -Encoding utf8
Write-Host "补丁已应用：$file"
