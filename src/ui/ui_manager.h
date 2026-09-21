#pragma once

#include <stdbool.h>
#include "esp_err.h"

// 仅建立 LVGL 显示核心与黑色启动页；不依赖触摸、TF 卡或中文字体。
esp_err_t ui_manager_bootstrap_init();

// 初始化 CST820 输入、字体和完整业务界面；复用已经建立的 LVGL 显示核心。
esp_err_t ui_manager_init();

// 判断界面是否初始化完成
bool ui_manager_is_ready();

// 启动核心已经存在时，把临时启动页切换为持久致命错误页。
// 若显示/LVGL 尚不可用则返回 false，Boot 仅保留串口错误并进入安全终态。
bool ui_manager_show_boot_fatal(const char *reason, esp_err_t error);

// 启动期复用启动页显示首次建库或增量更新状态；已有曲库且无变化时不会额外提示。
// 首次建库时可传入当前已经发现的歌曲数；0 表示刚开始扫描。
bool ui_manager_show_library_build_progress(uint32_t scanned_count = 0U);
bool ui_manager_show_library_build_complete(uint32_t total_count);
bool ui_manager_show_library_update_progress();
bool ui_manager_show_library_update_complete(
    uint32_t added_count,
    uint32_t removed_count,
    uint32_t updated_count
);

// 一次性 TF 卡 USB MSC 服务模式复用启动页，不建立完整 Music/Settings UI。
// 返回 false 仅表示提示页不可用，不影响 USB 服务本身。
bool ui_manager_show_usb_storage_service();

// 最终触摸能力状态：不仅要求 CST820 就绪，还要求 LVGL 输入设备已经建立。
// Touch Fast Path 失败但同步读取回退可用时仍视为可用。
bool ui_manager_touch_available();
esp_err_t ui_manager_touch_error();
