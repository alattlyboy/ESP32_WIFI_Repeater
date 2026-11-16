"""
串口抓日志小工具（不用 idf.py monitor，避免交互阻塞）。

用法：
  python tools\monitor.py COM15 10            # 抓 10 秒
  python tools\monitor.py COM15 10 --reset    # 先硬复位再抓
  python tools\monitor.py COM15 6 --hold-s3 --port2 COM12   # 抓 C5 时按住 S3

只读输出，不发送任何命令。
"""
import sys
import time

import serial

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]

port = args[0] if args else "COM15"
secs = float(args[1]) if len(args) > 1 else 8.0
baud = 115200

def open_port(p):
    return serial.Serial(p, baud, timeout=0.2)

# 抓 C5 时先把 S3 按住复位，防止 GPIO42 周期性拉低打断
s3 = None
if "--hold-s3" in flags:
    port2 = "COM15"
    for i, f in enumerate(sys.argv):
        if f == "--port2" and i + 1 < len(sys.argv):
            port2 = sys.argv[i + 1]
    try:
        s3 = serial.Serial(port2, baud, timeout=0.2)
        s3.setDTR(False)
        s3.setRTS(True)   # EN 低 = 复位保持
        print(f"[hold] {port2} RTS asserted (S3 held in reset)", flush=True)
    except Exception as e:
        print(f"[hold] failed: {e}", flush=True)

ser = open_port(port)
if "--reset" in flags:
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
    time.sleep(0.05)

t0 = time.time()
buf = b""
try:
    while time.time() - t0 < secs:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
finally:
    ser.close()
    if s3:
        s3.close()
