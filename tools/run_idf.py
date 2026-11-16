import os
import runpy
import sys

# 本机装的是 ESP-IDF v6.1（EIM 安装），export.ps1 会因为缺少 riscv32-esp-elf-gdb 而失败，
# 用 Git Bash 时 idf.py 又会因为 MSYSTEM 变量直接退出。
# 这个小脚本负责：清掉 MSYS 标记 -> 用当前 python 跑 idf.py。

for key in ("MSYSTEM", "MSYS", "MINGW_PREFIX", "MINGW_CHOST"):
    os.environ.pop(key, None)

if len(sys.argv) < 2:
    sys.exit("usage: run_idf.py <idf.py.py> [idf.py args...]")

script = sys.argv[1]
sys.path.insert(0, os.path.dirname(script))
sys.argv = [script] + sys.argv[2:]
runpy.run_path(script, run_name="__main__")
