# 240×240 面包板小智（本项目改造说明）

本仓库基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，在此基础上做了一套「桌面小智」改造：44 脚 ESP32-S3 面包板 + 1.54 寸 240×240 SPI 屏，带待机信息三屏、24 路舵机语音控制、局域网电脑状态显示。

上游原始说明见 [README.md](README.md)。

## 硬件

| 部件 | 说明 |
|---|---|
| 主控 | ESP32-S3 面包板开发板（44 脚，8MB PSRAM / 16MB Flash） |
| 屏幕 | 1.54" 240×240 ST7789 SPI，丝印 `GND VCC SCL SDA RST DC CS BL` |
| 舵机控制板 | 松甲科技 Yeahbot 24 路板，UART1：TX=GPIO17 / RX=GPIO38，115200 8N1，需共地，5V 单独供电 |
| 舵机 | 协议 `#%03dP%04dT%04d!`（15 字符补零），多路用 `{...}`；旧格式 `#1P1500T500!` 不生效 |

## 功能

### 待机三屏（10 秒自动轮播）

| 屏 | 内容 |
|---|---|
| 第一屏 | 大时钟 + `11月13日 周五` + 天气图标 + 当前温度 + `高X°  低Y°` + `体感X°  PM2.5 N 等级` |
| 第二屏 | 未来三天，每行「天气小图标 + 星期 + `最高°/最低°`」 |
| 第三屏 | 同 WiFi 电脑状态：`● 在线  已开 2小时42分` / `CPU 16%  内存 77%` / `显卡 3%  50°` / `C盘剩 16G` |

切换可用语音触发，也可等 10 秒自动翻页；离开待机界面时暂停翻页，回来从第一屏开始。

### 天气

- 数据源 Open-Meteo（免 Key）：`api.open-meteo.com` 取 `temperature_2m / apparent_temperature / weather_code / daily`，`air-quality-api.open-meteo.com` 取 `pm2_5`
- 默认坐标天津 `39.13 / 117.20`，改 `main/weather/weather_service.cc` 顶部的 `WEATHER_LAT / WEATHER_LON`
- 成功后 10 分钟再抓、失败 60 秒重试、开机 1 秒内先用 NVS 缓存点亮（实测 0.8 s 出数）
- 实测公网 TLS 单次 8.5–13.6 s，所以周期不宜再缩短；空气质量与天气各自计时
- PM2.5 等级按中国 24 小时均值：≤35 优 / ≤75 良 / ≤115 轻度 / ≤150 中度 / ≤250 重度 / 其余严重

### 二十四路舵机

`main/servo/servo_controller.*` + `main/boards/bread-compact-wifi-lcd/config.h` 的 `SERVO_CHANNEL_COUNT 24`，语音工具：

| 工具 | 说明 |
|---|---|
| `self.servo.set_angle` | 单路：channel 0–23、angle 0–180、duration_ms |
| `self.servo.set_many` | 多路：支持 `all` / `1-8` / `1,2,3` / `1-4,7,12-15` |
| `self.servo.center` | 多路回中，默认全部 24 路 |
| `self.servo.scan` | 逐路扫描，默认扫到 23 |
| `self.servo.info` | 读回控制板信息 |

多路报文按每 8 路一包、包间 20 ms 发送，避免长报文被控制板截尾。

### 电脑状态

电脑端运行 `tools/pc_status_server.py`（依赖 `psutil`）：

```bash
python tools/pc_status_server.py 8765
curl http://<电脑IP>:8765/status       # JSON
curl http://<电脑IP>:8765/status?text=1  # 一整句中文，语音播报用
```

ESP32 侧改 `main/pc/pc_status_service.cc` 顶部的 `PC_STATUS_URL`（也可编译时 `-DPC_STATUS_URL=...` 覆盖），每 20 秒拉一次，失败 10 秒重试，连续失败显示「离线」。

语音可问：「我电脑怎么样」「电脑卡不卡」「电脑开了多久」→ `self.pc.status`；「刷新电脑状态」→ `self.pc.refresh`。

### 自生成中文字库

`main/display/idle_font_16.c` 是一份 267 字、16 px / 4bpp 的 simhei 字库（约 250 KB 源码），替代 LVGL 内置思源字库，省约 175 KB，并补齐内置字库缺的简体「后」与 `°`。

**改文案后必须重新生成**，否则新字显示空白：

```bash
python tools/gen_idle_font.py     # 依赖 npx + lv_font_conv 1.5.3，自动扫描 lcd_display.cc 的所有非 ASCII 字符
```

## 编译与烧录

```powershell
$env:IDF_TOOLS_PATH='D:\Espressif'
$env:IDF_PYTHON_ENV_PATH='D:\Espressif\tools\python\v6.1-beta1\venv'
. 'D:\esp\v6.1-beta1\esp-idf\export.ps1'

idf.py build

python -m esptool --chip esp32s3 --port COM13 --baud 460800 --before default-reset --after hard-reset write-flash 0x20000 build/xiaozhi.bin
```

板级默认配置在 `sdkconfig.defaults.board`（屏幕 SPI、背光、舵机串口、单灯等）。当前固件体积约 3.01 MB，分区余 27%。

## 目录

```
main/
├── display/            待机三屏、彩色天气图标绘制、自生成字库、背景图
├── weather/            Open-Meteo 天气 + 空气质量，NVS 缓存
├── pc/                 局域网电脑状态轮询
├── servo/              24 路舵机 UART 协议
└── boards/bread-compact-wifi-lcd/   本板引脚与语音工具注册
tools/
├── pc_status_server.py 电脑端状态 HTTP 服务
├── gen_idle_font.py    中文字库生成
└── capture_weather.py  串口日志抓取
```

## 已知限制

- CPU 温度在 Windows 上 `psutil` 拿不到（GPU 温度可由 `nvidia-smi` 提供）
- 24 路批量指令只在 3 个舵机上验证过，未测 24 路同时动作
- 天气图标是纯 LVGL 对象拼的（不用字体/位图），颜色偏深背景上对比度有限
- 加入第三个轮询任务后内部 SRAM 最低水位约 11.8 KB，后续叠加吃内存功能需重新评估
