#pragma once

#include <stdbool.h>
#include "esp_err.h"

// R.38.3：仅建立 LVGL 显示核心与黑色启动页；不依赖触摸、TF 卡或中文字体。
esp_err_t ui_manager_bootstrap_init();

// 初始化 CST820 输入、字体和完整业务界面；复用已经建立的 LVGL 显示核心。
esp_err_t ui_manager_init();

// 判断界面是否初始化完成
bool ui_manager_is_ready();
