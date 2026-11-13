#include "servo_controller.h"

#include <cstdio>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Servo"

ServoController::ServoController(uart_port_t port, gpio_num_t tx_gpio, gpio_num_t rx_gpio,
                                 int baud_rate)
    : port_(port), tx_gpio_(tx_gpio), rx_gpio_(rx_gpio), baud_rate_(baud_rate) {}

ServoController::~ServoController() {
    if (initialized_) {
        uart_driver_delete(port_);
        initialized_ = false;
    }
}

bool ServoController::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return true;
    }

    uart_config_t config = {};
    config.baud_rate = baud_rate_;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(port_, 512, 512, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_param_config(port_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(port_);
        return false;
    }
    err = uart_set_pin(port_, tx_gpio_, rx_gpio_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(port_);
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "UART%d ready: TX=GPIO%d RX=GPIO%d %d bps 8N1", (int)port_, (int)tx_gpio_,
             (int)rx_gpio_, baud_rate_);
    return true;
}

bool ServoController::SendRaw(const std::string& command) {
    return SendCommand(command);
}

bool ServoController::SendCommand(const std::string& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        ESP_LOGE(TAG, "not initialized, drop command: %s", command.c_str());
        return false;
    }

    // 指令以 '!' 结尾，再补一个 CRLF：不同固件版本结尾符不一样，多给的空白会被忽略
    std::string payload = command;
    payload += "\r\n";

    int written = uart_write_bytes(port_, payload.c_str(), payload.size());
    if (written != (int)payload.size()) {
        ESP_LOGE(TAG, "uart_write_bytes short write (%d/%d): %s", written, (int)payload.size(),
                 command.c_str());
        return false;
    }
    // 等发送完成再返回，避免连续指令在 FIFO 里被截断
    uart_wait_tx_done(port_, pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "-> %s", command.c_str());
    return true;
}

bool ServoController::SetBaud(int baud_rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        ESP_LOGE(TAG, "SetBaud called before Initialize");
        return false;
    }
    uart_wait_tx_done(port_, pdMS_TO_TICKS(200));
    uart_config_t config = {};
    config.baud_rate = baud_rate;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    esp_err_t err = uart_param_config(port_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config(%d) failed: %s", baud_rate, esp_err_to_name(err));
        return false;
    }
    baud_rate_ = baud_rate;
    ESP_LOGI(TAG, "baud rate -> %d", baud_rate);
    return true;
}

std::string ServoController::ReadRaw(int timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return std::string();
    }
    if (timeout_ms < 0) timeout_ms = 0;
    if (timeout_ms > 2000) timeout_ms = 2000;

    std::string out;
    char buf[129];
    int len = uart_read_bytes(port_, buf, sizeof(buf) - 1, pdMS_TO_TICKS(timeout_ms));
    if (len > 0) {
        buf[len] = '\0';
        out.assign(buf, len);
        ESP_LOGI(TAG, "RX %d byte(s): %s", len, buf);
    } else {
        ESP_LOGI(TAG, "RX nothing in %d ms", timeout_ms);
    }
    return out;
}

bool ServoController::SetAngle(int channel, int angle, int duration_ms) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    int pulse = 500 + (int)(angle * 2000.0f / 180.0f + 0.5f);
    return SetPulse(channel, pulse, duration_ms);
}

bool ServoController::SetPulse(int channel, int pulse_us, int duration_ms) {
    if (channel < 0) channel = 0;
    if (channel > 254) channel = 254;
    if (pulse_us < 500) pulse_us = 500;
    if (pulse_us > 2500) pulse_us = 2500;
    if (duration_ms < 0) duration_ms = 0;
    if (duration_ms > 9999) duration_ms = 9999;

    // 手册要求：Index 3 位、pwm 4 位、time 4 位，整条 15 字符
    char buf[32];
    snprintf(buf, sizeof(buf), "#%03dP%04dT%04d!", channel, pulse_us, duration_ms);
    return SendCommand(buf);
}

bool ServoController::Center(int channel, int duration_ms) {
    return SetPulse(channel, 1500, duration_ms);
}

bool ServoController::SetPulseMany(const int* channels, const int* pulses, int count,
                                   int duration_ms) {
    if (channels == nullptr || pulses == nullptr || count <= 0) {
        return false;
    }
    if (duration_ms < 0) duration_ms = 0;
    if (duration_ms > 9999) duration_ms = 9999;

    // 一包最多 8 路：24 路全发时拆成 3 包，避免控制板解析超长报文时丢尾
    constexpr int kChunk = 8;
    bool all_ok = true;
    for (int start = 0; start < count; start += kChunk) {
        int end = start + kChunk;
        if (end > count) end = count;

        std::string command = "{";
        for (int i = start; i < end; ++i) {
            int ch = channels[i];
            if (ch < 0) ch = 0;
            if (ch > 254) ch = 254;
            int pulse = pulses[i];
            if (pulse < 500) pulse = 500;
            if (pulse > 2500) pulse = 2500;
            char part[32];
            snprintf(part, sizeof(part), "#%03dP%04dT%04d!", ch, pulse, duration_ms);
            command += part;
        }
        command += '}';
        if (!SendCommand(command)) {
            all_ok = false;
        }
        if (end < count) {
            vTaskDelay(pdMS_TO_TICKS(20));  // 包间留间隙
        }
    }
    return all_ok;
}

bool ServoController::CenterMany(const int* channels, int count, int duration_ms) {
    if (channels == nullptr || count <= 0) {
        return false;
    }
    if (duration_ms < 0) duration_ms = 0;
    if (duration_ms > 9999) duration_ms = 9999;

    int pulses[32];
    int n = count > 32 ? 32 : count;
    for (int i = 0; i < n; ++i) {
        pulses[i] = 1500;
    }
    return SetPulseMany(channels, pulses, n, duration_ms);
}

bool ServoController::StopAll() {
    return SendCommand("$DST!");
}

bool ServoController::Stop(int channel) {
    if (channel < 0) channel = 0;
    if (channel > 254) channel = 254;
    char buf[24];
    snprintf(buf, sizeof(buf), "$DST:%03d!", channel);
    return SendCommand(buf);
}

bool ServoController::Reset() {
    return SendCommand("$RST!");
}
