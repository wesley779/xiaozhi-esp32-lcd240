"""抓一段串口日志，用来确认设备没有 panic、天气/电脑状态拉取正常。

用法：
    python tools/capture_serial.py 40 diag.txt [COM13]
需要串口空闲（先关掉 idf.py monitor）。
"""
import sys

import serial

seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 40
out = sys.argv[2] if len(sys.argv) > 2 else "diag-serial.txt"
port = sys.argv[3] if len(sys.argv) > 3 else "COM13"

KEYS = ("WeatherService", "Weather", "Air", "PcStatus", "restored", "IDLE", "panic", "Guru")

s = serial.Serial(port, 115200, timeout=0.2)
s.reset_input_buffer()
# 断电复位，从启动日志开始抓
s.setDTR(False)
s.setRTS(True)
import time

time.sleep(0.2)
s.setRTS(False)

buf = []
end = time.time() + seconds
while time.time() < end:
    d = s.read(4096)
    if d:
        buf.append(d.decode("utf-8", "replace"))
s.close()

txt = "".join(buf)
with open(out, "w", encoding="utf-8") as f:
    f.write(txt)
for line in txt.splitlines():
    if any(k in line for k in KEYS):
        print(line)
print("TOTAL_LINES", len(txt.splitlines()))
