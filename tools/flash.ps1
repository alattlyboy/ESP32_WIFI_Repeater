# 刷机脚本
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Port 5 -Target all
#   powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Port 3 -Target s3 -Erase
#
# -Port 只写数字，脚本自己拼成 COM5；也可以直接 -PortName COM7

param(
    [int]$Port = 0,
    [string]$PortName = "",
    [ValidateSet("all", "s3", "c5")]
    [string]$Target = "all",
    [string]$Baud = "460800",
    [switch]$Erase
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$esptool = "C:\Espressif\python_env\idf6.1_py3.13_env\Scripts\esptool.exe"

if (-not (Test-Path $esptool)) { throw "找不到 esptool：$esptool" }
if ($PortName -eq "") {
    if ($Port -eq 0) { throw "请指定串口，例如 -Port 5 或 -PortName COM7" }
    $PortName = "COM$Port"
}

function Flash-One {
    param([string]$Chip, [string]$Bin, [string]$Name)
    if (-not (Test-Path $Bin)) { throw "找不到 $Bin，请先运行 tools\build.ps1" }

    if ($Erase) {
        Write-Host "== 擦除 $Name ($Chip) =="
        & $esptool --chip $Chip -p $PortName -b $Baud erase-flash
        if ($LASTEXITCODE -ne 0) { throw "erase-flash 失败，检查串口是否被占用 / 是否按住 BOOT" }
    }

    Write-Host "== 写入 $Name -> $PortName =="
    & $esptool --chip $Chip -p $PortName -b $Baud --before default-reset --after hard_reset `
        write-flash 0x0 $Bin
    if ($LASTEXITCODE -ne 0) { throw "write-flash 失败（$Name）" }
    Write-Host "  $Name 刷写完成"
}

$s3 = Join-Path $Root "dist\s3_host_merged.bin"
$c5 = Join-Path $Root "dist\c5_slave_merged.bin"

if ($Target -in @("all", "s3")) { Flash-One "esp32s3" $s3 "ESP32-S3 主机" }
if ($Target -in @("all", "c5")) {
    if ($Target -eq "all") {
        Write-Host ""
        Write-Host ">>> 现在把 USB 线换到 ESP32-C5 那块板，回车继续 <<<"
        Read-Host
    }
    Flash-One "esp32c5" $c5 "ESP32-C5 从机"
}

Write-Host "全部完成。"
