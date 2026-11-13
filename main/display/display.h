#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"
#include "text_glyph.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <time.h>
#include <vector>

class LvglFont;
class LvglImage;

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }
    virtual std::shared_ptr<LvglFont> GetTextFont() const { return nullptr; }

private:
    std::string name_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);
    // Returns true when the requested emotion was accepted and set in the rendering
    // system. False means invalid input, a missing resource, an uninitialised display, or
    // an unsupported operation. For asynchronous displays, true does not promise that the
    // physical refresh has already completed. Callers are free to ignore the result.
    virtual bool SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    // Remove a matching message without clearing newer content, when supported.
    virtual void RemoveChatMessage(const char* role, const char* content) {}
    virtual void ClearChatMessages();
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    // Idle home screen: clock / calendar / weather, shown when the device is not in an
    // interaction state. Displays without such a screen simply ignore it.
    virtual void SetIdleHome(bool visible) { (void)visible; }
    // Weather push from the weather service. Values may be NAN when unknown.
    virtual void SetWeather(float temperature, float temp_max, float temp_min, int weather_code) {
        (void)temperature;
        (void)temp_max;
        (void)temp_min;
        (void)weather_code;
    }
    // Feels-like temperature (apparent), pushed separately so the main weather row can
    // render before the extra fields arrive.
    virtual void SetWeatherFeelsLike(float apparent) { (void)apparent; }
    // Air quality: PM2.5 concentration in ug/m3 (NAN/negative when unknown).
    virtual void SetAirQuality(float pm25) { (void)pm25; }
    // 未来几天的预报（从"明天"开始），用于待机第二屏。值为 NAN 表示未知。
    virtual void SetDailyForecast(const float* maxs, const float* mins, const int* codes,
                                  int count) {
        (void)maxs;
        (void)mins;
        (void)codes;
        (void)count;
    }
    // 同 WiFi 下电脑的运行状态，用于待机第三屏。online=false 表示这台电脑连不上；
    // gpu_util/gpu_temp 传负数表示没有可读的显卡。
    virtual void SetPcStatus(bool online, float cpu, float mem, float gpu_util, float gpu_temp,
                             float disk_free_gb, int uptime_min, const char* name) {
        (void)online;
        (void)cpu;
        (void)mem;
        (void)gpu_util;
        (void)gpu_temp;
        (void)disk_free_gb;
        (void)uptime_min;
        (void)name;
    }
    // Called by the status-bar refresh when the device is idle. A display that paints the
    // idle clock itself returns true, which tells the status bar to skip its own clock text.
    virtual bool RenderIdleClock(const struct tm* tm_now, bool force) {
        (void)tm_now;
        (void)force;
        return false;
    }
    virtual void SetPowerSaveMode(bool on);
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) { return false; }
    virtual void ClearTextGlyphs() {}
    virtual void SetEmojiCollection(std::shared_ptr<EmojiCollection>) {}
    virtual void SetupUI() { setup_ui_called_ = true; }
    virtual bool IsMonochrome() const { return false; }
    virtual bool SupportsGuiOperations() const { return false; }
    virtual void SetHideSubtitle(bool hide) { (void)hide; }
    virtual bool InsertAnimDialog(const char* name, uint32_t duration_ms) {
        (void)name;
        (void)duration_ms;
        return false;
    }
    virtual bool MountAssets(const char* partition_label) {
        (void)partition_label;
        return false;
    }
    virtual void UnmountAssets() {}
    virtual bool GetAssetData(const std::string& name, const uint8_t*& data, size_t& size) {
        (void)name;
        (void)data;
        (void)size;
        return false;
    }
    virtual void LoadAssets() {}
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    virtual bool SetTextFont(std::shared_ptr<LvglFont> text_font) {
        (void)text_font;
        return false;
    }
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80) {
        (void)jpeg_data;
        (void)quality;
        return false;
    }

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;  // Track if SetupUI() has been called

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

class DisplayLockGuard {
public:
    explicit DisplayLockGuard(Display* display)
        : display_(display), locked_(display_->Lock(30000)) {
        if (!locked_) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        if (locked_) {
            display_->Unlock();
        }
    }

    DisplayLockGuard(const DisplayLockGuard&) = delete;
    DisplayLockGuard& operator=(const DisplayLockGuard&) = delete;

    explicit operator bool() const { return locked_; }

private:
    Display* display_;
    bool locked_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override { return true; }
    virtual void Unlock() override {}
};

#endif
