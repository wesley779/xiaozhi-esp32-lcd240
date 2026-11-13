#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    /* Idle home screen (clock / calendar / weather) */
    lv_obj_t* idle_panel_ = nullptr;
    lv_obj_t* idle_bg_image_ = nullptr;
    lv_obj_t* idle_bg_overlay_ = nullptr;
    lv_timer_t* idle_bg_timer_ = nullptr;
    int idle_bg_index_ = 0;
    lv_obj_t* idle_clock_label_ = nullptr;
    lv_obj_t* idle_date_label_ = nullptr;
    lv_obj_t* idle_icon_box_ = nullptr;
    lv_obj_t* idle_icon_groups_[7] = {nullptr};
    lv_obj_t* idle_temp_label_ = nullptr;
    lv_obj_t* idle_cond_label_ = nullptr;
    lv_obj_t* idle_range_label_ = nullptr;
    lv_obj_t* idle_weather_row_ = nullptr;
    lv_obj_t* idle_page2_ = nullptr;          // 第二屏：未来三天
    lv_obj_t* idle_forecast_label_[3] = {};
    lv_obj_t* idle_forecast_icon_[3] = {};    // 第二屏每行的小天气图标（26x26 盒子）
    lv_obj_t* idle_page3_ = nullptr;          // 第三屏：电脑运行状态
    lv_obj_t* idle_pc_state_label_ = nullptr;
    lv_obj_t* idle_pc_cpu_label_ = nullptr;
    lv_obj_t* idle_pc_gpu_label_ = nullptr;
    lv_obj_t* idle_pc_disk_label_ = nullptr;
    lv_obj_t* idle_pc_dot_ = nullptr;         // 在线/离线状态灯
    lv_timer_t* idle_page_timer_ = nullptr;
    int idle_page_index_ = 0;
    bool has_forecast_ = false;
    float forecast_max_[3] = {};
    float forecast_min_[3] = {};
    int forecast_code_[3] = {};
    bool has_pc_ = false;
    bool pc_online_ = false;
    float pc_cpu_ = 0.0f;
    float pc_mem_ = 0.0f;
    float pc_gpu_util_ = -1.0f;
    float pc_gpu_temp_ = -1.0f;
    float pc_disk_free_gb_ = 0.0f;
    int pc_uptime_min_ = 0;
    char pc_name_[24] = {0};
    bool idle_home_visible_ = false;
    bool has_weather_ = false;
    float weather_temp_ = 0.0f;
    float weather_max_ = 0.0f;
    float weather_min_ = 0.0f;
    int weather_code_ = -1;
    bool has_feels_like_ = false;
    float weather_feels_ = 0.0f;
    bool has_air_quality_ = false;
    float air_pm25_ = 0.0f;

    void InitializeLcdThemes();
    void SetupIdleHome();  // caller must hold the lvgl lock
    void UpdateIdleClockLocked();  // caller must hold the lvgl lock
    void ApplyWeatherLocked();  // caller must hold the lvgl lock
    void ApplyForecastLocked();  // caller must hold the lvgl lock
    void ApplyPcStatusLocked();  // caller must hold the lvgl lock
    void ShowIdlePage(int index);  // caller must hold the lvgl lock
    static void IdlePageTimerCb(lv_timer_t* timer);
    void AdvanceIdleBackground();  // runs in the lvgl task
    static void IdleBackgroundTimerCb(lv_timer_t* timer);
    static void IdleBackgroundFadeOutDone(lv_anim_t* anim);
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual bool SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void RemoveChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    virtual void SetHideSubtitle(bool hide) override;

    // Idle home screen: clock / calendar / weather
    virtual void SetIdleHome(bool visible) override;
    virtual void SetWeather(float temperature, float temp_max, float temp_min,
                            int weather_code) override;
    virtual void SetWeatherFeelsLike(float apparent) override;
    virtual void SetAirQuality(float pm25) override;
    virtual void SetDailyForecast(const float* maxs, const float* mins, const int* codes,
                                  int count) override;
    virtual void SetPcStatus(bool online, float cpu, float mem, float gpu_util, float gpu_temp,
                             float disk_free_gb, int uptime_min, const char* name) override;
    virtual bool RenderIdleClock(const struct tm* tm_now, bool force) override;
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
