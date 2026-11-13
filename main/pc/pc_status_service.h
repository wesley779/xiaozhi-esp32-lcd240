#ifndef PC_STATUS_SERVICE_H
#define PC_STATUS_SERVICE_H

#include <atomic>
#include <mutex>
#include <string>

/**
 * 局域网内拉取电脑运行状态（CPU / 内存 / 显卡 / 磁盘 / 开机时长）。
 *
 * 数据来自电脑上跑的 pc_status_server.py（同一 WiFi 下的明文 HTTP，端口 8765），
 * 轮询间隔 20 秒，失败 10 秒后重试。拿到数据后推给 display 的第三个待机页。
 */
class PcStatusService {
public:
    static PcStatusService& GetInstance();

    void Start();

    // 语音说"刷新电脑状态"时立刻抓一次
    void RequestRefresh();

    // 供语音工具使用的一整句中文摘要（离线时也会正常返回）
    std::string GetSummary();

    bool IsOnline() const { return online_; }
    bool HasData() const { return has_data_; }

private:
    PcStatusService() = default;
    ~PcStatusService() = default;
    PcStatusService(const PcStatusService&) = delete;
    PcStatusService& operator=(const PcStatusService&) = delete;

    static void TaskEntry(void* arg);
    void Run();
    bool FetchOnce();

    std::mutex mutex_;
    std::string host_name_;
    bool online_ = false;
    bool has_data_ = false;
    float cpu_ = 0.0f;
    float mem_ = 0.0f;
    float gpu_util_ = -1.0f;   // -1 表示没有可读的显卡
    float gpu_temp_ = -1.0f;
    float disk_free_gb_ = 0.0f;
    int uptime_min_ = 0;

    std::atomic<bool> started_{false};
    std::atomic<bool> refresh_requested_{false};
};

#endif  // PC_STATUS_SERVICE_H
