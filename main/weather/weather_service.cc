#include "weather_service.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "application.h"
#include "board.h"
#include "display.h"
#include "settings.h"

#define TAG "WeatherService"

// Tianjin (Heping district). Edit latitude/longitude to move the forecast location.
#define WEATHER_LAT "39.13"
#define WEATHER_LON "117.20"

#define FETCH_OK_INTERVAL_MS (10 * 60 * 1000)  // 10 minutes after a successful fetch
#define FETCH_RETRY_INTERVAL_MS (60 * 1000)    // retry sooner after a failure
#define FETCH_FIRST_DELAY_MS (1000)            // then poll until the network is up
#define NETWORK_WAIT_MAX_MS (60 * 1000)        // give up waiting for wifi after 60s

static const char* kWeatherUrl =
    "https://api.open-meteo.com/v1/forecast?latitude=" WEATHER_LAT "&longitude=" WEATHER_LON
    "&current=temperature_2m,apparent_temperature,weather_code"
    "&daily=temperature_2m_max,temperature_2m_min,weather_code"
    "&timezone=Asia%2FShanghai&forecast_days=4";

// 空气质量走独立接口（免费、无需 key），只在天气抓取成功后顺带更新
static const char* kAirQualityUrl =
    "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=" WEATHER_LAT
    "&longitude=" WEATHER_LON "&current=pm2_5&timezone=Asia%2FShanghai";

static esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr) {
        auto* body = static_cast<std::string*>(evt->user_data);
        body->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

WeatherService& WeatherService::GetInstance() {
    static WeatherService instance;
    return instance;
}

void WeatherService::Start() {
    if (started_.exchange(true)) {
        return;
    }
    xTaskCreate(TaskEntry, "weather", 4096 * 2, this, 2, nullptr);
}

void WeatherService::TaskEntry(void* arg) {
    static_cast<WeatherService*>(arg)->Run();
    vTaskDelete(nullptr);
}

void WeatherService::RequestRefresh() {
    refresh_requested_.store(true);
    ESP_LOGI(TAG, "manual refresh requested");
}

static bool NetworkReady() {
    auto state = Application::GetInstance().GetDeviceState();
    return state != kDeviceStateStarting && state != kDeviceStateWifiConfiguring;
}

// 开机时 MQTT/TLS 正在抢占带宽，此时抓天气很容易 CONNECT 超时；
// 等设备进入 idle（服务器连好了）再抓，既快又稳。90 秒兜底：服务器连不上也照抓。
static bool QuietNetworkReady() {
    if (!NetworkReady()) {
        return false;
    }
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        return true;
    }
    return (esp_timer_get_time() / 1000) > 90 * 1000;
}

void WeatherService::Run() {
    // 等显示对象建好，先把 NVS 里上一次的天气画出来：开机不再干等网络显示 "--°"
    for (int i = 0; i < 40 && Board::GetInstance().GetDisplay() == nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    RestoreCache();

    vTaskDelay(pdMS_TO_TICKS(FETCH_FIRST_DELAY_MS));

    // 不再固定干等 20 秒：wifi 一连上就抓，最多等 60 秒后按原逻辑继续
    const int kPollMs = 500;
    for (int waited = 0; waited < NETWORK_WAIT_MAX_MS; waited += kPollMs) {
        if (QuietNetworkReady() || refresh_requested_.load()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }

    while (true) {
        uint32_t wait_ms = FETCH_RETRY_INTERVAL_MS;
        bool forced = refresh_requested_.exchange(false);
        if (QuietNetworkReady() || forced) {
            int64_t t0 = esp_timer_get_time();
            bool ok = FetchOnce();
            int cost_ms = (int)((esp_timer_get_time() - t0) / 1000);
            ESP_LOGI(TAG, "fetch %s in %d ms", ok ? "ok" : "failed", cost_ms);
            if (ok) {
                int64_t t1 = esp_timer_get_time();
                bool air_ok = FetchAirQuality();
                ESP_LOGI(TAG, "air fetch %s in %d ms", air_ok ? "ok" : "failed",
                         (int)((esp_timer_get_time() - t1) / 1000));
            }
            wait_ms = ok ? FETCH_OK_INTERVAL_MS : FETCH_RETRY_INTERVAL_MS;
        } else {
            wait_ms = FETCH_RETRY_INTERVAL_MS;
        }

        // 等待期间每 500 ms 检查一次手动刷新，语音说"刷新天气"能立刻生效
        int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
        while (esp_timer_get_time() < deadline) {
            if (refresh_requested_.exchange(false)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static float JsonNumber(cJSON* obj, const char* key, float fallback) {
    if (obj == nullptr) {
        return fallback;
    }
    cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? static_cast<float>(item->valuedouble) : fallback;
}

static float JsonArrayFirst(cJSON* obj, const char* key, float fallback) {
    if (obj == nullptr) {
        return fallback;
    }
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsArray(item)) {
        cJSON* first = cJSON_GetArrayItem(item, 0);
        if (cJSON_IsNumber(first)) {
            return static_cast<float>(first->valuedouble);
        }
    }
    return fallback;
}

static float JsonArrayAt(cJSON* obj, const char* key, int index, float fallback) {
    if (obj == nullptr) {
        return fallback;
    }
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsArray(item)) {
        cJSON* value = cJSON_GetArrayItem(item, index);
        if (cJSON_IsNumber(value)) {
            return static_cast<float>(value->valuedouble);
        }
    }
    return fallback;
}

// NVS 缓存编码：32767 表示 NAN，其余按 0.1 精度存整数
#define CACHE_NAN 32767
static int CacheEncode(float v) {
    return std::isfinite(v) ? (int)lroundf(v * 10.0f) : CACHE_NAN;
}
static float CacheDecode(int v) {
    return v == CACHE_NAN ? NAN : (float)v / 10.0f;
}

// NVS 缓存：把上一次抓到的数值存下来，开机直接点亮，不用等 8 秒的 TLS 握手
void WeatherService::RestoreCache() {
    Settings settings("weather");
    int code_raw = settings.GetInt("valid", 0);
    if (code_raw != 1) {
        ESP_LOGI(TAG, "no cached weather");
        return;
    }

    last_temp_ = CacheDecode(settings.GetInt("temp", 0));
    last_max_ = CacheDecode(settings.GetInt("tmax", CACHE_NAN));
    last_min_ = CacheDecode(settings.GetInt("tmin", CACHE_NAN));
    last_code_ = settings.GetInt("code", -1);
    int feels = settings.GetInt("feels", CACHE_NAN);
    int pm25 = settings.GetInt("pm25", -1);
    if (!std::isfinite(last_temp_)) {
        return;
    }
    has_weather_data_ = true;

    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    display->SetWeather(last_temp_, last_max_, last_min_, last_code_);
    if (feels != CACHE_NAN) {
        has_feels_ = true;
        last_feels_ = CacheDecode(feels);
        display->SetWeatherFeelsLike(last_feels_);
    }
    if (pm25 >= 0) {
        has_pm25_ = true;
        last_pm25_ = (float)pm25;
        display->SetAirQuality(last_pm25_);
    }

    static const char* kMaxKeys[3] = {"d1max", "d2max", "d3max"};
    static const char* kMinKeys[3] = {"d1min", "d2min", "d3min"};
    static const char* kCodeKeys[3] = {"d1code", "d2code", "d3code"};
    bool any_forecast = false;
    for (int i = 0; i < 3; ++i) {
        fc_max_[i] = CacheDecode(settings.GetInt(kMaxKeys[i], CACHE_NAN));
        fc_min_[i] = CacheDecode(settings.GetInt(kMinKeys[i], CACHE_NAN));
        fc_code_[i] = settings.GetInt(kCodeKeys[i], -1);
        if (std::isfinite(fc_max_[i])) {
            any_forecast = true;
        }
    }
    if (any_forecast) {
        display->SetDailyForecast(fc_max_, fc_min_, fc_code_, 3);
    }

    ESP_LOGI(TAG, "restored cached weather: %.1fC, feels %.1f, pm25 %.0f", last_temp_,
             has_feels_ ? last_feels_ : 0.0f, has_pm25_ ? last_pm25_ : 0.0f);
}

void WeatherService::SaveCache() {
    if (!has_weather_data_) {
        return;
    }
    Settings settings("weather", true);
    settings.SetInt("valid", 1);
    settings.SetInt("temp", CacheEncode(last_temp_));
    settings.SetInt("tmax", CacheEncode(last_max_));
    settings.SetInt("tmin", CacheEncode(last_min_));
    settings.SetInt("code", last_code_);
    settings.SetInt("feels", has_feels_ ? CacheEncode(last_feels_) : CACHE_NAN);
    settings.SetInt("pm25", has_pm25_ ? (int)lroundf(last_pm25_) : -1);
    static const char* kMaxKeys[3] = {"d1max", "d2max", "d3max"};
    static const char* kMinKeys[3] = {"d1min", "d2min", "d3min"};
    static const char* kCodeKeys[3] = {"d1code", "d2code", "d3code"};
    for (int i = 0; i < 3; ++i) {
        settings.SetInt(kMaxKeys[i], CacheEncode(fc_max_[i]));
        settings.SetInt(kMinKeys[i], CacheEncode(fc_min_[i]));
        settings.SetInt(kCodeKeys[i], fc_code_[i]);
    }
}

bool WeatherService::FetchOnce() {
    std::string body;
    esp_http_client_config_t config = {};
    config.url = kWeatherUrl;
    config.event_handler = HttpEventHandler;
    config.user_data = &body;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to create http client");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Weather request failed: err=%s status=%d", esp_err_to_name(err), status);
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Weather response is not valid JSON (%u bytes)", (unsigned)body.size());
        return false;
    }

    cJSON* current = cJSON_GetObjectItem(root, "current");
    cJSON* daily = cJSON_GetObjectItem(root, "daily");

    float temp = JsonNumber(current, "temperature_2m", NAN);
    float feels = JsonNumber(current, "apparent_temperature", NAN);
    int code = static_cast<int>(JsonNumber(current, "weather_code", -1));
    float tmax = JsonArrayFirst(daily, "temperature_2m_max", NAN);
    float tmin = JsonArrayFirst(daily, "temperature_2m_min", NAN);
    // 未来三天（数组下标 1..3，0 是今天）
    for (int i = 0; i < 3; ++i) {
        fc_max_[i] = JsonArrayAt(daily, "temperature_2m_max", i + 1, NAN);
        fc_min_[i] = JsonArrayAt(daily, "temperature_2m_min", i + 1, NAN);
        fc_code_[i] = (int)JsonArrayAt(daily, "weather_code", i + 1, -1);
    }
    cJSON_Delete(root);

    if (!std::isfinite(temp)) {
        ESP_LOGW(TAG, "Weather response has no temperature");
        return false;
    }

    ESP_LOGI(TAG, "Weather: %.1fC (feels %.1f, H %.1f / L %.1f) code=%d", temp, feels, tmax, tmin,
             code);

    last_temp_ = temp;
    last_max_ = tmax;
    last_min_ = tmin;
    last_code_ = code;
    has_weather_data_ = true;
    if (std::isfinite(feels)) {
        last_feels_ = feels;
        has_feels_ = true;
    }
    SaveCache();

    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetWeather(temp, tmax, tmin, code);
        display->SetWeatherFeelsLike(feels);
        display->SetDailyForecast(fc_max_, fc_min_, fc_code_, 3);
    }
    return true;
}

bool WeatherService::FetchAirQuality() {
    std::string body;
    esp_http_client_config_t config = {};
    config.url = kAirQualityUrl;
    config.event_handler = HttpEventHandler;
    config.user_data = &body;
    config.timeout_ms = 8000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to create air quality http client");
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Air quality request failed: err=%s status=%d", esp_err_to_name(err), status);
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Air quality response is not valid JSON");
        return false;
    }
    cJSON* current = cJSON_GetObjectItem(root, "current");
    float pm25 = JsonNumber(current, "pm2_5", NAN);
    cJSON_Delete(root);

    if (!std::isfinite(pm25)) {
        ESP_LOGW(TAG, "Air quality response has no pm2_5");
        return false;
    }

    ESP_LOGI(TAG, "Air: PM2.5 %.1f ug/m3", pm25);
    last_pm25_ = pm25;
    has_pm25_ = true;
    SaveCache();

    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetAirQuality(pm25);
    }
    return true;
}
