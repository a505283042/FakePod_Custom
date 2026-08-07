#pragma once

#include <stdbool.h>
#include "esp_err.h"

// 初始化 LVGL、CO5300 显示适配和 CST820 触摸输入
esp_err_t ui_manager_init();

// 判断界面是否初始化完成
bool ui_manager_is_ready();
