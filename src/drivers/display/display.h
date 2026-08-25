#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"   // esp_lcd_panel_handle_t

// 初始化 CO5300 AMOLED。
esp_err_t display_init();

// 判断显示硬件是否初始化完成。
bool display_is_ready();

// LVGL 首帧完成后才允许显示输出可见。
esp_err_t display_reveal_after_first_frame();

// 全屏封面 Present Hold 兼容回退；仅用于 LVGL 官方刷新链的原子可见性保护。
void display_present_request_hold();
bool display_present_take_hold_request();
bool display_present_set_output(bool enabled);

// 获取底层 LCD panel 句柄（screen_lock_simple 等模块需要直接调用
// CO5300 亮度 / disp_on_off 控制）。未初始化完成时返回 nullptr。
esp_lcd_panel_handle_t display_get_panel_handle(void);
