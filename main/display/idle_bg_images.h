#pragma once

#include <lvgl.h>

// 空闲屏背景轮播图（240x240 RGB565），由 tools/make_idle_bg.ps1 生成，勿手工修改。
#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t kIdleBgImages[];
extern const int kIdleBgImageCount;
extern const char * const kIdleBgImageNames[];

#ifdef __cplusplus
}
#endif