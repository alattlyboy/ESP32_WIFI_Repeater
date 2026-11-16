"""
刷 C5 时必须先按住 S3：S3 的 GPIO42 接 C5 的 EN，S3 一复位就会把 C5 打断，
esptool 表现为端口乱变 / 写入失败。本脚本先拉 S3 的 RTS（EN 低）再刷 C5。

用法:
  python tools\flash_c5.py [--erase] [--bin dist\c5_slave_merged.bin]
  python tools\flash_c5.py --app            # 只刷 app 到 0x10000（保留 C5 的 NVS）
"""
import os
import subprocess
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESPTOOL = r"C:\Espressif\python_env\idf6.1_py3.13_env\Scripts\esptool.exe"
S3_PORT = "COM15"
C5_PORT = "COM12"
BAUD = "460800"


def main():
    args = sys.argv[1:]
    erase = "--erase" in args
    app_only = "--app" in args
    bin_path = os.path.join(ROOT, "dist", "c5_slave_merged.bin")
    for i, a in enumerate(args):
        if a == "--bin" and i + 1 < len(args):
            bin_path = args[i + 1]

    if not os.path.exists(bin_path):
        sys.exit(f"找不到 {bin_path}，先跑 tools\\build.ps1 -Target c5")

    s3 = serial.Serial(S3_PORT, 115200, timeout=0.2)
    s3.setDTR(False)
    s3.setRTS(True)          # EN 拉低 = S3 保持复位，GPIO42 不再乱动
    time.sleep(0.3)
    print(f"[hold] {S3_PORT} 已按住复位，开始刷 {os.path.basename(bin_path)}", flush=True)

    try:
        if erase:
            print("[erase] erase-flash ...", flush=True)
            subprocess.run([ESPTOOL, "--chip", "esp32c5", "-p", C5_PORT, "-b", BAUD,
                            "erase-flash"], check=True)

        if app_only:
            app = os.path.join(ROOT, "c5_slave", "build", "esp32_c5_coprocessor.bin")
            offset = "0x10000"
            target = app
        else:
            offset = "0x0"
            target = bin_path

        print(f"[flash] {target} -> {C5_PORT} @ {offset}", flush=True)
        subprocess.run([ESPTOOL, "--chip", "esp32c5", "-p", C5_PORT, "-b", BAUD,
                        "--before", "default-reset", "--after", "hard-reset",
                        "write-flash", offset, target], check=True)
        print("[done]", flush=True)
    finally:
        time.sleep(0.3)
        s3.setRTS(False)     # 放开 S3，整机重新上电
        s3.close()
        print("[hold] S3 已放开", flush=True)


if __name__ == "__main__":
    main()
