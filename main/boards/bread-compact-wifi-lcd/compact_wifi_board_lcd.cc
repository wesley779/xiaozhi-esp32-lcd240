#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "adc_battery_monitor.h"
#include "servo_controller.h"
#include "weather/weather_service.h"
#include "pc/pc_status_service.h"

#include <memory>
#include <cstdio>
#include <string>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

// 舵机自检：扫 9600 / 115200 两种波特率，各自试通道 1 和通道 0，
// 每个组合做一次 60° -> 120° -> 90° 的往返，用户盯着舵机看哪组动起来即可定位问题。
static void ServoSelfTestTask(void* arg) {
    auto servo = static_cast<ServoController*>(arg);
    static const int kChannels[] = {1, 0};
    static const int kBauds[] = {9600, 115200};
    int original_baud = servo->GetBaud();  // 结束时必须还原，否则后续指令全废

    for (int b = 0; b < 2; ++b) {
        servo->SetBaud(kBauds[b]);
        vTaskDelay(pdMS_TO_TICKS(200));
        for (int c = 0; c < 2; ++c) {
            int ch = kChannels[c];
            ESP_LOGI("ServoTest", "baud=%d channel=%d 60->120->90", kBauds[b], ch);
            servo->SetAngle(ch, 60, 600);
            vTaskDelay(pdMS_TO_TICKS(900));
            servo->SetAngle(ch, 120, 600);
            vTaskDelay(pdMS_TO_TICKS(900));
            servo->SetAngle(ch, 90, 600);
            vTaskDelay(pdMS_TO_TICKS(900));
        }
    }
    servo->SetBaud(original_baud);
    ESP_LOGI("ServoTest", "self test finished, baud restored to %d", original_baud);
    vTaskDelete(nullptr);
}

// 通道扫描：在当前波特率下依次点动 0..max_channel，谁动就说明那路插着舵机
struct ServoSweepArgs {
    ServoController* servo;
    int max_channel;
};

static void ServoSweepTask(void* arg) {
    auto* args = static_cast<ServoSweepArgs*>(arg);
    auto servo = args->servo;
    int max_channel = args->max_channel;
    for (int ch = 0; ch <= max_channel; ++ch) {
        ESP_LOGI("ServoTest", "sweep channel=%d", ch);
        servo->SetAngle(ch, 100, 500);
        vTaskDelay(pdMS_TO_TICKS(700));
        servo->SetAngle(ch, 140, 500);
        vTaskDelay(pdMS_TO_TICKS(700));
        servo->SetAngle(ch, 120, 500);
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    ESP_LOGI("ServoTest", "sweep finished 0..%d", max_channel);
    delete args;
    vTaskDelete(nullptr);
}

// 通道表达式解析：支持 "all"（=0..23）、"1-8" 区间、"1,2,3" 列表、混写 "1-4,7,12-15"
static int ParseServoChannels(const std::string& text, int* out, int max_count) {
    std::string lower;
    lower.reserve(text.size());
    for (char c : text) {
        lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    int count = 0;
    if (lower.find("all") != std::string::npos || lower.find("全部") != std::string::npos) {
        for (int i = 0; i <= SERVO_MAX_CHANNEL && count < max_count; ++i) {
            out[count++] = i;
        }
        return count;
    }

    const char* s = lower.c_str();
    while (*s != '\0' && count < max_count) {
        while (*s == ' ' || *s == ',' || *s == ';' || *s == '\t') {
            ++s;
        }
        if (*s == '\0') {
            break;
        }
        if (*s < '0' || *s > '9') {
            ++s;
            continue;
        }
        int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            ++s;
        }
        if (*s == '-') {  // 区间 a-b
            ++s;
            int hi = 0;
            bool has_hi = false;
            while (*s >= '0' && *s <= '9') {
                hi = hi * 10 + (*s - '0');
                ++s;
                has_hi = true;
            }
            if (has_hi) {
                if (v > hi) {
                    int t = v;
                    v = hi;
                    hi = t;
                }
                if (hi > SERVO_MAX_CHANNEL) hi = SERVO_MAX_CHANNEL;
                for (int i = v; i <= hi && count < max_count; ++i) {
                    out[count++] = i;
                }
                continue;
            }
        }
        if (v <= SERVO_MAX_CHANNEL) {
            out[count++] = v;
        }
    }
    return count;
}

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

#if defined(LCD_TYPE_GC9A01_SERIAL)
#include "esp_lcd_gc9a01.h"
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                0x04, 0x12, 0x14, 0x1f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                0x0C, 0x1A, 0x14, 0x1E},
    14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif
 
#define TAG "CompactWifiBoardLCD"

class CompactWifiBoardLCD : public WifiBoard {
private:
 
    Button boot_button_;
    Button volume_down_button_;
    LcdDisplay* display_;
    std::unique_ptr<AdcBatteryMonitor> battery_monitor_;

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };        
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif
        
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef  LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // Volume down key on GPIO39 (same key as the non-LCD bread board).
        if (VOLUME_DOWN_BUTTON_GPIO != GPIO_NUM_NC) {
            volume_down_button_.OnClick([this]() {
                auto codec = GetAudioCodec();
                auto volume = codec->output_volume() - 10;
                if (volume < 0) {
                    volume = 0;
                }
                codec->SetOutputVolume(volume);
                GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
            });

            volume_down_button_.OnLongPress([this]() {
                GetAudioCodec()->SetOutputVolume(0);
                GetDisplay()->ShowNotification(Lang::Strings::MUTED);
            });
        }
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);

        // 板载 WS2812 待机氛围灯：只在 idle 生效，唤醒/配网/升级时由原有指示自动接管
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool(
            "self.led.set_color",
            "设置板载RGB指示灯在待机(未唤醒)时的颜色。可选 color: red/orange/yellow/green/"
            "cyan/blue/purple/pink/white/warm，也接受中文 红/橙/黄/绿/青/蓝/紫/粉/白/暖。"
            "brightness 为亮度百分比 1-100。唤醒对话、配网、升级时该颜色会被系统原有指示灯"
            "逻辑自动接管，回到待机后恢复。",
            PropertyList({
                Property("color", kPropertyTypeString, std::string("white")),
                Property("brightness", kPropertyTypeInteger, 40, 1, 100),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string color_name = properties["color"].value<std::string>();
                int brightness = properties["brightness"].value<int>();
                auto rgb = LedColorFromName(color_name);
                uint8_t r = ScaleLedChannel(rgb.r, brightness);
                uint8_t g = ScaleLedChannel(rgb.g, brightness);
                uint8_t b = ScaleLedChannel(rgb.b, brightness);
                ESP_LOGI(TAG, "MCP led.set_color: %s (%d%%) -> R%d G%d B%d", color_name.c_str(),
                         brightness, r, g, b);
                GetSingleLed()->SetIdleColor(r, g, b);
                return true;
            });

        mcp_server.AddTool("self.led.turn_on",
            "打开板载RGB指示灯（待机时亮起），使用上次设定的颜色，没有设定时用白色。",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                auto led = GetSingleLed();
                if (led->HasIdleColor()) {
                    uint8_t saved_r = 0, saved_g = 0, saved_b = 0;
                    led->GetIdleColor(saved_r, saved_g, saved_b);
                    led->SetIdleColor(saved_r, saved_g, saved_b);
                } else {
                    led->SetIdleColor(255, 255, 255);
                }
                ESP_LOGI(TAG, "MCP led.turn_on");
                return true;
            });

        mcp_server.AddTool("self.led.turn_off",
            "关闭板载RGB指示灯的待机颜色，恢复系统默认的指示灯逻辑（待机时熄灭）。",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "MCP led.turn_off");
                GetSingleLed()->ClearIdleColor();
                return true;
            });

        mcp_server.AddTool("self.led.set_breath",
            "开启或关闭板载RGB指示灯的待机呼吸律动（颜色明暗缓慢起伏，约2秒一轮）。"
            "enabled=true 开启呼吸，false 则待机时保持恒定亮度。",
            PropertyList({
                Property("enabled", kPropertyTypeBoolean, true),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                bool enabled = properties["enabled"].value<bool>();
                ESP_LOGI(TAG, "MCP led.set_breath: %s", enabled ? "on" : "off");
                GetSingleLed()->SetBreathing(enabled);
                return true;
            });

        mcp_server.AddTool("self.led.get_state",
            "查询板载RGB指示灯当前的待机颜色设定和呼吸律动状态。",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                auto led = GetSingleLed();
                uint8_t r = 0, g = 0, b = 0;
                led->GetIdleColor(r, g, b);
                char buf[112];
                snprintf(buf, sizeof(buf), "{\"on\":%s,\"r\":%u,\"g\":%u,\"b\":%u,\"breath\":%s}",
                         led->HasIdleColor() ? "true" : "false", r, g, b,
                         led->IsBreathing() ? "true" : "false");
                return std::string(buf);
            });

        mcp_server.AddTool("self.weather.refresh",
            "立刻刷新一次待机界面上的天气（不等自动的十分钟周期）。",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "MCP weather.refresh");
                WeatherService::GetInstance().RequestRefresh();
                return std::string("{\"ok\":true,\"queued\":true}");
            });

        mcp_server.AddTool("self.pc.status",
            "查询同一 WiFi 下这台电脑的运行状态：CPU 占用、内存占用、显卡占用和温度、C 盘剩余"
            "空间、已经开机多久。结果为一整句中文，直接念给用户听。用户问“我电脑怎么样”"
            "“电脑卡不卡”“电脑开了多久”时调用。",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "MCP pc.status");
                auto& svc = PcStatusService::GetInstance();
                if (!svc.HasData()) {
                    svc.RequestRefresh();
                    for (int i = 0; i < 15 && !svc.HasData(); ++i) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                }
                return svc.GetSummary();
            });

        mcp_server.AddTool("self.pc.refresh",
            "立刻重新拉取一次电脑运行状态（平时每 20 秒自动更新，待机第三屏会显示）。",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "MCP pc.refresh");
                PcStatusService::GetInstance().RequestRefresh();
                return std::string("{\"ok\":true,\"queued\":true}");
            });

        // 幻尔 LSC-24 舵机控制板：UART1，9600 8N1，ASCII 指令 "#1P1500T1000!"
        static ServoController servo(static_cast<uart_port_t>(SERVO_UART_PORT),
                                     SERVO_UART_TX_GPIO, SERVO_UART_RX_GPIO, SERVO_UART_BAUD);
        servo.Initialize();

        mcp_server.AddTool("self.servo.set_angle",
            "控制舵机控制板上某一路舵机转到指定角度。channel 是控制板上的通道号 0-23"
            "（与丝印一致）；angle 为 0-180 度；duration_ms 为转动时间（毫秒，0 为最快，"
            "默认 500 约半秒走完）。多路要一起转请用 self.servo.set_many。",
            PropertyList({
                Property("channel", kPropertyTypeInteger, 1, 0, 23),
                Property("angle", kPropertyTypeInteger, 90, 0, 180),
                Property("duration_ms", kPropertyTypeInteger, 500, 0, 9999),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int channel = properties["channel"].value<int>();
                int angle = properties["angle"].value<int>();
                int duration_ms = properties["duration_ms"].value<int>();
                ESP_LOGI(TAG, "MCP servo.set_angle: ch%d -> %d deg in %d ms", channel, angle,
                         duration_ms);
                if (!servo.SetAngle(channel, angle, duration_ms)) {
                    return std::string("{\"ok\":false}");
                }
                char buf[72];
                snprintf(buf, sizeof(buf), "{\"ok\":true,\"channel\":%d,\"angle\":%d}", channel,
                         angle);
                return std::string(buf);
            });

        mcp_server.AddTool("self.servo.set_pulse",
            "直接用脉宽(500-2500us)控制某一路舵机，用于精细校准中位或行程。"
            "channel 是通道号 0-23；pulse_us 常用 1500 为中位；duration_ms 为转动时间。",
            PropertyList({
                Property("channel", kPropertyTypeInteger, 1, 0, 23),
                Property("pulse_us", kPropertyTypeInteger, 1500, 500, 2500),
                Property("duration_ms", kPropertyTypeInteger, 500, 0, 9999),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int channel = properties["channel"].value<int>();
                int pulse = properties["pulse_us"].value<int>();
                int duration_ms = properties["duration_ms"].value<int>();
                ESP_LOGI(TAG, "MCP servo.set_pulse: ch%d -> %d us in %d ms", channel, pulse,
                         duration_ms);
                if (!servo.SetPulse(channel, pulse, duration_ms)) {
                    return std::string("{\"ok\":false}");
                }
                char buf[72];
                snprintf(buf, sizeof(buf), "{\"ok\":true,\"channel\":%d,\"pulse_us\":%d}", channel,
                         pulse);
                return std::string(buf);
            });

        mcp_server.AddTool("self.servo.center",
            "把指定几路舵机全部转回中位(1500us)。channels 支持 \"all\"（全部 24 路）、"
            "区间 \"1-8\"、列表 \"1,2,3\"，也可以混写 \"1-4,7,12-15\"。",
            PropertyList({
                Property("channels", kPropertyTypeString, std::string("all")),
                Property("duration_ms", kPropertyTypeInteger, 800, 0, 9999),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string text = properties["channels"].value<std::string>();
                int duration_ms = properties["duration_ms"].value<int>();

                int ch[SERVO_CHANNEL_COUNT];
                int count = ParseServoChannels(text, ch, SERVO_CHANNEL_COUNT);
                if (count == 0) {
                    return std::string("{\"ok\":false,\"error\":\"no channel\"}");
                }

                ESP_LOGI(TAG, "MCP servo.center: %d channel(s) in %d ms", count, duration_ms);
                if (!servo.CenterMany(ch, count, duration_ms)) {
                    return std::string("{\"ok\":false}");
                }
                char buf[96];
                snprintf(buf, sizeof(buf), "{\"ok\":true,\"channels\":%d}", count);
                return std::string(buf);
            });

        mcp_server.AddTool("self.servo.set_many",
            "一句话同时控制多路舵机转到相同角度（比逐路调用快，24 路一次到位）。"
            "channels 支持 \"all\"、区间 \"1-8\"、列表 \"1,2,3\"、混写 \"1-4,7\"；"
            "angle 为 0-180 度；duration_ms 为转动时间。",
            PropertyList({
                Property("channels", kPropertyTypeString, std::string("all")),
                Property("angle", kPropertyTypeInteger, 90, 0, 180),
                Property("duration_ms", kPropertyTypeInteger, 800, 0, 9999),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string text = properties["channels"].value<std::string>();
                int angle = properties["angle"].value<int>();
                int duration_ms = properties["duration_ms"].value<int>();

                int ch[SERVO_CHANNEL_COUNT];
                int pulses[SERVO_CHANNEL_COUNT];
                int count = ParseServoChannels(text, ch, SERVO_CHANNEL_COUNT);
                if (count == 0) {
                    return std::string("{\"ok\":false,\"error\":\"no channel\"}");
                }
                int pulse = 500 + static_cast<int>(angle * 2000.0f / 180.0f + 0.5f);
                for (int i = 0; i < count; ++i) {
                    pulses[i] = pulse;
                }

                ESP_LOGI(TAG, "MCP servo.set_many: %d channel(s) -> %d deg (%d us) in %d ms",
                         count, angle, pulse, duration_ms);
                if (!servo.SetPulseMany(ch, pulses, count, duration_ms)) {
                    return std::string("{\"ok\":false}");
                }
                char buf[96];
                snprintf(buf, sizeof(buf), "{\"ok\":true,\"channels\":%d,\"angle\":%d}", count,
                         angle);
                return std::string(buf);
            });


        // 诊断三件套：自检（扫波特率+通道号）、监听回传、改波特率
        mcp_server.AddTool("self.servo.selftest",
            "舵机自检。会在 9600 和 115200 两种波特率下，依次让通道 1 和通道 0 各做一次"
            "60度->120度->90度的往返，约 8 秒。用户盯着舵机看哪个组合动起来即可定位问题。",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "MCP servo.selftest start");
                xTaskCreate(ServoSelfTestTask, "servo_selftest", 3072, &servo, 5, nullptr);
                return std::string("{\"ok\":true,\"running\":true}");
            });

        mcp_server.AddTool("self.servo.listen",
            "监听舵机控制板通过串口返回的数据（原始文本）。返回空说明 RX 方向没收到任何"
            "字节，通常意味着 ESP32 的 RX 与控制板 TX 没接对，或波特率不匹配。",
            PropertyList({
                Property("timeout_ms", kPropertyTypeInteger, 500, 50, 2000),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int timeout_ms = properties["timeout_ms"].value<int>();
                std::string raw = servo.ReadRaw(timeout_ms);
                if (raw.empty()) {
                    return std::string("{\"ok\":true,\"bytes\":0,\"data\":\"\"}");
                }
                std::string escaped;
                for (unsigned char c : raw) {
                    if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
                        escaped += (char)c;
                    } else {
                        char tmp[8];
                        snprintf(tmp, sizeof(tmp), "\\x%02X", c);
                        escaped += tmp;
                    }
                }
                char prefix[64];
                snprintf(prefix, sizeof(prefix), "{\"ok\":true,\"bytes\":%d,\"data\":\"",
                         (int)raw.size());
                return std::string(prefix) + escaped + "\"}";
            });

        mcp_server.AddTool("self.servo.set_baud",
            "更改与控制板通讯的波特率（常用 9600 / 115200，改完立即生效）。",
            PropertyList({
                Property("baud", kPropertyTypeInteger, 9600, 2400, 115200),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int baud = properties["baud"].value<int>();
                bool ok = servo.SetBaud(baud);
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"ok\":%s,\"baud\":%d}", ok ? "true" : "false", baud);
                return std::string(buf);
            });

        mcp_server.AddTool("self.servo.stop",
            "让舵机停在当前角度（发送 $DST! 停止全部；给 channel 则只停那一路）。",
            PropertyList({
                Property("channel", kPropertyTypeInteger, -1, -1, 23),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int channel = properties["channel"].value<int>();
                bool ok = (channel < 0) ? servo.StopAll() : servo.Stop(channel);
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"ok\":%s,\"channel\":%d}", ok ? "true" : "false",
                         channel);
                return std::string(buf);
            });

        mcp_server.AddTool("self.servo.scan",
            "通道扫描：在 0 到 max_channel 之间依次让每一路小幅点动（100°→140°→120°），"
            "哪个通道有舵机动起来，就说明那个舵机插在那一路上。装新舵机后用它确认通道号。",
            PropertyList({
                Property("max_channel", kPropertyTypeInteger, 23, 0, 23),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int max_channel = properties["max_channel"].value<int>();
                auto* args = new ServoSweepArgs{&servo, max_channel};
                xTaskCreate(ServoSweepTask, "servo_sweep", 3072, args, 5, nullptr);
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"ok\":true,\"max_channel\":%d}", max_channel);
                return std::string(buf);
            });

        mcp_server.AddTool("self.servo.info",
            "查询舵机控制板的连接状态：串口、波特率、可控制通道数、当前通讯是否就绪。",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "{\"ok\":true,\"port\":\"UART%d\",\"tx\":%d,\"rx\":%d,\"baud\":%d,"
                         "\"channels\":%d,\"max_channel\":%d}",
                         SERVO_UART_PORT, (int)SERVO_UART_TX_GPIO, (int)SERVO_UART_RX_GPIO,
                         servo.GetBaud(), SERVO_CHANNEL_COUNT, SERVO_MAX_CHANNEL);
                return std::string(buf);
            });

    }

    // 颜色名 → 满亮度 RGB
    struct LedRgb {
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    static LedRgb LedColorFromName(const std::string& name) {
        std::string n = name;
        for (auto& c : n) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        if (n == "red" || n == "红" || n == "红色") return {255, 0, 0};
        if (n == "orange" || n == "橙" || n == "橙色") return {255, 120, 0};
        if (n == "yellow" || n == "黄" || n == "黄色") return {255, 220, 0};
        if (n == "green" || n == "绿" || n == "绿色") return {0, 255, 0};
        if (n == "cyan" || n == "青" || n == "青色") return {0, 255, 255};
        if (n == "blue" || n == "蓝" || n == "蓝色") return {0, 0, 255};
        if (n == "purple" || n == "紫" || n == "紫色") return {150, 0, 255};
        if (n == "pink" || n == "粉" || n == "粉色") return {255, 60, 150};
        if (n == "warm" || n == "暖" || n == "暖白") return {255, 150, 60};
        // white 及未知颜色
        return {255, 255, 255};
    }

    static uint8_t ScaleLedChannel(uint8_t value, int brightness_percent) {
        if (brightness_percent < 1) {
            brightness_percent = 1;
        }
        if (brightness_percent > 100) {
            brightness_percent = 100;
        }
        return static_cast<uint8_t>((static_cast<int>(value) * brightness_percent) / 100);
    }

    SingleLed* GetSingleLed() {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    // 电池电压采样（ADC 分压）。GPIO1 == ESP32-S3 的 ADC1_CH0。
    void InitializeBattery() {
        battery_monitor_ = std::make_unique<AdcBatteryMonitor>(
            ADC_UNIT_1, ADC_CHANNEL_0,
            BATTERY_UPPER_RESISTOR, BATTERY_LOWER_RESISTOR,
            BATTERY_CHARGING_PIN);
    }

public:
    CompactWifiBoardLCD() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeBattery();
        InitializeButtons();
        InitializeTools();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
        
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        if (!battery_monitor_) {
            return false;
        }
        charging = battery_monitor_->IsCharging();
        discharging = battery_monitor_->IsDischarging();
        level = battery_monitor_->GetBatteryLevel();
        // 每 10 秒打印一次，便于用万用表对照校准分压
        static int64_t last_log_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_log_us >= 10 * 1000 * 1000) {
            last_log_us = now_us;
            ESP_LOGI(TAG, "Battery: level=%d%% charging=%d discharging=%d", level, (int)charging,
                     (int)discharging);
        }
        return true;
    }

    virtual Led* GetLed() override {
        return GetSingleLed();
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }
};

DECLARE_BOARD(CompactWifiBoardLCD);
