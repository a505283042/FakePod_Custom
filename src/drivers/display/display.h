#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"


// 初始化 CO5300 AMOLED
esp_err_t display_init();


// 判断显示屏是否初始化完成
bool display_is_ready();


// 执行启动阶段颜色测试
esp_err_t display_test_colors();


// 获取底层 LCD 句柄，供 LVGL 显示适配层使用
esp_lcd_panel_io_handle_t display_get_panel_io();
esp_lcd_panel_handle_t display_get_panel();