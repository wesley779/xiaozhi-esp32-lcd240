#include "single_led.h"
#include "application.h"
#include "settings.h"
#include <cmath>
#include <esp_log.h> 

#define TAG "SingleLed"

#define DEFAULT_BRIGHTNESS 4
#define HIGH_BRIGHTNESS 16
#define LOW_BRIGHTNESS 2

#define BLINK_INFINITE -1

// 呼吸律动：40 ms 一格，约 2.1 秒一轮，亮度在 15%~100% 之间余弦过渡
#define BREATH_TICK_US 40000
#define BREATH_TICKS_PER_CYCLE 52
#define BREATH_MIN_PERCENT 15


SingleLed::SingleLed(gpio_num_t gpio) {
    if (gpio == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "SingleLed initialized with GPIO_NUM_NC, LED will not function");
        return;
    }

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = 1;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    esp_timer_create_args_t blink_timer_args = {
        .callback = [](void *arg) {
            auto led = static_cast<SingleLed*>(arg);
            led->OnBlinkTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "blink_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&blink_timer_args, &blink_timer_));

    esp_timer_create_args_t breath_timer_args = {
        .callback = [](void *arg) {
            auto led = static_cast<SingleLed*>(arg);
            led->OnBreathTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "breath_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&breath_timer_args, &breath_timer_));

    LoadIdleColorFromSettings();
    LoadBreathingFromSettings();
}

SingleLed::~SingleLed() {
    if (blink_timer_ != nullptr) {
        esp_timer_stop(blink_timer_);
        esp_timer_delete(blink_timer_);
        blink_timer_ = nullptr;
    }
    if (breath_timer_ != nullptr) {
        esp_timer_stop(breath_timer_);
        esp_timer_delete(breath_timer_);
        breath_timer_ = nullptr;
    }
    if (led_strip_ != nullptr) {
        led_strip_del(led_strip_);
        led_strip_ = nullptr;
    }
}


void SingleLed::SetColor(uint8_t r, uint8_t g, uint8_t b) {
    r_ = r;
    g_ = g;
    b_ = b;
}

void SingleLed::TurnOn() {
    if (led_strip_ == nullptr) {
        return;
    }
    
    StopBreathing();
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    led_strip_set_pixel(led_strip_, 0, r_, g_, b_);
    led_strip_refresh(led_strip_);
}

void SingleLed::TurnOff() {
    if (led_strip_ == nullptr) {
        return;
    }

    StopBreathing();
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    led_strip_clear(led_strip_);
}

void SingleLed::StartBreathing() {
    if (led_strip_ == nullptr || breath_timer_ == nullptr || !has_idle_color_ || !breathing_) {
        return;
    }
    breath_tick_ = 0;
    esp_timer_stop(breath_timer_);  // 已在跑就先归零，保证从头开始呼吸
    OnBreathTimer();                // 立刻点亮第一帧
    esp_timer_start_periodic(breath_timer_, BREATH_TICK_US);
}

void SingleLed::StopBreathing() {
    if (breath_timer_ != nullptr) {
        esp_timer_stop(breath_timer_);
    }
}

void SingleLed::OnBreathTimer() {
    if (led_strip_ == nullptr || !has_idle_color_) {
        StopBreathing();
        return;
    }

    // 余弦曲线：0 -> 满亮 -> 0，避免线性波形顶端的“顿一下”
    const float kPi = 3.14159265f;
    float phase = 2.0f * kPi * (float)breath_tick_ / (float)BREATH_TICKS_PER_CYCLE;
    float k = 0.5f - 0.5f * cosf(phase);  // 0..1
    int percent = BREATH_MIN_PERCENT +
                  (int)((100 - BREATH_MIN_PERCENT) * k + 0.5f);
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;

    auto scale = [percent](uint8_t v) -> uint8_t {
        int out = (int)v * percent / 100;
        return (uint8_t)(out > 255 ? 255 : out);
    };

    breath_tick_ = (breath_tick_ + 1) % BREATH_TICKS_PER_CYCLE;

    std::lock_guard<std::mutex> lock(mutex_);
    led_strip_set_pixel(led_strip_, 0, scale(idle_r_), scale(idle_g_), scale(idle_b_));
    led_strip_refresh(led_strip_);
}

void SingleLed::SetBreathing(bool on) {
    breathing_ = on;
    SaveBreathingToSettings();

    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        if (has_idle_color_ && breathing_) {
            StartBreathing();
        } else if (has_idle_color_) {
            StopBreathing();
            SetColor(idle_r_, idle_g_, idle_b_);
            TurnOn();
        }
    }
    ESP_LOGI(TAG, "Breathing %s", on ? "enabled" : "disabled");
}

void SingleLed::SaveBreathingToSettings() {
    Settings settings("led", true);
    settings.SetInt("idle_breath", breathing_ ? 1 : 0);
}

void SingleLed::LoadBreathingFromSettings() {
    if (led_strip_ == nullptr) {
        return;
    }
    Settings settings("led");
    breathing_ = settings.GetInt("idle_breath", 1) != 0;
}

void SingleLed::BlinkOnce() {
    Blink(1, 100);
}

void SingleLed::Blink(int times, int interval_ms) {
    StartBlinkTask(times, interval_ms);
}

void SingleLed::StartContinuousBlink(int interval_ms) {
    StartBlinkTask(BLINK_INFINITE, interval_ms);
}

void SingleLed::StartBlinkTask(int times, int interval_ms) {
    if (led_strip_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    
    blink_counter_ = times * 2;
    blink_interval_ms_ = interval_ms;
    esp_timer_start_periodic(blink_timer_, interval_ms * 1000);
}

void SingleLed::OnBlinkTimer() {
    std::lock_guard<std::mutex> lock(mutex_);
    blink_counter_--;
    if (blink_counter_ & 1) {
        led_strip_set_pixel(led_strip_, 0, r_, g_, b_);
        led_strip_refresh(led_strip_);
    } else {
        led_strip_clear(led_strip_);

        if (blink_counter_ == 0) {
            esp_timer_stop(blink_timer_);
        }
    }
}


void SingleLed::OnStateChanged() {
    auto& app = Application::GetInstance();
    auto device_state = app.GetDeviceState();
    switch (device_state) {
        case kDeviceStateStarting:
            SetColor(0, 0, DEFAULT_BRIGHTNESS);
            StartContinuousBlink(100);
            break;
        case kDeviceStateWifiConfiguring:
            SetColor(0, 0, DEFAULT_BRIGHTNESS);
            StartContinuousBlink(500);
            break;
        case kDeviceStateIdle:
            if (has_idle_color_) {
                SetColor(idle_r_, idle_g_, idle_b_);
                if (breathing_) {
                    StartBreathing();
                } else {
                    TurnOn();
                }
            } else {
                TurnOff();
            }
            break;
        case kDeviceStateConnecting:
            SetColor(0, 0, DEFAULT_BRIGHTNESS);
            TurnOn();
            break;
        case kDeviceStateListening:
        case kDeviceStateAudioTesting:
            if (app.IsVoiceDetected()) {
                SetColor(HIGH_BRIGHTNESS, 0, 0);
            } else {
                SetColor(LOW_BRIGHTNESS, 0, 0);
            }
            TurnOn();
            break;
        case kDeviceStateSpeaking:
        case kDeviceStateNotifying:
            SetColor(0, DEFAULT_BRIGHTNESS, 0);
            TurnOn();
            break;
        case kDeviceStateUpgrading:
            SetColor(0, DEFAULT_BRIGHTNESS, 0);
            StartContinuousBlink(100);
            break;
        case kDeviceStateActivating:
            SetColor(0, DEFAULT_BRIGHTNESS, 0);
            StartContinuousBlink(500);
            break;
        default:
            ESP_LOGW(TAG, "Unknown led strip event: %d", device_state);
            return;
    }
}

void SingleLed::SetIdleColor(uint8_t r, uint8_t g, uint8_t b) {
    if (led_strip_ == nullptr) {
        return;
    }

    has_idle_color_ = true;
    idle_r_ = r;
    idle_g_ = g;
    idle_b_ = b;
    SaveIdleColorToSettings();

    // 只有当前处于待机才立即点亮；其他状态沿用原有指示，回到待机后自动生效
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        SetColor(r, g, b);
        if (breathing_) {
            StartBreathing();
        } else {
            TurnOn();
        }
    }
    ESP_LOGI(TAG, "Idle color set to R%d G%d B%d", r, g, b);
}

void SingleLed::ClearIdleColor() {
    has_idle_color_ = false;
    SaveIdleColorToSettings();

    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        TurnOff();
    }
    ESP_LOGI(TAG, "Idle color cleared, LED back to built-in indication");
}

void SingleLed::SaveIdleColorToSettings() {
    Settings settings("led", true);
    settings.SetInt("idle_on", has_idle_color_ ? 1 : 0);
    settings.SetInt("idle_r", idle_r_);
    settings.SetInt("idle_g", idle_g_);
    settings.SetInt("idle_b", idle_b_);
}

void SingleLed::LoadIdleColorFromSettings() {
    if (led_strip_ == nullptr) {
        return;
    }

    Settings settings("led");
    has_idle_color_ = settings.GetInt("idle_on", 0) != 0;
    idle_r_ = static_cast<uint8_t>(settings.GetInt("idle_r", 0));
    idle_g_ = static_cast<uint8_t>(settings.GetInt("idle_g", 0));
    idle_b_ = static_cast<uint8_t>(settings.GetInt("idle_b", 0));
    ESP_LOGI(TAG, "Restored idle color: on=%d R%d G%d B%d", has_idle_color_ ? 1 : 0, idle_r_,
             idle_g_, idle_b_);
}
