#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

// 初始化原厂 24px 中文字体。
// 字体文件从 TF 卡读取到 PSRAM，运行时不占用固件 Flash。
esp_err_t font_manager_init();

// 获取当前 UI 字体。
// 如果原厂字体初始化失败，则自动返回 LVGL 默认字体。
const lv_font_t *font_manager_get_ui_font();

// 判断原厂字体是否已经初始化成功。
bool font_manager_is_ready();
