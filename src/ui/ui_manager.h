#pragma once

#include <stdbool.h>
#include "esp_err.h"

// R.38.3：仅建立 LVGL 显示核心与黑色启动页；不依赖触摸、TF 卡或中文字体。
esp_err_t ui_manager_bootstrap_init();

// 初始化 CST820 输入、字体和完整业务界面；复用已经建立的 LVGL 显示核心。
esp_err_t ui_manager_init();

// 判断界面是否初始化完成
bool ui_manager_is_ready();

// 启动核心已经存在时，把临时启动页切换为持久致命错误页。
// 若显示/LVGL 尚不可用则返回 false，Boot 仅保留串口错误并进入安全终态。
bool ui_manager_show_boot_fatal(const char *reason, esp_err_t error);

// 最终触摸能力状态：不仅要求 CST820 就绪，还要求 LVGL 输入设备已经建立。
// Touch Fast Path 失败但同步读取回退可用时仍视为可用。
bool ui_manager_touch_available();
esp_err_t ui_manager_touch_error();
