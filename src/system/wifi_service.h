#pragma once

#include <stdint.h>

#include "esp_err.h"

// R46.0.55：Wi-Fi STA 生命周期服务。
// 无线策略固定为互斥：启动 Wi-Fi 前必须完整停止 BLE；Wi-Fi 失败/掉线恢复失败后再恢复 BLE。
enum class WifiServiceState : uint8_t {
    Unconfigured = 0,
    ConfiguredIdle,
    WaitingBleOff,
    SavingCredentials,
    WifiInit,
    Connecting,
    Connected,
    CleaningUp,
    RestoringBle,
    Failed,
};

struct WifiServiceSnapshot {
    bool ready = false;
    bool configured = false;
    bool transition_running = false;
    bool connected = false;
    bool ble_fallback_enabled = false;
    WifiServiceState state = WifiServiceState::Unconfigured;
    esp_err_t last_error = ESP_OK;
    char ssid[33] = {};
};

esp_err_t wifi_service_init();
void wifi_service_update();

// Device Settings 的 BLE 开关同时作为 Wi-Fi 失败时是否恢复 BLE 的用户偏好。
void wifi_service_set_ble_fallback_enabled(bool enabled);

// 开机有已保存凭据时调用；不会启动 BLE，而是直接尝试 Wi-Fi。
esp_err_t wifi_service_connect_saved();

// 由 BLE 配网收包完成后提交。函数只复制到 RAM 并创建 Internal-RAM Worker；
// NVS 写入、BLE 关闭和 Wi-Fi 初始化都不在 NimBLE HostTask 上执行。
esp_err_t wifi_service_submit_credentials(const char *ssid, const char *password);

bool wifi_service_has_credentials();
bool wifi_service_owns_radio();
bool wifi_service_get_snapshot(WifiServiceSnapshot *out_snapshot);
const char *wifi_service_state_name(WifiServiceState state);
