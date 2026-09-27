#pragma once

#include <stdint.h>

#include "esp_err.h"

// R46.0：BLE Foundation 负责蓝牙生命周期、广播和连接状态。
// 手机遥控通过本服务的加密 GATT 写入入口调用现有 Player 控制，不让 APP 直接操作 NimBLE。
enum class BleRemoteState : uint8_t {
    Disabled = 0,
    Starting,
    Advertising,
    Scanning,
    Connected,
    Stopping,
    RetryWait,
    Unsupported,
};

enum class BleRemoteWorkMode : uint8_t {
    Broadcast = 0,
    Scan,
};

struct BleRemoteSnapshot {
    bool ready = false;
    bool desired_enabled = false;
    bool stack_initialized = false;
    bool connected = false;
    BleRemoteWorkMode work_mode = BleRemoteWorkMode::Broadcast;
    BleRemoteState state = BleRemoteState::Disabled;
    esp_err_t last_error = ESP_OK;
};

// 轻量初始化；不会在用户关闭 BLE 时启动蓝牙栈。
esp_err_t ble_remote_service_init();

// 只更新“用户意图”，真正的 NimBLE 启停由 update() 异步执行，避免阻塞 LVGL/loopTask。
void ble_remote_service_set_enabled(bool enabled);
// 工作模式不写 NVS；每次启动默认广播模式。模式切换通过完整重启 NimBLE 栈完成，避免广播/扫描并发冲突。
void ble_remote_service_set_work_mode(BleRemoteWorkMode mode);
void ble_remote_service_update();

bool ble_remote_service_get_snapshot(BleRemoteSnapshot *out_snapshot);
const char *ble_remote_service_state_name(BleRemoteState state);
const char *ble_remote_service_work_mode_name(BleRemoteWorkMode mode);
