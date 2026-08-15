#pragma once

#include "app_manager.h"
#include "esp_err.h"
#include "lvgl.h"

// APP 公共圆环 Overlay。R.39.5.6 起不再复制 Music 视觉实现：
// I4帧解压、palette、中心图标几何、帧进度和径向 hit-test 全部由 player_home 的共享接口提供。
// 调用方页面只保留自己的 LVGL 背景并加矩形压暗层；圆环本体使用原生 I4 外圈 + 原生 I2 中心图层。
esp_err_t app_launcher_overlay_show(lv_obj_t *parent, AppId selected);
void app_launcher_overlay_hide();
void app_launcher_overlay_destroy();
bool app_launcher_overlay_is_visible();
