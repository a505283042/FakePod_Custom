#pragma once

#include "esp_err.h"

// TF 卡 USB MSC 服务：既支持一次性重启维护模式，也支持正常系统运行时安全热接管/热归还。
// 两条路径都必须保证应用侧 VFS 与 TinyUSB raw block device 不并存。
esp_err_t usb_storage_service_start();
esp_err_t usb_storage_service_stop();

// 服务是否仍处于 MSC 生命周期；成功 stop 后变为 false。
bool usb_storage_service_is_active();

// V3.5：实机确认 Windows 安全弹出会把 MSC storage 切回 APP owner；
// owner=APP 稳定 250ms 后返回 true。首次 USB 枚举的 DETACHED 不再作为安全归还判据。
bool usb_storage_service_host_safe_to_return();

// V3.5：Windows 安全弹出在实机上会触发 MSC MOUNT_COMPLETE owner=APP。
// 只用该 storage-owner 事件请求自动热归还，避免首次 USB 枚举的 DETACHED 抖动误触发。
bool usb_storage_service_auto_return_requested();

// 正常系统热切换时先发布 transition gate，让 system_loop 停止 Player/Artwork/Lyrics 等业务调度。
bool usb_storage_service_begin_runtime_transition();
void usb_storage_service_cancel_runtime_transition();
void usb_storage_service_finish_runtime_transition();

// 从 MSC 热归还 FakePod。V3.5 正常路径由 owner=APP 自动触发；
// user_confirmed_eject=true 的手动按钮仅作为异常/兼容兜底。return transition 在
// MSC+CDC -> CDC-only 重枚举和 /sdcard 重挂载完成前持续阻断正常业务。
bool usb_storage_service_begin_runtime_return(bool user_confirmed_eject);
void usb_storage_service_cancel_runtime_return();
void usb_storage_service_finish_runtime_return(bool app_storage_ready);

bool usb_storage_service_blocks_normal_runtime();
