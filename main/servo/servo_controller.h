#ifndef _SERVO_CONTROLLER_H_
#define _SERVO_CONTROLLER_H_

#include <cstdint>
#include <mutex>
#include <string>

#include <driver/uart.h>
#include <driver/gpio.h>

// Yeahbot / 幻尔 LSC-24 类多路舵机控制板的串口客户端。
// 协议（按《24路舵机控制板使用手册》5.2）：
//   单路： #IndexPpwmTtime!   Index 3 位(000-254)、pwm 4 位(0500-2500)、time 4 位(0000-9999)
//           例："#001P1500T1000!" 共 15 个字符，不足补 0
//   多路： {#000P1500T1000!#001P0900T1000!}
//   停止： $DST!   单路停止： $DST:001!
//   软件复位： $RST!
class ServoController {
public:
    ServoController(uart_port_t port, gpio_num_t tx_gpio, gpio_num_t rx_gpio, int baud_rate);
    ~ServoController();

    bool Initialize();

    // 角度 0-180 → 脉宽 500-2500us
    bool SetAngle(int channel, int angle, int duration_ms);
    bool SetPulse(int channel, int pulse_us, int duration_ms);
    // 一次报文驱动多路（每路脉宽可不同）：{#000P1500T0800!#001P0900T0800!}
    // 内部按 8 路一组分包发送，24 路也不会超出控制板单包解析长度
    bool SetPulseMany(const int* channels, const int* pulses, int count, int duration_ms);
    bool Center(int channel, int duration_ms);
    // 一次报文控制多路：{#000P1500T1000!#001P1500T1000!}
    bool CenterMany(const int* channels, int count, int duration_ms);
    // $DST! / $DST:001!
    bool StopAll();
    bool Stop(int channel);
    // $RST!
    bool Reset();

    // 直接发原始指令（调试用，需自带 '!' 结尾）
    bool SendRaw(const std::string& command);

    // 调试用：切换波特率（现场试 9600 / 115200 最省事）
    bool SetBaud(int baud_rate);
    int GetBaud() const { return baud_rate_; }

    // 调试用：监听控制板回来的数据，返回原始内容（可能为空）
    std::string ReadRaw(int timeout_ms);

private:
    bool SendCommand(const std::string& command);

    uart_port_t port_;
    gpio_num_t tx_gpio_;
    gpio_num_t rx_gpio_;
    int baud_rate_;
    bool initialized_ = false;
    std::mutex mutex_;
};

#endif  // _SERVO_CONTROLLER_H_
