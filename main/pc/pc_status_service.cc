#include "pc_status_service.h"

#include <cJSON.h>
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

#define TAG "PcStatus"

// 电脑上 pc_status_server.py 的地址：把 192.168.1.100 换成你自己电脑的内网 IP
//（路由器里给电脑绑定静态 DHCP 地址更稳），端口要和 pc_status_server.py 启动时的一致。
// 也可以在编译时用 -DPC_STATUS_URL=\"http://x.x.x.x:8765/status\" 覆盖。
#ifndef PC_STATUS_URL
#define PC_STATUS_URL "http://192.168.1.100:8765/status"
#endif

#define FETCH_OK_INTERVAL_MS (20 * 1000)   // 内网很快，20 秒一轮
#define FETCH_RETRY_INTERVAL_MS (10 * 1000)
#define FIRST_DELAY_MS (2000)
#define NETWORK_WAIT_MAX_MS (60 * 1000)
#define HTTP_TIMEOUT_MS (5000)

static esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr) {
        auto* body = static_cast<std::string*>(evt->user_data);
        body->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

static bool NetworkReady() {
    auto state = Application::GetInstance().GetDeviceState();
    return state != kDeviceStateStarting && state != kDeviceStateWifiConfiguring;
}

static float JsonNumber(cJSON* obj, const char* key, float fallback) {
    if (obj == nullptr) {
        return fallback;
    }
    cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? static_cast<float>(item->valuedouble) : fallback;
}

PcStatusService& PcStatusService::GetInstance() {
    static PcStatusService instance;
    return instance;
}

void PcStatusService::Start() {
    if (started_.exchange(true)) {
        return;
    }
    xTaskCreate(TaskEntry, "pc_status", 4096 * 2, this, 2, nullptr);
}

void PcStatusService::TaskEntry(void* arg) {
    static_cast<PcStatusService*>(arg)->Run();
    vTaskDelete(nullptr);
}

void PcStatusService::RequestRefresh() {
    refresh_requested_.store(true);
    ESP_LOGI(TAG, "manual refresh requested");
}

void PcStatusService::Run() {
    vTaskDelay(pdMS_TO_TICKS(FIRST_DELAY_MS));

    const int kPollMs = 500;
    for (int waited = 0; waited < NETWORK_WAIT_MAX_MS; waited += kPollMs) {
        if (NetworkReady()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }

    while (true) {
        uint32_t wait_ms = FETCH_RETRY_INTERVAL_MS;
        bool forced = refresh_requested_.exchange(false);
        if (NetworkReady() || forced) {
            int64_t t0 = esp_timer_get_time();
            bool ok = FetchOnce();
            ESP_LOGI(TAG, "fetch %s in %d ms", ok ? "ok" : "failed",
                     (int)((esp_timer_get_time() - t0) / 1000));
            wait_ms = ok ? FETCH_OK_INTERVAL_MS : FETCH_RETRY_INTERVAL_MS;
        }

        int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
        while (esp_timer_get_time() < deadline) {
            if (refresh_requested_.exchange(false)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

bool PcStatusService::FetchOnce() {
    std::string body;
    esp_http_client_config_t config = {};
    config.url = PC_STATUS_URL;
    config.event_handler = HttpEventHandler;
    config.user_data = &body;
    config.timeout_ms = HTTP_TIMEOUT_MS;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to create http client");
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    auto display = Board::GetInstance().GetDisplay();
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "pc status request failed: err=%s status=%d", esp_err_to_name(err), status);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            online_ = false;
        }
        if (display != nullptr) {
            display->SetPcStatus(false, 0, 0, -1, -1, 0, 0, nullptr);
        }
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "pc status response is not valid JSON (%u bytes)", (unsigned)body.size());
        return false;
    }

    // 名字必须在这里拷出来：下面 cJSON_Delete 会把 root 释放掉，
    // 之后再解引用 valuestring 就是悬空指针（日志里会打出乱码）。
    char name_buf[32] = {0};
    cJSON* name_item = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(name_item) && name_item->valuestring != nullptr) {
        snprintf(name_buf, sizeof(name_buf), "%s", name_item->valuestring);
    }
    float cpu = JsonNumber(root, "cpu", -1);
    float mem = JsonNumber(root, "mem", -1);
    float disk_free = JsonNumber(root, "disk_free_gb", -1);
    int uptime = (int)JsonNumber(root, "uptime_min", 0);
    cJSON* gpu = cJSON_GetObjectItem(root, "gpu");
    float gpu_util = -1.0f;
    float gpu_temp = -1.0f;
    if (cJSON_IsObject(gpu)) {
        gpu_util = JsonNumber(gpu, "util", -1);
        gpu_temp = JsonNumber(gpu, "temp", -1);
    }
    cJSON_Delete(root);

    if (cpu < 0 || mem < 0) {
        ESP_LOGW(TAG, "pc status response misses cpu/mem");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        host_name_ = name_buf;
        cpu_ = cpu;
        mem_ = mem;
        gpu_util_ = gpu_util;
        gpu_temp_ = gpu_temp;
        disk_free_gb_ = disk_free;
        uptime_min_ = uptime;
        online_ = true;
        has_data_ = true;
    }

    ESP_LOGI(TAG, "pc %s cpu=%.0f%% mem=%.0f%% gpu=%.0f%% disk_free=%.0fG up=%dmin", name_buf,
             cpu, mem, gpu_util, disk_free, uptime);

    if (display != nullptr) {
        display->SetPcStatus(true, cpu, mem, gpu_util, gpu_temp, disk_free, uptime, name_buf);
    }
    return true;
}

std::string PcStatusService::GetSummary() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_data_) {
        return "还没有拿到电脑状态，可能电脑上的 pc_status_server.py 没有启动，或者不在同一个 WiFi。";
    }
    if (!online_) {
        return "电脑现在连不上，可能关机了、休眠了，或者 pc_status_server.py 没有在运行。";
    }

    char buf[256];
    bool busy = cpu_ >= 40 || gpu_util_ >= 50;
    char gpu_part[48] = "";
    if (gpu_util_ >= 0) {
        if (gpu_temp_ >= 0) {
            snprintf(gpu_part, sizeof(gpu_part), "，显卡 %.0f%%，%.0f度", gpu_util_, gpu_temp_);
        } else {
            snprintf(gpu_part, sizeof(gpu_part), "，显卡 %.0f%%", gpu_util_);
        }
    }
    snprintf(buf, sizeof(buf),
             "电脑（%s）%s，CPU %.0f%%，内存 %.0f%%，C盘剩 %.0fG%s，已经开机 %d 小时 %d 分钟。",
             host_name_.c_str(), busy ? "正在忙" : "基本空闲", cpu_, mem_, disk_free_gb_, gpu_part,
             uptime_min_ / 60, uptime_min_ % 60);
    return std::string(buf);
}
