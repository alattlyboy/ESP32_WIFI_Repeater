# 编译两个固件，并把 bin 汇总到 dist\
# 用法： powershell -ExecutionPolicy Bypass -File tools\build.ps1
#        powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target s3

param(
    [ValidateSet("all", "s3", "c5")]
    [string]$Target = "all",
    [int]$Jobs = 2
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

# ---- ESP-IDF 环境（按本机安装路径写死，换机器改这几行即可）----
$IdfPath   = "C:\esp\v6.1\esp-idf"
$ToolsPath = "C:\Espressif"
$PyEnv     = "C:\Espressif\python_env\idf6.1_py3.13_env"

$env:IDF_PATH              = $IdfPath
$env:IDF_TOOLS_PATH        = $ToolsPath
$env:IDF_PYTHON_ENV_PATH   = $PyEnv
$env:ESP_IDF_VERSION       = "6.1"
$env:IDF_COMPONENT_MANAGER = "1"
$env:PYTHONUTF8            = "1"

$extra = @(
    "C:\Espressif\tools\cmake\4.0.3\bin",
    "C:\Espressif\tools\ninja\1.12.1",
    "C:\Espressif\tools\ccache\4.12.1",
    "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin",
    "C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin",
    "$PyEnv\Scripts"
) -join ";"
$env:PATH = "$extra;$env:PATH"

$py      = "$PyEnv\Scripts\python.exe"
$idfpy   = "$IdfPath\tools\idf.py"
$runner  = "$PSScriptRoot\run_idf.py"
$esptool = "$PyEnv\Scripts\esptool.exe"

# 只跑 configure，再用 ninja 限并行编译。
# 原因：这台机器上高并行编译偶尔会把 gcc 卡死，-j 2 稳定。
function Build-Project {
    param([string]$WorkDir, [string]$ChipTarget)

    Push-Location $WorkDir
    try {
        if (-not (Test-Path "sdkconfig")) {
            Write-Host "  首次配置：set-target $ChipTarget"
            & $py $runner $idfpy set-target $ChipTarget
            if ($LASTEXITCODE -ne 0) { throw "set-target 失败" }
        } else {
            & $py $runner $idfpy reconfigure
            if ($LASTEXITCODE -ne 0) { throw "reconfigure 失败" }
        }

        Push-Location "build"
        & ninja -j $Jobs
        $rc = $LASTEXITCODE
        Pop-Location
        if ($rc -ne 0) { throw "编译失败（$WorkDir）" }
    } finally {
        Pop-Location
    }
}

function New-Merged {
    param([string]$WorkDir, [string]$Chip, [string]$OutName, [string]$FlashSize, [string]$AppBin,
          [string]$BootOffset = "0x0")
    # ESP32-S3 的 bootloader 在 0x0；ESP32-C5（RISC-V）在 0x2000。
    # 用 0x2000 时 merge-bin 会在文件开头填 0x2000 字节 0xFF，
    # 所以合并镜像仍然从 0x0 开始写，bootloader 正好落在 0x2000。
    $b   = Join-Path $WorkDir "build"
    $out = Join-Path $Root "dist\$OutName"
    & $esptool --chip $Chip merge-bin -o $out --flash-mode dio --flash-freq 80m --flash-size $FlashSize `
        $BootOffset (Join-Path $b "bootloader\bootloader.bin") `
        "0x8000"  (Join-Path $b "partition_table\partition-table.bin") `
        "0x10000" (Join-Path $b $AppBin)
    if ($LASTEXITCODE -ne 0) { throw "merge-bin 失败: $OutName" }
    Write-Host "  -> dist\$OutName"
}

New-Item -ItemType Directory -Force -Path (Join-Path $Root "dist") | Out-Null

if ($Target -in @("all", "s3")) {
    Write-Host "== 编译 ESP32-S3 主机固件 =="
    Build-Project (Join-Path $Root "s3_host") "esp32s3"
    New-Merged (Join-Path $Root "s3_host") "esp32s3" "s3_host_merged.bin" "16MB" "esp32_s3_nat_repeater.bin"
}

if ($Target -in @("all", "c5")) {
    Write-Host "== 编译 ESP32-C5 从机固件 =="
    Build-Project (Join-Path $Root "c5_slave") "esp32c5"
    New-Merged (Join-Path $Root "c5_slave") "esp32c5" "c5_slave_merged.bin" "4MB" "esp32_c5_coprocessor.bin" "0x2000"
}

Write-Host "完成。产物在 dist\ 目录。"
