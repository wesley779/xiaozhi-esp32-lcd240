#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

#include <atomic>

/**
 * Fetches current weather from Open-Meteo (no API key required) and pushes it to
 * the display, which paints it on the idle home screen.
 *
 * Runs in its own task so the HTTP round trip never blocks lvgl or the main loop.
 */
class WeatherService {
public:
    static WeatherService& GetInstance();

    void Start();

    // 请求立刻抓一次（不必等下一个自动周期），语音"刷新天气"走这里
    void RequestRefresh();

private:
    WeatherService() = default;
    ~WeatherService() = default;
    WeatherService(const WeatherService&) = delete;
    WeatherService& operator=(const WeatherService&) = delete;

    static void TaskEntry(void* arg);
    void Run();
    bool FetchOnce();
    bool FetchAirQuality();
    void RestoreCache();  // 开机先用 NVS 里上一次的数据点亮界面
    void SaveCache();

    float last_temp_ = 0.0f;
    float last_max_ = 0.0f;
    float last_min_ = 0.0f;
    float last_feels_ = 0.0f;
    float last_pm25_ = 0.0f;
    int last_code_ = -1;
    bool has_weather_data_ = false;
    bool has_feels_ = false;
    bool has_pm25_ = false;
    float fc_max_[3] = {};
    float fc_min_[3] = {};
    int fc_code_[3] = {};

    std::atomic<bool> started_{false};
    std::atomic<bool> refresh_requested_{false};
};

#endif  // WEATHER_SERVICE_H
