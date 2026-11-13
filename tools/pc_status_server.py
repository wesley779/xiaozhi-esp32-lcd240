"""PC 状态 HTTP 服务：给同 WiFi 下的小智 AI（ESP32-S3）拉取电脑运行状态。

用法：
    python pc_status_server.py            # 默认 0.0.0.0:8765
    python pc_status_server.py 8765       # 指定端口

ESP32 侧请求  http://<本机IP>:8765/status  得到 JSON；
请求 http://<本机IP>:8765/status?text=1 得到一行给语音播报的中文摘要。

依赖：psutil（pip install psutil）
"""
import json
import os
import socket
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import psutil

HOST_NAME = socket.gethostname()
START = time.time()
_last_net = psutil.net_io_counters()
_last_net_ts = time.time()


def gpu_info():
    """用 nvidia-smi 读显卡；没装 N 卡或没装驱动就返回 None。"""
    try:
        out = subprocess.run(
            ["nvidia-smi",
             "--query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=2,
        )
        if out.returncode != 0 or not out.stdout.strip():
            return None
        util, mem_used, mem_total, temp = [p.strip() for p in out.stdout.splitlines()[0].split(",")]
        return {"util": float(util), "mem_used_mb": float(mem_used),
                "mem_total_mb": float(mem_total), "temp": float(temp)}
    except Exception:
        return None


def cpu_temp():
    """Windows 上 psutil 基本拿不到温度；能拿到就用，拿不到返回 None。"""
    try:
        temps = psutil.sensors_temperatures()
        for entries in temps.values():
            for e in entries:
                if e.current:
                    return round(float(e.current), 1)
    except Exception:
        pass
    return None


def collect():
    global _last_net, _last_net_ts
    now = time.time()

    cpu = psutil.cpu_percent(interval=0.5)
    cores = psutil.cpu_count(logical=True)

    vm = psutil.virtual_memory()
    disk = psutil.disk_usage("C:\\")

    net = psutil.net_io_counters()
    dt = max(now - _last_net_ts, 0.001)
    up_kbps = (net.bytes_sent - _last_net.bytes_sent) / dt / 1024
    down_kbps = (net.bytes_recv - _last_net.bytes_recv) / dt / 1024
    _last_net, _last_net_ts = net, now

    batt = psutil.sensors_battery()

    data = {
        "name": HOST_NAME,
        "ts": int(now),
        "cpu": round(cpu, 1),
        "cores": cores,
        "mem": round(vm.percent, 1),
        "mem_used_gb": round((vm.total - vm.available) / 1024 ** 3, 1),
        "mem_total_gb": round(vm.total / 1024 ** 3, 1),
        "disk_c": round(disk.percent, 1),
        "disk_free_gb": round(disk.free / 1024 ** 3, 1),
        "net_up_kbps": round(up_kbps, 0),
        "net_down_kbps": round(down_kbps, 0),
        "uptime_min": int((now - psutil.boot_time()) / 60),
        "cpu_temp": cpu_temp(),
        "gpu": gpu_info(),
        "battery": None if batt is None else {
            "percent": round(batt.percent, 0),
            "plugged": bool(batt.power_plugged),
        },
    }
    return data


def summary_line(d):
    parts = [f"CPU {d['cpu']:.0f}%", f"内存 {d['mem']:.0f}%"]
    gpu = d.get("gpu")
    if gpu:
        parts.append(f"显卡 {gpu['util']:.0f}%")
    if d.get("cpu_temp"):
        parts.append(f"温度 {d['cpu_temp']:.0f}度")
    parts.append(f"C盘剩 {d['disk_free_gb']:.0f}G")
    busy = "正在忙" if d["cpu"] > 40 or (gpu and gpu["util"] > 50) else "基本空闲"
    return f"电脑{('（' + d['name'] + '）')}{busy}，" + "，".join(parts)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        url = urlparse(self.path)
        if url.path not in ("/status", "/"):
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"not found")
            return
        d = collect()
        qs = parse_qs(url.query)
        if qs.get("text"):
            body = summary_line(d).encode("utf-8")
            ctype = "text/plain; charset=utf-8"
        else:
            body = json.dumps(d, ensure_ascii=False).encode("utf-8")
            ctype = "application/json; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass  # 静默，别把 ESP32 每 20 秒的轮询刷满控制台


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"PC status server on http://{socket.gethostbyname(HOST_NAME)}:{port}/status")
    for ip in socket.gethostbyname_ex(HOST_NAME)[2]:
        print(f"  可用地址: http://{ip}:{port}/status")
    server.serve_forever()
