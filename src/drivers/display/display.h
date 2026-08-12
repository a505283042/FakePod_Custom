#pragma once

#include <stdbool.h>

#include "esp_err.h"

// 初始化 CO5300 AMOLED。
esp_err_t display_init();

// 判断显示硬件是否初始化完成。
bool display_is_ready();

// R.38.3：LVGL 首帧完成后才允许显示输出可见。
esp_err_t display_reveal_after_first_frame();

// 全屏封面 Present Hold 兼容回退；仅用于 LVGL 官方刷新链的原子可见性保护。
void display_present_request_hold();
bool display_present_take_hold_request();
bool display_present_set_output(bool enabled);
