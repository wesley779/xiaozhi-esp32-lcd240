#ifndef _SINGLE_LED_H_
#define _SINGLE_LED_H_

#include "led.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <led_strip.h>
#include <esp_timer.h>
#include <atomic>
#include <mutex>

class SingleLed : public Led {
public:
    SingleLed(gpio_num_t gpio);
    virtual ~SingleLed();

    void OnStateChanged() override;

    // 待机氛围灯：只在 kDeviceStateIdle 时生效，其余状态完全沿用原有指示逻辑
    void SetIdleColor(uint8_t r, uint8_t g, uint8_t b);
    void ClearIdleColor();
    bool HasIdleColor() const { return has_idle_color_; }
    void GetIdleColor(uint8_t& r, uint8_t& g, uint8_t& b) const {
        r = idle_r_;
        g = idle_g_;
        b = idle_b_;
    }

    // 待机呼吸律动：待机时设定色做明暗循环（约 2 秒一轮），默认开启
    void SetBreathing(bool on);
    bool IsBreathing() const { return breathing_; }

private:
    std::mutex mutex_;
    TaskHandle_t blink_task_ = nullptr;
    led_strip_handle_t led_strip_ = nullptr;
    uint8_t r_ = 0, g_ = 0, b_ = 0;
    int blink_counter_ = 0;
    int blink_interval_ms_ = 0;
    esp_timer_handle_t blink_timer_ = nullptr;

    // 语音设定的待机颜色，掉电保存在 NVS 的 led 命名空间
    bool has_idle_color_ = false;
    uint8_t idle_r_ = 0, idle_g_ = 0, idle_b_ = 0;
    void LoadIdleColorFromSettings();
    void SaveIdleColorToSettings();

    // 呼吸律动：自己的定时器，只在待机且有设定色时运行
    bool breathing_ = true;
    esp_timer_handle_t breath_timer_ = nullptr;
    int breath_tick_ = 0;
    void StartBreathing();
    void StopBreathing();
    void OnBreathTimer();
    void SaveBreathingToSettings();
    void LoadBreathingFromSettings();

    void StartBlinkTask(int times, int interval_ms);
    void OnBlinkTimer();

    void BlinkOnce();
    void Blink(int times, int interval_ms);
    void StartContinuousBlink(int interval_ms);
    void TurnOn();
    void TurnOff();
    void SetColor(uint8_t r, uint8_t g, uint8_t b);
};

#endif // _SINGLE_LED_H_
