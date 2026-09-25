#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lvgl.h"

using FontManagerCacheWriteCallback = void (*)();

// 初始化用户选择的界面字体。
// 字体优先使用 Flash fontcache mmap；缓存不可用时回退到 TF -> PSRAM。
esp_err_t font_manager_init(FontManagerCacheWriteCallback cache_write_callback = nullptr);

// 获取当前 UI 字体。
// 如果界面字体初始化失败，则自动返回 LVGL 默认字体。
const lv_font_t *font_manager_get_ui_font();

// 判断界面字体是否已经初始化成功。
bool font_manager_is_ready();

// 启动时从 /sdcard/FONTS 扫描出的有效字体列表。文件名保持 UTF-8 原样。
const char *font_manager_active_filename();
bool font_manager_is_available_filename(const char *filename);

// 返回 current_filename 的下一项；当前项不存在时返回第一项。无可用字体时返回 nullptr。
const char *font_manager_next_available_filename(const char *current_filename);

// 文件名去掉末尾 .bin 后作为设置页显示名称。
bool font_manager_format_display_name(
    const char *filename,
    char *out_name,
    size_t out_name_size);
