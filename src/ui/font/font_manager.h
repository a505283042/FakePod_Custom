#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

using FontManagerCacheWriteCallback = void (*)();

// 初始化用户选择的 24px 界面字体。
// 字体优先使用 Flash fontcache mmap；缓存不可用时回退到 TF -> PSRAM。
esp_err_t font_manager_init(FontManagerCacheWriteCallback cache_write_callback = nullptr);

// 获取当前 UI 字体。
// 如果界面字体初始化失败，则自动返回 LVGL 默认字体。
const lv_font_t *font_manager_get_ui_font();

// 判断界面字体是否已经初始化成功。
bool font_manager_is_ready();
