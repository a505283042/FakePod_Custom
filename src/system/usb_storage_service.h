#pragma once

#include "esp_err.h"

// TF 卡 USB MSC 服务：既支持一次性重启维护模式，也支持正常系统运行时安全热接管/热归还。
// 两条路径都必须保证应用侧 VFS 与 TinyUSB raw block device 不并存。
esp_err_t usb_storage_service_start();
esp_err_t usb_storage_service_stop();

// 服务是否仍处于 MSC 生命周期；成功 stop 后变为 false。
bool usb_storage_service_is_active();

// Host 已发送设备级安全释放、USB 已断开，或本次 MSC 根本未被电脑枚举时，
// 返回 true。Windows 仅“弹出卷”不保证产生这些事件，因此 UI 不能依赖它来决定按钮是否显示。
bool usb_storage_service_host_safe_to_return();

// 正常系统热切换时先发布 transition gate，让 system_loop 停止 Player/Artwork/Lyrics 等业务调度。
bool usb_storage_service_begin_runtime_transition();
void usb_storage_service_cancel_runtime_transition();
void usb_storage_service_finish_runtime_transition();

// V3.3：从 MSC 热归还 FakePod。user_confirmed_eject=true 表示用户已经按屏幕提示
// 在电脑端完成安全弹出并显式点击“恢复”；这条人工确认路径用于兼容 Windows 不上报设备级 detach 的情况。
// return transition 在 USB teardown + /sdcard 重挂载完成前持续阻断正常业务。
bool usb_storage_service_begin_runtime_return(bool user_confirmed_eject);
void usb_storage_service_cancel_runtime_return();
void usb_storage_service_finish_runtime_return(bool app_storage_ready);

bool usb_storage_service_blocks_normal_runtime();
