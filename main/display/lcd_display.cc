#include "lcd_display.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "idle_bg_images.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);
LV_FONT_DECLARE(font_noto_sans_basic_30_4);
// 内置 LVGL 中文点阵字库（含"体感/明天/高/月/日/周"等常用字），
// 资源包里的 Noto 子集缺这些字形，待机界面的中文标签单独用它。
LV_FONT_DECLARE(idle_font_16);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
#if CONFIG_LV_USE_BUILTIN_MALLOC
        // Base TLSF pool is CONFIG_LV_MEM_SIZE_KILOBYTES in PSRAM; grow it so
        // the 2MB decoded-image cache does not exhaust a 2MB-PSRAM-safe pool.
        // TLSF rejects any single pool larger than LV_MEM_SIZE when
        // LV_MEM_POOL_EXPAND_SIZE is 0 (block_size_max = 1 << ceil(log2(LV_MEM_SIZE))).
        const size_t extra_total = 2560 * 1024;
        const size_t chunk_size = LV_MEM_SIZE;
        size_t added = 0;
        while (added < extra_total) {
            size_t this_chunk = extra_total - added;
            if (this_chunk > chunk_size) {
                this_chunk = chunk_size;
            }
            void* pool = heap_caps_malloc(this_chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pool == nullptr || lv_mem_add_pool(pool, this_chunk) == nullptr) {
                if (pool != nullptr) {
                    heap_caps_free(pool);
                }
                ESP_LOGE(TAG, "Failed to add %uKB LVGL PSRAM pool (added %uKB)",
                         (unsigned)(this_chunk / 1024), (unsigned)(added / 1024));
                break;
            }
            added += this_chunk;
        }
        if (added == extra_total) {
            ESP_LOGI(TAG, "Added %uKB LVGL pool in PSRAM", (unsigned)(added / 1024));
        }
#endif
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    // SPI LCD still needs an internal DMA bounce buffer for PSRAM sources.
    // A full-frame transfer (~150KB) cannot allocate that bounce buffer, so
    // keep a small DMA strip in internal SRAM and partial refresh.
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);

    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (idle_bg_timer_ != nullptr) {
        lv_timer_delete(idle_bg_timer_);
        idle_bg_timer_ = nullptr;
    }
    if (idle_panel_ != nullptr) {
        lv_obj_del(idle_panel_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(),
                              0);  // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);  // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0,
                 text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called "
                     "but container not created)",
                     role, content);
        }
        return;
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // Collapse system messages (if it's a system message, check if the last message is also a
    // system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) &&
                lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr &&
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if (strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);

    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);

    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;   // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100;  // 50% of screen height

    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld",
                 img_width, img_height, max_width, max_height);
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256)
        zoom = 256;

    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release();  // Release ownership of smart pointer
    lv_obj_add_event_cb(
        preview_image,
        [](lv_event_t* e) {
            LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
            if (img != nullptr) {
                delete img;  // Properly release memory by deleting LvglImage object
            }
        },
        LV_EVENT_DELETE, (void*)raw_image);

    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);

    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);

    // Center the image within the bubble
    lv_obj_center(preview_image);

    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr || role == nullptr || content == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(content_); ++i) {
        auto container = lv_obj_get_child(content_, i);
        auto bubble = container;
        auto bubble_role = static_cast<const char*>(lv_obj_get_user_data(bubble));
        if (bubble_role == nullptr && lv_obj_get_child_cnt(bubble) > 0) {
            bubble = lv_obj_get_child(bubble, 0);
            bubble_role = static_cast<const char*>(lv_obj_get_user_data(bubble));
        }
        if (bubble_role == nullptr || strcmp(bubble_role, role) != 0 ||
            lv_obj_get_child_cnt(bubble) == 0) {
            continue;
        }
        auto label = lv_obj_get_child(bubble, 0);
        if (strcmp(lv_label_get_text(label), content) != 0) {
            continue;
        }
        if (chat_message_label_ == label) {
            chat_message_label_ = nullptr;
        }
        lv_obj_del(container);
        if (lv_obj_get_child_cnt(content_) == 0 && emoji_label_ != nullptr) {
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);

    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Bottom layer: emoji_box_ - centered display */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Idle home screen (clock / calendar / weather), sits under the status bars */
    SetupIdleHome();

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    /* Bottom bar - auto height, grows upward with wrapped text */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, multiline wrapped display */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#else
    /* Top layer: Bottom bar - fixed height at bottom */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, single-line horizontal scroll */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    // Start scrolling after a delay (short text won't scroll)
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#endif

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() "
                     "was called but label not created)",
                     role, content);
        }
        return;
    }
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, content);
    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    // Re-align bottom_bar_ after text change so it stays anchored to the bottom
    // as its height adapts to the wrapped content.
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
#endif
}

void LcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || content == nullptr ||
        strcmp(lv_label_get_text(chat_message_label_), content) != 0) {
        return;
    }
    // The classic layout has one text slot and does not distinguish message roles.
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, "");
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

bool LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!",
                 emotion);
    }
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but "
                     "emoji image not created)",
                     emotion);
        }
        return false;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = noto_emoji_get_utf8(emotion);
        const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (utf8 == nullptr || emoji_label_ == nullptr) {
            // No image and no glyph for this name: nothing was shown for the request.
            return false;
        }
        DisplayLockGuard lock(this);
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        lv_obj_set_style_text_font(emoji_label_, emotion_font, 0);
        lv_label_set_text(emoji_label_, utf8);
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        // The glyph is this emotion's own, from noto_emoji or material_symbols, not a
        // stand-in - so the request was honoured.
        return true;
    }

    DisplayLockGuard lock(this);
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    bool shown = false;
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());

        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback(
                [this]() { lv_image_set_src(emoji_image_, gif_controller_->image_dsc()); });

            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();

            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            shown = true;
        } else {
            // The collection had an image for this emotion but it would not decode, so
            // nothing is on screen for the request.
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        shown = true;
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    // The WeChat style deliberately hides a neutral emotion. That is a choice about
    // presentation, not a failure to honour the request, so `shown` stands as it was.
    return shown;
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }

    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }

    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr)
            continue;

        lv_obj_t* bubble = nullptr;

        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }

        if (bubble == nullptr)
            continue;

        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }

            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);

            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }

    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }

    // Update bottom bar background color with 50% opacity
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
#endif

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // Keep the idle home screen in step with the theme colors
    if (idle_panel_ != nullptr) {
        lv_obj_set_style_bg_color(idle_panel_, lvgl_theme->background_color(), 0);
        lv_obj_set_style_text_color(idle_clock_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_text_color(idle_date_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_text_color(idle_temp_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_text_color(idle_cond_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_text_color(idle_range_label_, lvgl_theme->text_color(), 0);
    }

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;

    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text =
                (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Idle home screen: big clock + date + weather.
 * Text uses only digits / ASCII / degree sign because the built-in fonts carry
 * a fixed CJK subset; weather glyphs are drawn from shapes instead of a font.
 * ------------------------------------------------------------------------- */

static const char* WeatherConditionText(int code) {
    if (code < 0) {
        return "--";
    }
    if (code == 0) {
        return "晴";
    }
    if (code <= 2) {
        return "多云";
    }
    if (code == 3) {
        return "阴";
    }
    if (code <= 48) {
        return "雾";
    }
    if (code <= 57) {
        return "小雨";
    }
    if (code <= 67) {
        return "雨";
    }
    if (code <= 77) {
        return "雪";
    }
    if (code <= 82) {
        return "阵雨";
    }
    if (code <= 86) {
        return "阵雪";
    }
    return "雷阵雨";
}

// PM2.5 浓度 → 中国空气质量等级（按 24 小时均值分级阈值，近似口径）
static const char* AirLevelText(float pm25) {
    if (pm25 <= 35) return "优";
    if (pm25 <= 75) return "良";
    if (pm25 <= 115) return "轻度";
    if (pm25 <= 150) return "中度";
    if (pm25 <= 250) return "重度";
    return "严重";
}

// 自生成小字库（idle_font_16.c）已含 多云/阴/雾/雷阵雨/阵雨 等字，
// 所以这里可以全部用中文短标签，不再退化到英文单词。
static const char* SafeConditionText(int code) {
    if (code < 0) {
        return "?";
    }
    if (code == 0 || code == 1) {
        return "晴";
    }
    if (code == 2) {
        return "多云";
    }
    if (code == 3) {
        return "阴";
    }
    if (code == 45 || code == 48) {
        return "雾";
    }
    if (code >= 51 && code <= 57) {
        return "小雨";
    }
    if (code >= 61 && code <= 67) {
        return "雨";
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return "雪";
    }
    if (code >= 80 && code <= 82) {
        return "阵雨";
    }
    if (code >= 95) {
        return "雷阵雨";
    }
    return "?";
}

// Icon group index: 0 clear, 1 partly, 2 cloudy, 3 rain, 4 snow, 5 fog, 6 storm
static int WeatherIconIndex(int code) {
    if (code < 0) {
        return -1;
    }
    if (code == 0) {
        return 0;
    }
    if (code <= 2) {
        return 1;
    }
    if (code == 3) {
        return 2;
    }
    if (code <= 48) {
        return 5;
    }
    if (code <= 57 || (code >= 61 && code <= 67) || (code >= 80 && code <= 82)) {
        return 3;
    }
    if (code >= 71 && code <= 77) {
        return 4;
    }
    if (code >= 85 && code <= 86) {
        return 4;
    }
    return 6;
}

/* -------------------------------------------------------------------------
 * Colored, animated idle weather icons.
 * Everything is drawn from plain LVGL objects (no icon font, no glyphs), so it
 * works with the built-in Latin-only font set. Each icon lives in a
 * IDLE_ICON_W x IDLE_ICON_H box and all children stay inside those bounds.
 * ---------------------------------------------------------------------- */
#define IDLE_ICON_W 46
#define IDLE_ICON_H 42

/* Photo slideshow on the idle screen: interval + cross fade timings. */
#define IDLE_BG_INTERVAL_MS 15000
// 待机两屏轮播间隔：第一屏时钟+当前天气，第二屏未来三天
#define IDLE_PAGE_INTERVAL_MS 10000
#define IDLE_BG_FADE_OUT_MS 400
#define IDLE_BG_FADE_IN_MS 700

static lv_color_t IdleSunCore()    { return lv_color_hex(0xFFB300); }
static lv_color_t IdleSunGlow()    { return lv_color_hex(0xFFD54F); }
static lv_color_t IdleCloudLight() { return lv_color_hex(0xE4EBF2); }
static lv_color_t IdleCloudMid()   { return lv_color_hex(0xB9C6D3); }
static lv_color_t IdleCloudDark()  { return lv_color_hex(0x8FA0B0); }
static lv_color_t IdleRainColor()  { return lv_color_hex(0x4FC3F7); }
static lv_color_t IdleSnowColor()  { return lv_color_hex(0xE1F5FE); }
static lv_color_t IdleFogColor()   { return lv_color_hex(0xAEBBC4); }
static lv_color_t IdleBoltColor()  { return lv_color_hex(0xFFD740); }

/* Animation exec callbacks: plain int setters so lv_anim can drive them. */
static void IdleSizeCb(void* var, int32_t v) {
    lv_obj_set_size(static_cast<lv_obj_t*>(var), v, v);
}
static void IdleOpaCb(void* var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(v), 0);
}

/* 天气图标改为静态：保留彩色绘制，但不再跑动画。
 * 关掉后省掉每帧的 lv_anim 对象和动画定时器开销（RAM + CPU），外观仍是彩色图标。
 * 想恢复动态效果，把下面改成 true。 */
static constexpr bool kIdleWeatherIconAnimated = false;

/* Infinite animation helper. yoyo=true makes it bounce back and forth. */
static void StartIdleAnim(lv_obj_t* obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                          uint32_t duration, uint32_t delay, bool yoyo) {
    if (!kIdleWeatherIconAnimated) {
        return;  // 静态模式：图标停在初始位置/尺寸/不透明度
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, duration);
    if (delay != 0) {
        lv_anim_set_delay(&a, delay);
    }
    if (yoyo) {
        lv_anim_set_reverse_duration(&a, duration);
    }
    lv_anim_set_path_cb(&a, yoyo ? lv_anim_path_ease_in_out : lv_anim_path_linear);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static lv_obj_t* IdleDot(lv_obj_t* parent, lv_color_t color, int size, lv_opa_t opa) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, size, size);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    return obj;
}

static lv_obj_t* IdleDotAt(lv_obj_t* parent, lv_color_t color, int x, int y, int size,
                           lv_opa_t opa) {
    lv_obj_t* obj = IdleDot(parent, color, size, opa);
    lv_obj_set_pos(obj, x, y);
    return obj;
}

static lv_obj_t* IdleBar(lv_obj_t* parent, lv_color_t color, int x, int y, int w, int h,
                         int radius) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    return obj;
}

/* Cloud: a wrapper holding bumps + a base bar, so the whole cloud can drift. */
static lv_obj_t* IdleCloud(lv_obj_t* parent, lv_color_t color, bool large) {
    int w = large ? 34 : 24;
    int h = large ? 25 : 18;
    lv_obj_t* wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, w, h);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_set_style_shadow_width(wrap, 0, 0);
    if (large) {
        IdleDotAt(wrap, color, 1, 8, 14, LV_OPA_COVER);
        IdleDotAt(wrap, color, 8, 1, 20, LV_OPA_COVER);
        IdleDotAt(wrap, color, 20, 8, 14, LV_OPA_COVER);
        IdleBar(wrap, color, 1, 14, 32, 10, 5);
    } else {
        IdleDotAt(wrap, color, 1, 6, 10, LV_OPA_COVER);
        IdleDotAt(wrap, color, 6, 1, 15, LV_OPA_COVER);
        IdleDotAt(wrap, color, 14, 6, 10, LV_OPA_COVER);
        IdleBar(wrap, color, 1, 11, 22, 7, 3);
    }
    return wrap;
}

/* Drift a cloud left/right by a few pixels around its current x. */
static void IdleCloudDrift(lv_obj_t* cloud, int amount, uint32_t duration) {
    int32_t x = lv_obj_get_x(cloud);
    StartIdleAnim(cloud, reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_x), x, x + amount,
                  duration, 0, true);
}

/* 0: clear - pulsing sun with fading rays */
static void BuildIdleIconClear(lv_obj_t* g) {
    lv_obj_t* glow = IdleDot(g, IdleSunGlow(), 28, LV_OPA_30);
    lv_obj_align(glow, LV_ALIGN_CENTER, 0, 0);
    StartIdleAnim(glow, IdleSizeCb, 26, 32, 2600, 0, true);

    static const int kRays[8][2] = {{0, -16}, {0, 16}, {-16, 0}, {16, 0},
                                    {-11, -11}, {11, -11}, {-11, 11}, {11, 11}};
    for (int i = 0; i < 8; ++i) {
        lv_obj_t* ray = IdleDot(g, IdleSunCore(), 4, LV_OPA_COVER);
        lv_obj_align(ray, LV_ALIGN_CENTER, kRays[i][0], kRays[i][1]);
        StartIdleAnim(ray, IdleOpaCb, 255, 130, 1300, static_cast<uint32_t>(i) * 90, true);
    }

    lv_obj_t* core = IdleDot(g, IdleSunCore(), 18, LV_OPA_COVER);
    lv_obj_align(core, LV_ALIGN_CENTER, 0, 0);
    StartIdleAnim(core, IdleSizeCb, 17, 21, 2600, 0, true);
}

/* 1: partly cloudy - sun behind, cloud in front drifting */
static void BuildIdleIconPartly(lv_obj_t* g) {
    lv_obj_t* glow = IdleDot(g, IdleSunGlow(), 24, LV_OPA_30);
    lv_obj_align(glow, LV_ALIGN_CENTER, -8, -8);
    StartIdleAnim(glow, IdleSizeCb, 22, 26, 2600, 0, true);

    lv_obj_t* sun = IdleDot(g, IdleSunCore(), 15, LV_OPA_COVER);
    lv_obj_align(sun, LV_ALIGN_CENTER, -8, -8);
    StartIdleAnim(sun, IdleSizeCb, 15, 17, 2600, 0, true);

    lv_obj_t* cloud = IdleCloud(g, IdleCloudLight(), true);
    lv_obj_align(cloud, LV_ALIGN_CENTER, 7, 8);
    IdleCloudDrift(cloud, 3, 3200);
}

/* 2: cloudy - dark cloud behind, light cloud in front, drifting opposite ways */
static void BuildIdleIconCloudy(lv_obj_t* g) {
    lv_obj_t* back = IdleCloud(g, IdleCloudDark(), true);
    lv_obj_align(back, LV_ALIGN_CENTER, 5, -5);
    IdleCloudDrift(back, -3, 4200);

    lv_obj_t* front = IdleCloud(g, IdleCloudLight(), true);
    lv_obj_align(front, LV_ALIGN_CENTER, -4, 5);
    IdleCloudDrift(front, 3, 3400);
}

/* 3: rain - cloud plus four falling drops */
static void BuildIdleIconRain(lv_obj_t* g) {
    lv_obj_t* cloud = IdleCloud(g, IdleCloudMid(), true);
    lv_obj_align(cloud, LV_ALIGN_TOP_MID, 0, 0);

    static const int kDropX[4] = {8, 17, 26, 35};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* drop = IdleBar(g, IdleRainColor(), kDropX[i], 22, 3, 9, 2);
        StartIdleAnim(drop, reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_y), 22, 32, 900,
                      static_cast<uint32_t>(i) * 220, false);
    }
}

/* 4: snow - cloud plus three falling flakes */
static void BuildIdleIconSnow(lv_obj_t* g) {
    lv_obj_t* cloud = IdleCloud(g, IdleCloudMid(), true);
    lv_obj_align(cloud, LV_ALIGN_TOP_MID, 0, 0);

    static const int kFlakeX[3] = {10, 21, 32};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* flake = IdleDotAt(g, IdleSnowColor(), kFlakeX[i], 22, 5, LV_OPA_COVER);
        int32_t y = 22;
        StartIdleAnim(flake, reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_y), y, 34, 2200,
                      static_cast<uint32_t>(i) * 500, false);
    }
}

/* 5: fog - cloud plus two drifting bars */
static void BuildIdleIconFog(lv_obj_t* g) {
    lv_obj_t* cloud = IdleCloud(g, IdleCloudMid(), true);
    lv_obj_align(cloud, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* bar1 = IdleBar(g, IdleFogColor(), 6, 26, 32, 4, 2);
    StartIdleAnim(bar1, reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_x), 6, 10, 3000, 0, true);

    lv_obj_t* bar2 = IdleBar(g, IdleFogColor(), 11, 33, 24, 3, 2);
    StartIdleAnim(bar2, reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_x), 11, 7, 2400, 0,
                  true);
}

/* 6: storm - dark cloud plus a flickering bolt */
static void BuildIdleIconStorm(lv_obj_t* g) {
    lv_obj_t* cloud = IdleCloud(g, IdleCloudDark(), true);
    lv_obj_align(cloud, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* bolt = lv_obj_create(g);
    lv_obj_remove_style_all(bolt);
    lv_obj_set_size(bolt, 14, 15);
    lv_obj_set_pos(bolt, 21, 22);
    lv_obj_set_style_bg_opa(bolt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(bolt, 0, 0);
    IdleBar(bolt, IdleBoltColor(), 6, 0, 6, 11, 1);
    IdleBar(bolt, IdleBoltColor(), 0, 9, 8, 6, 1);
    lv_obj_set_style_shadow_width(bolt, 0, 0);

    StartIdleAnim(bolt, IdleOpaCb, 255, 90, 280, 0, true);
}

static void SetupIdleWeatherIcons(lv_obj_t** groups) {
    BuildIdleIconClear(groups[0]);
    BuildIdleIconPartly(groups[1]);
    BuildIdleIconCloudy(groups[2]);
    BuildIdleIconRain(groups[3]);
    BuildIdleIconSnow(groups[4]);
    BuildIdleIconFog(groups[5]);
    BuildIdleIconStorm(groups[6]);
}

/* -------------------------------------------------------------------------
 * 第二屏（未来三天）用的小天气图标：26x26，同样用纯 LVGL 对象绘制，
 * 不用字体也不用位图。只在预报数据刷新时重建（天气 10 分钟才变一次），
 * 轮播本身不重建，所以对象数量稳定、不会随翻页累积。
 * ---------------------------------------------------------------------- */
#define IDLE_MINI_ICON 26

static void BuildIdleMiniIcon(lv_obj_t* box, int idx) {
    if (box == nullptr) {
        return;
    }
    lv_obj_clean(box);  // 清掉上一轮的子对象
    if (idx < 0) {
        return;
    }
    switch (idx) {
        case 0: {  // 晴：太阳 + 四个方向的短光线
            IdleDotAt(box, IdleSunCore(), 4, 4, 12, LV_OPA_COVER);
            IdleDotAt(box, IdleSunCore(), 11, 0, 3, LV_OPA_COVER);
            IdleDotAt(box, IdleSunCore(), 11, 19, 3, LV_OPA_COVER);
            IdleDotAt(box, IdleSunCore(), 0, 9, 3, LV_OPA_COVER);
            IdleDotAt(box, IdleSunCore(), 19, 9, 3, LV_OPA_COVER);
            break;
        }
        case 1: {  // 多云：太阳在左上角，云挡在右下
            IdleDotAt(box, IdleSunCore(), 2, 2, 9, LV_OPA_COVER);
            lv_obj_t* cloud = IdleCloud(box, IdleCloudLight(), false);
            lv_obj_set_pos(cloud, 2, 8);
            break;
        }
        case 2: {  // 阴：深云在后、浅云在前
            lv_obj_t* back = IdleCloud(box, IdleCloudDark(), false);
            lv_obj_set_pos(back, 1, 1);
            lv_obj_t* front = IdleCloud(box, IdleCloudLight(), false);
            lv_obj_set_pos(front, 2, 8);
            break;
        }
        case 3: {  // 雨：云 + 三道蓝色雨丝
            lv_obj_t* cloud = IdleCloud(box, IdleCloudMid(), false);
            lv_obj_set_pos(cloud, 1, 0);
            IdleBar(box, IdleRainColor(), 4, 19, 2, 6, 1);
            IdleBar(box, IdleRainColor(), 11, 19, 2, 6, 1);
            IdleBar(box, IdleRainColor(), 18, 19, 2, 6, 1);
            break;
        }
        case 4: {  // 雪：云 + 三颗雪花
            lv_obj_t* cloud = IdleCloud(box, IdleCloudMid(), false);
            lv_obj_set_pos(cloud, 1, 0);
            IdleDotAt(box, IdleSnowColor(), 4, 19, 5, LV_OPA_COVER);
            IdleDotAt(box, IdleSnowColor(), 11, 19, 5, LV_OPA_COVER);
            IdleDotAt(box, IdleSnowColor(), 18, 19, 5, LV_OPA_COVER);
            break;
        }
        case 5: {  // 雾：云 + 两条灰雾线
            lv_obj_t* cloud = IdleCloud(box, IdleCloudMid(), false);
            lv_obj_set_pos(cloud, 1, 0);
            IdleBar(box, IdleFogColor(), 3, 19, 20, 3, 1);
            IdleBar(box, IdleFogColor(), 6, 24, 14, 2, 1);
            break;
        }
        default: {  // 6 雷阵雨：深云 + 黄色闪电
            lv_obj_t* cloud = IdleCloud(box, IdleCloudDark(), false);
            lv_obj_set_pos(cloud, 1, 0);
            IdleBar(box, IdleBoltColor(), 12, 18, 4, 7, 1);
            IdleBar(box, IdleBoltColor(), 9, 22, 5, 4, 1);
            break;
        }
    }
}

void LcdDisplay::SetupIdleHome() {
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Text font is inherited from the screen style, deliberately NOT cached here: the theme
    // font is swapped (and the previous one freed) when the CJK font is loaded from the
    // assets partition, so holding a raw pointer to it would crash LVGL's layout pass.
    lv_color_t fg = lvgl_theme->text_color();

    idle_panel_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(idle_panel_);
    lv_obj_set_size(idle_panel_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(idle_panel_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(idle_panel_, lvgl_theme->background_color(), 0);
    // Photos live inside the panel, so the panel itself stays transparent.
    lv_obj_set_style_bg_opa(idle_panel_, kIdleBgImageCount > 0 ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(idle_panel_, 0, 0);
    lv_obj_set_style_pad_top(idle_panel_, 26, 0);  // keep clear of the status bar
    lv_obj_set_style_pad_bottom(idle_panel_, 6, 0);
    lv_obj_set_style_pad_row(idle_panel_, 2, 0);
    lv_obj_set_style_border_width(idle_panel_, 0, 0);
    lv_obj_set_style_radius(idle_panel_, 0, 0);
    lv_obj_set_flex_flow(idle_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(idle_panel_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(idle_panel_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(idle_panel_, LV_OBJ_FLAG_HIDDEN);

    /* Rotating photo background (behind everything, excluded from the flex layout) */
    if (kIdleBgImageCount > 0) {
        idle_bg_image_ = lv_image_create(idle_panel_);
        lv_obj_set_size(idle_bg_image_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(idle_bg_image_, 0, 0);
        lv_obj_set_style_border_width(idle_bg_image_, 0, 0);
        lv_obj_set_style_pad_all(idle_bg_image_, 0, 0);
        lv_obj_set_style_radius(idle_bg_image_, 0, 0);
        lv_obj_add_flag(idle_bg_image_, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_image_set_inner_align(idle_bg_image_, LV_IMAGE_ALIGN_COVER);
        lv_image_set_src(idle_bg_image_, &kIdleBgImages[idle_bg_index_ % kIdleBgImageCount]);

        // Dark scrim so the white text stays readable on bright photos.
        idle_bg_overlay_ = lv_obj_create(idle_panel_);
        lv_obj_remove_style_all(idle_bg_overlay_);
        lv_obj_set_size(idle_bg_overlay_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(idle_bg_overlay_, 0, 0);
        lv_obj_set_style_bg_color(idle_bg_overlay_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(idle_bg_overlay_, LV_OPA_40, 0);
        lv_obj_set_style_border_width(idle_bg_overlay_, 0, 0);
        lv_obj_set_style_radius(idle_bg_overlay_, 0, 0);
        lv_obj_set_style_pad_all(idle_bg_overlay_, 0, 0);
        lv_obj_add_flag(idle_bg_overlay_, LV_OBJ_FLAG_IGNORE_LAYOUT);

        // 只有一张图时不建轮播定时器：省掉每 15 秒一次的淡入淡出和一次无效重绘
        if (kIdleBgImageCount > 1) {
            idle_bg_timer_ =
                lv_timer_create(&LcdDisplay::IdleBackgroundTimerCb, IDLE_BG_INTERVAL_MS, this);
        }
    }

    /* Big clock */
    idle_clock_label_ = lv_label_create(idle_panel_);
    lv_obj_set_style_text_font(idle_clock_label_, &font_noto_sans_basic_30_4, 0);
    lv_obj_set_style_text_color(idle_clock_label_, fg, 0);
    lv_label_set_text(idle_clock_label_, "--:--");

    /* Date + weekday (built-in CJK font: the assets Noto subset misses 月/日/周) */
    idle_date_label_ = lv_label_create(idle_panel_);
    lv_obj_set_style_text_font(idle_date_label_, &idle_font_16, 0);
    lv_obj_set_style_text_color(idle_date_label_, fg, 0);
    lv_label_set_text(idle_date_label_, "-- --  ---");

    /* Weather row: drawn icon + temperature + condition */
    lv_obj_t* weather_row = lv_obj_create(idle_panel_);
    lv_obj_remove_style_all(weather_row);
    lv_obj_set_size(weather_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(weather_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_row, 0, 0);
    lv_obj_set_style_pad_all(weather_row, 0, 0);
    lv_obj_set_style_pad_column(weather_row, 6, 0);
    lv_obj_set_style_pad_top(weather_row, 4, 0);
    lv_obj_set_flex_flow(weather_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weather_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    idle_weather_row_ = weather_row;

    idle_icon_box_ = lv_obj_create(weather_row);
    lv_obj_remove_style_all(idle_icon_box_);
    lv_obj_set_size(idle_icon_box_, IDLE_ICON_W, IDLE_ICON_H);
    lv_obj_set_style_bg_opa(idle_icon_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_icon_box_, 0, 0);
    lv_obj_set_style_pad_all(idle_icon_box_, 0, 0);

    for (int i = 0; i < 7; ++i) {
        lv_obj_t* group = lv_obj_create(idle_icon_box_);
        lv_obj_remove_style_all(group);
        lv_obj_set_size(group, IDLE_ICON_W, IDLE_ICON_H);
        lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(group, 0, 0);
        lv_obj_set_style_pad_all(group, 0, 0);
        lv_obj_add_flag(group, LV_OBJ_FLAG_HIDDEN);
        idle_icon_groups_[i] = group;
    }

    // Colored + animated icons: sun pulses, clouds drift, rain/snow fall, bolt flickers.
    SetupIdleWeatherIcons(idle_icon_groups_);

    idle_temp_label_ = lv_label_create(weather_row);
    lv_obj_set_style_text_color(idle_temp_label_, fg, 0);
    lv_label_set_text(idle_temp_label_, "--°");

    idle_cond_label_ = lv_label_create(weather_row);
    lv_obj_set_style_text_font(idle_cond_label_, &idle_font_16, 0);
    lv_obj_set_style_text_color(idle_cond_label_, fg, 0);
    lv_label_set_text(idle_cond_label_, "--");

    /* Daily high / low + feels like + PM2.5 */
    idle_range_label_ = lv_label_create(idle_panel_);
    lv_obj_set_style_text_font(idle_range_label_, &idle_font_16, 0);
    lv_obj_set_style_text_color(idle_range_label_, fg, 0);
    lv_label_set_text(idle_range_label_, "");

    /* 第二屏：未来三天。铺满整屏、忽略 flex 布局，透明底让背景照片透出来 */
    idle_page2_ = lv_obj_create(idle_panel_);
    lv_obj_remove_style_all(idle_page2_);
    lv_obj_set_size(idle_page2_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(idle_page2_, 0, 0);
    lv_obj_set_style_bg_opa(idle_page2_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_page2_, 0, 0);
    lv_obj_set_style_radius(idle_page2_, 0, 0);
    lv_obj_set_style_pad_all(idle_page2_, 0, 0);
    lv_obj_add_flag(idle_page2_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_flex_flow(idle_page2_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(idle_page2_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(idle_page2_, 8, 0);

    lv_obj_t* fc_title = lv_label_create(idle_page2_);
    lv_obj_set_style_text_font(fc_title, &idle_font_16, 0);
    lv_obj_set_style_text_color(fc_title, fg, 0);
    lv_label_set_text(fc_title, "未来三天");

    for (int i = 0; i < 3; ++i) {
        // 每行 = [26x26 小天气图标] + [星期 + 温度]
        lv_obj_t* row = lv_obj_create(idle_page2_);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_HOR_RES - 24, IDLE_MINI_ICON + 2);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        idle_forecast_icon_[i] = lv_obj_create(row);
        lv_obj_remove_style_all(idle_forecast_icon_[i]);
        lv_obj_set_size(idle_forecast_icon_[i], IDLE_MINI_ICON, IDLE_MINI_ICON);
        lv_obj_set_style_bg_opa(idle_forecast_icon_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(idle_forecast_icon_[i], 0, 0);
        lv_obj_set_style_pad_all(idle_forecast_icon_[i], 0, 0);

        idle_forecast_label_[i] = lv_label_create(row);
        lv_obj_set_style_text_font(idle_forecast_label_[i], &idle_font_16, 0);
        lv_obj_set_style_text_color(idle_forecast_label_[i], fg, 0);
        lv_label_set_text(idle_forecast_label_[i], "--");
    }
    lv_obj_add_flag(idle_page2_, LV_OBJ_FLAG_HIDDEN);

    /* 第三屏：电脑运行状态（同 WiFi 下 pc_status_server.py 推来的数据） */
    idle_page3_ = lv_obj_create(idle_panel_);
    lv_obj_remove_style_all(idle_page3_);
    lv_obj_set_size(idle_page3_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(idle_page3_, 0, 0);
    lv_obj_set_style_bg_opa(idle_page3_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_page3_, 0, 0);
    lv_obj_set_style_radius(idle_page3_, 0, 0);
    lv_obj_set_style_pad_all(idle_page3_, 0, 0);
    lv_obj_add_flag(idle_page3_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_flex_flow(idle_page3_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(idle_page3_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(idle_page3_, 8, 0);

    lv_obj_t* pc_title = lv_label_create(idle_page3_);
    lv_obj_set_style_text_font(pc_title, &idle_font_16, 0);
    lv_obj_set_style_text_color(pc_title, fg, 0);
    lv_label_set_text(pc_title, "电脑状态");

    /* 状态行：[圆点] [在线 / 离线 + 开机时长] */
    lv_obj_t* pc_state_row = lv_obj_create(idle_page3_);
    lv_obj_remove_style_all(pc_state_row);
    lv_obj_set_size(pc_state_row, LV_HOR_RES - 40, 18);
    lv_obj_set_style_bg_opa(pc_state_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pc_state_row, 0, 0);
    lv_obj_set_style_pad_all(pc_state_row, 0, 0);
    lv_obj_set_style_pad_column(pc_state_row, 6, 0);
    lv_obj_set_flex_flow(pc_state_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pc_state_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    idle_pc_dot_ = lv_obj_create(pc_state_row);
    lv_obj_remove_style_all(idle_pc_dot_);
    lv_obj_set_size(idle_pc_dot_, 8, 8);
    lv_obj_set_style_radius(idle_pc_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(idle_pc_dot_, lv_color_hex(0x9E9E9E), 0);
    lv_obj_set_style_bg_opa(idle_pc_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(idle_pc_dot_, 0, 0);
    lv_obj_set_style_pad_all(idle_pc_dot_, 0, 0);

    idle_pc_state_label_ = lv_label_create(pc_state_row);
    lv_obj_set_style_text_font(idle_pc_state_label_, &idle_font_16, 0);
    lv_obj_set_style_text_color(idle_pc_state_label_, fg, 0);
    lv_label_set_text(idle_pc_state_label_, "等待数据");

    idle_pc_cpu_label_ = lv_label_create(idle_page3_);
    lv_obj_set_style_text_font(idle_pc_cpu_label_, &idle_font_16, 0);
    lv_obj_set_style_text_color(idle_pc_cpu_label_, fg, 0);
    lv_label_set_text(idle_pc_cpu_label_, "CPU --   内存 --");

    idle_pc_gpu_label_ = lv_label_create(idle_page3_);
    lv_obj_set_style_text_font(idle_pc_gpu_label_, &idle_font_16, 0);
    lv_obj_set_style_text_color(idle_pc_gpu_label_, fg, 0);
    lv_label_set_text(idle_pc_gpu_label_, "显卡 --");

    idle_pc_disk_label_ = lv_label_create(idle_page3_);
    lv_obj_set_style_text_font(idle_pc_disk_label_, &idle_font_16, 0);
    lv_obj_set_style_text_color(idle_pc_disk_label_, fg, 0);
    lv_label_set_text(idle_pc_disk_label_, "C盘剩 --");

    lv_obj_add_flag(idle_page3_, LV_OBJ_FLAG_HIDDEN);

    idle_page_timer_ = lv_timer_create(&LcdDisplay::IdlePageTimerCb, IDLE_PAGE_INTERVAL_MS, this);

    UpdateIdleClockLocked();
    ApplyWeatherLocked();
    ApplyForecastLocked();
    ApplyPcStatusLocked();
    ShowIdlePage(0);
}

/* Timer body runs inside the LVGL task, so no display lock is taken here. */
void LcdDisplay::IdleBackgroundTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->AdvanceIdleBackground();
    }
}

void LcdDisplay::AdvanceIdleBackground() {
    if (idle_bg_image_ == nullptr || kIdleBgImageCount <= 1) {
        return;
    }
    // Fade the current photo out; the completed callback swaps the image and fades back in.
    lv_anim_delete(idle_bg_image_, reinterpret_cast<lv_anim_exec_xcb_t>(IdleOpaCb));

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, idle_bg_image_);
    lv_anim_set_exec_cb(&a, reinterpret_cast<lv_anim_exec_xcb_t>(IdleOpaCb));
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_duration(&a, IDLE_BG_FADE_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&a, this);
    lv_anim_set_completed_cb(&a, &LcdDisplay::IdleBackgroundFadeOutDone);
    lv_anim_start(&a);
}

void LcdDisplay::IdleBackgroundFadeOutDone(lv_anim_t* anim) {
    auto* self = static_cast<LcdDisplay*>(lv_anim_get_user_data(anim));
    if (self == nullptr || self->idle_bg_image_ == nullptr) {
        return;
    }
    self->idle_bg_index_ = (self->idle_bg_index_ + 1) % kIdleBgImageCount;
    lv_image_set_src(self->idle_bg_image_, &kIdleBgImages[self->idle_bg_index_]);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, self->idle_bg_image_);
    lv_anim_set_exec_cb(&b, reinterpret_cast<lv_anim_exec_xcb_t>(IdleOpaCb));
    lv_anim_set_values(&b, 0, 255);
    lv_anim_set_duration(&b, IDLE_BG_FADE_IN_MS);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
    lv_anim_start(&b);
}

void LcdDisplay::UpdateIdleClockLocked() {
    if (idle_clock_label_ == nullptr) {
        return;
    }
    static const char* kWeekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    char buf[40];
    if (tm_now.tm_year >= 2025 - 1900) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        lv_label_set_text(idle_clock_label_, buf);
        snprintf(buf, sizeof(buf), "%02d月%02d日 %s", tm_now.tm_mon + 1, tm_now.tm_mday,
                 kWeekdays[tm_now.tm_wday % 7]);
        lv_label_set_text(idle_date_label_, buf);
    } else {
        lv_label_set_text(idle_clock_label_, "--:--");
        lv_label_set_text(idle_date_label_, "-- --  ---");
    }
}

void LcdDisplay::ApplyWeatherLocked() {
    if (idle_temp_label_ == nullptr) {
        return;
    }
    char buf[136];
    if (!has_weather_) {
        lv_label_set_text(idle_temp_label_, "--°");
        lv_label_set_text(idle_cond_label_, "--");
        lv_label_set_text(idle_range_label_, "");
        for (int i = 0; i < 7; ++i) {
            if (idle_icon_groups_[i] != nullptr) {
                lv_obj_add_flag(idle_icon_groups_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }

    snprintf(buf, sizeof(buf), "%.0f°", weather_temp_);
    lv_label_set_text(idle_temp_label_, buf);
    lv_label_set_text(idle_cond_label_, SafeConditionText(weather_code_));

    // 第二行：高/低温 + 体感 + PM2.5 等级（数据分两次到，逐项拼接）
    char line1[48];
    char line2[72];
    line1[0] = '\0';
    line2[0] = '\0';

    if (std::isfinite(weather_max_) && std::isfinite(weather_min_)) {
        snprintf(line1, sizeof(line1), "高%.0f°  低%.0f°", weather_max_, weather_min_);
    }
    int off = 0;
    if (has_feels_like_) {
        off += snprintf(line2 + off, sizeof(line2) - off, "体感%.0f°", weather_feels_);
    }
    if (has_air_quality_) {
        off += snprintf(line2 + off, sizeof(line2) - off, "%sPM2.5 %.0f %s", off > 0 ? "  " : "",
                        air_pm25_, AirLevelText(air_pm25_));
    }

    if (line1[0] != '\0' && line2[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s\n%s", line1, line2);
    } else if (line1[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s", line1);
    } else {
        snprintf(buf, sizeof(buf), "%s", line2);
    }
    lv_label_set_text(idle_range_label_, buf);

    int icon = WeatherIconIndex(weather_code_);
    for (int i = 0; i < 7; ++i) {
        if (idle_icon_groups_[i] == nullptr) {
            continue;
        }
        if (i == icon) {
            lv_obj_remove_flag(idle_icon_groups_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(idle_icon_groups_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::SetIdleHome(bool visible) {
    DisplayLockGuard lock(this);
    if (idle_panel_ == nullptr) {
        return;
    }
    if (visible) {
        UpdateIdleClockLocked();
        ApplyWeatherLocked();
        ApplyForecastLocked();
        ShowIdlePage(0);  // 每次回到待机都先显示第一屏
        lv_obj_remove_flag(idle_panel_, LV_OBJ_FLAG_HIDDEN);
        if (status_label_ != nullptr) {
            lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (preview_image_ != nullptr) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(idle_panel_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_box_ != nullptr && preview_image_cached_ == nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_label_ != nullptr) {
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    idle_home_visible_ = visible;
}

void LcdDisplay::SetWeather(float temperature, float temp_max, float temp_min, int weather_code) {
    DisplayLockGuard lock(this);
    weather_temp_ = temperature;
    weather_max_ = temp_max;
    weather_min_ = temp_min;
    weather_code_ = weather_code;
    has_weather_ = true;
    ApplyWeatherLocked();
}

void LcdDisplay::SetWeatherFeelsLike(float apparent) {
    DisplayLockGuard lock(this);
    if (!std::isfinite(apparent)) {
        has_feels_like_ = false;
    } else {
        weather_feels_ = apparent;
        has_feels_like_ = true;
    }
    ApplyWeatherLocked();
}

void LcdDisplay::SetAirQuality(float pm25) {
    DisplayLockGuard lock(this);
    if (!std::isfinite(pm25) || pm25 < 0.0f) {
        has_air_quality_ = false;
    } else {
        air_pm25_ = pm25;
        has_air_quality_ = true;
    }
    ApplyWeatherLocked();
}

void LcdDisplay::SetDailyForecast(const float* maxs, const float* mins, const int* codes,
                                  int count) {
    DisplayLockGuard lock(this);
    int n = count > 3 ? 3 : count;
    for (int i = 0; i < 3; ++i) {
        if (i < n && maxs != nullptr && mins != nullptr && codes != nullptr) {
            forecast_max_[i] = maxs[i];
            forecast_min_[i] = mins[i];
            forecast_code_[i] = codes[i];
        } else {
            forecast_max_[i] = NAN;
            forecast_min_[i] = NAN;
            forecast_code_[i] = -1;
        }
    }
    has_forecast_ = (n > 0);
    ApplyForecastLocked();
}

void LcdDisplay::ApplyForecastLocked() {
    // 第二屏只显示「星期 + 最高/最低温度」，天气本身用左侧小图标表达，不再写文字。
    static const char* kWeekdays[7] = {"周日", "周一", "周二", "周三",
                                       "周四", "周五", "周六"};
    time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    bool time_ok = tm_now.tm_year >= 2025 - 1900;

    for (int i = 0; i < 3; ++i) {
        if (idle_forecast_label_[i] == nullptr) {
            continue;
        }
        if (!has_forecast_ || !std::isfinite(forecast_max_[i])) {
            lv_label_set_text(idle_forecast_label_[i], "--");
            BuildIdleMiniIcon(idle_forecast_icon_[i], -1);
            continue;
        }
        char name[12];
        if (time_ok) {
            int wd = (tm_now.tm_wday + i + 1) % 7;
            snprintf(name, sizeof(name), "%s", kWeekdays[wd]);
        } else {
            snprintf(name, sizeof(name), "+%d", i + 1);
        }
        char buf[72];
        if (std::isfinite(forecast_min_[i])) {
            snprintf(buf, sizeof(buf), "%s  %.0f°/%.0f°", name, forecast_max_[i],
                     forecast_min_[i]);
        } else {
            snprintf(buf, sizeof(buf), "%s  %.0f°", name, forecast_max_[i]);
        }
        lv_label_set_text(idle_forecast_label_[i], buf);
        BuildIdleMiniIcon(idle_forecast_icon_[i], WeatherIconIndex(forecast_code_[i]));
    }
}

void LcdDisplay::ApplyPcStatusLocked() {
    if (idle_page3_ == nullptr || idle_pc_state_label_ == nullptr) {
        return;
    }
    char buf[48];

    if (!has_pc_) {
        if (idle_pc_dot_ != nullptr) {
            lv_obj_set_style_bg_color(idle_pc_dot_, lv_color_hex(0x9E9E9E), 0);
        }
        lv_label_set_text(idle_pc_state_label_, "等待数据");
        lv_label_set_text(idle_pc_cpu_label_, "CPU --   内存 --");
        lv_label_set_text(idle_pc_gpu_label_, "显卡 --");
        lv_label_set_text(idle_pc_disk_label_, "C盘剩 --");
        return;
    }

    if (idle_pc_dot_ != nullptr) {
        lv_obj_set_style_bg_color(idle_pc_dot_, lv_color_hex(pc_online_ ? 0x4CAF50 : 0xF44336),
                                  0);
    }

    if (!pc_online_) {
        lv_label_set_text(idle_pc_state_label_, "离线");
        lv_label_set_text(idle_pc_cpu_label_, "电脑不在线");
        lv_label_set_text(idle_pc_gpu_label_, "");
        lv_label_set_text(idle_pc_disk_label_, "");
        return;
    }

    snprintf(buf, sizeof(buf), "在线  已开 %d小时%d分", pc_uptime_min_ / 60,
             pc_uptime_min_ % 60);
    lv_label_set_text(idle_pc_state_label_, buf);

    snprintf(buf, sizeof(buf), "CPU %.0f%%   内存 %.0f%%", pc_cpu_, pc_mem_);
    lv_label_set_text(idle_pc_cpu_label_, buf);

    if (pc_gpu_util_ >= 0) {
        if (pc_gpu_temp_ >= 0) {
            snprintf(buf, sizeof(buf), "显卡 %.0f%%   %.0f°", pc_gpu_util_, pc_gpu_temp_);
        } else {
            snprintf(buf, sizeof(buf), "显卡 %.0f%%", pc_gpu_util_);
        }
    } else {
        snprintf(buf, sizeof(buf), "显卡 --");
    }
    lv_label_set_text(idle_pc_gpu_label_, buf);

    snprintf(buf, sizeof(buf), "C盘剩 %.0fG", pc_disk_free_gb_);
    lv_label_set_text(idle_pc_disk_label_, buf);
}

void LcdDisplay::SetPcStatus(bool online, float cpu, float mem, float gpu_util, float gpu_temp,
                             float disk_free_gb, int uptime_min, const char* name) {
    DisplayLockGuard lock(this);
    has_pc_ = true;
    pc_online_ = online;
    if (online) {
        pc_cpu_ = cpu;
        pc_mem_ = mem;
        pc_gpu_util_ = gpu_util;
        pc_gpu_temp_ = gpu_temp;
        pc_disk_free_gb_ = disk_free_gb;
        pc_uptime_min_ = uptime_min;
        if (name != nullptr) {
            snprintf(pc_name_, sizeof(pc_name_), "%s", name);
        }
    }
    ApplyPcStatusLocked();
}

void LcdDisplay::ShowIdlePage(int index) {
    if (idle_page2_ == nullptr) {
        return;
    }
    if (index < 0 || index > 2) {
        index = 0;
    }
    idle_page_index_ = index;

    lv_obj_t* page1[] = {idle_clock_label_, idle_date_label_, idle_weather_row_,
                         idle_range_label_};
    for (int i = 0; i < 4; ++i) {
        if (page1[i] == nullptr) {
            continue;
        }
        if (index == 0) {
            lv_obj_remove_flag(page1[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(page1[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (index == 1) {
        lv_obj_remove_flag(idle_page2_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(idle_page2_, LV_OBJ_FLAG_HIDDEN);
    }
    if (idle_page3_ != nullptr) {
        if (index == 2) {
            lv_obj_remove_flag(idle_page3_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(idle_page3_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::IdlePageTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr || !self->idle_home_visible_) {
        return;  // 不在待机界面时不轮播，回来时统一从第一屏开始
    }
    int page_count = (self->idle_page3_ != nullptr) ? 3 : 2;
    self->ShowIdlePage((self->idle_page_index_ + 1) % page_count);
}

bool LcdDisplay::RenderIdleClock(const struct tm* tm_now, bool force) {
    (void)tm_now;
    (void)force;
    if (idle_panel_ == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);
    if (lv_obj_has_flag(idle_panel_, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    UpdateIdleClockLocked();
    // The panel owns the clock now; keep the small status-bar clock out of the way.
    if (status_label_ != nullptr) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

