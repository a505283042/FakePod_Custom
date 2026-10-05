#pragma once

#include <stdint.h>

#include "esp_err.h"

// R46.0.55：Wi-Fi STA 生命周期服务。
// 无线策略固定为互斥：启动 Wi-Fi 前必须完整停止 BLE；关闭 Wi-Fi 后是否恢复 BLE 由已保存的 BLE 开关决定。
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
    // 只有 STA 已拿到 IP，且 Wi-Fi runtime/Remote 启动完整收口后才为 true。
    bool connected = false;
    bool ble_fallback_enabled = false;
    WifiServiceState state = WifiServiceState::Unconfigured;
    esp_err_t last_error = ESP_OK;
    char ssid[33] = {};
};

esp_err_t wifi_service_init();
void wifi_service_update();

// Device Settings 的 BLE 开关决定 Wi-Fi 收尾后是否恢复 BLE。
void wifi_service_set_ble_fallback_enabled(bool enabled);

// 已保存 Wi-Fi 开关为 ON 时调用；启动前会完整停止 BLE。
// boot_rebind_once=true 仅用于开机自动恢复：首次GOT_IP后在发布业务READY前完整重绑定一次，
// 规避实机偶发“有IP但TCP/UDP数据面不通”；运行期手动开关保持单次连接。
esp_err_t wifi_service_connect_saved(bool boot_rebind_once = false);

// 异步关闭 Wi-Fi Remote + STA/driver/netif。若 BLE 保存状态为 ON，清理完成后再恢复 BLE。
esp_err_t wifi_service_stop();

// 由 BLE 配网收包完成后提交。函数只复制到 RAM 并创建 Internal-RAM Worker；
// NVS 写入、BLE 关闭和 Wi-Fi 初始化都不在 NimBLE HostTask 上执行。
esp_err_t wifi_service_submit_credentials(const char *ssid, const char *password);

bool wifi_service_has_credentials();
bool wifi_service_owns_radio();
bool wifi_service_get_snapshot(WifiServiceSnapshot *out_snapshot);
const char *wifi_service_state_name(WifiServiceState state);
