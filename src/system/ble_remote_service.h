#pragma once

#include <stdint.h>

#include "esp_err.h"

// R46.0：BLE Foundation 只负责蓝牙生命周期、广播和连接状态。
// 后续 HID Host / 遥控协议都复用这里的生命周期，不让具体 APP 直接操作 NimBLE。
enum class BleRemoteState : uint8_t {
    Disabled = 0,
    Starting,
    Advertising,
    Connected,
    Stopping,
    RetryWait,
    Unsupported,
};

struct BleRemoteSnapshot {
    bool ready = false;
    bool desired_enabled = false;
    bool stack_initialized = false;
    bool connected = false;
    BleRemoteState state = BleRemoteState::Disabled;
    esp_err_t last_error = ESP_OK;
};

// 轻量初始化；不会在用户关闭 BLE 时启动蓝牙栈。
esp_err_t ble_remote_service_init();

// 只更新“用户意图”，真正的 NimBLE 启停由 update() 异步执行，避免阻塞 LVGL/loopTask。
void ble_remote_service_set_enabled(bool enabled);
void ble_remote_service_update();

bool ble_remote_service_get_snapshot(BleRemoteSnapshot *out_snapshot);
const char *ble_remote_service_state_name(BleRemoteState state);
