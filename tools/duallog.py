"""
同时抓 C5(COM12) 的日志，并用 COM15 的 RTS 复位 S3 以触发整机重启。
用法: python tools\duallog.py COM12 COM15 25
"""
import sys
import time

import serial

cp_port = sys.argv[1] if len(sys.argv) > 1 else "COM12"
s3_port = sys.argv[2] if len(sys.argv) > 2 else "COM15"
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 25.0

cp = serial.Serial(cp_port, 115200, timeout=0.1)
s3 = serial.Serial(s3_port, 115200, timeout=0.1)

# 复位 S3（GPIO42 会把 C5 一起复位）
s3.setDTR(False)
s3.setRTS(True)
time.sleep(0.2)
s3.setRTS(False)

t0 = time.time()
while time.time() - t0 < secs:
    d = cp.read(8192)
    if d:
        sys.stdout.write(d.decode("utf-8", "replace"))
        sys.stdout.flush()
    d = s3.read(8192)
    if d:
        t = time.time() - t0
        for line in d.decode("utf-8", "replace").splitlines():
            print(f"[S3 {t:5.1f}] {line}")
        sys.stdout.flush()

cp.close()
s3.close()
