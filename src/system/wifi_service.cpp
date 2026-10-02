#include "wifi_service.h"

#include <stdio.h>
#include <string.h>

#include "ble_remote_service.h"
#include "wifi_remote_service.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "WiFi服务";

namespace {

static constexpr const char *kNvsNamespace = "wifi_cfg";
static constexpr uint16_t kNvsSchema = 1U;
static constexpr TickType_t kBleStopTimeout = pdMS_TO_TICKS(6000);
static constexpr TickType_t kBleRestoreTimeout = pdMS_TO_TICKS(6000);
static constexpr TickType_t kBlePollDelay = pdMS_TO_TICKS(20);
static constexpr TickType_t kProvisionAckGrace = pdMS_TO_TICKS(350);
static constexpr TickType_t kConnectTimeout = pdMS_TO_TICKS(15000);
static constexpr TickType_t kReconnectTimeout = pdMS_TO_TICKS(10000);
static constexpr uint8_t kMaxConnectAttempts = 5U;
// R46.0.57：FakePod 仅需低带宽遥控/NAS音频，收紧 Wi-Fi burst buffer，保护 FLAC 切歌峰值的 Internal RAM。
static constexpr int kWifiStaticRxBuffers = 6;
static constexpr int kWifiDynamicRxBuffers = 16;
static constexpr int kWifiDynamicTxBuffers = 16;
static constexpr uint32_t kWorkerStackBytes = 6144U;
static constexpr UBaseType_t kWorkerPriority = 2U;
static constexpr BaseType_t kWorkerCore = 1;
static constexpr UBaseType_t kWorkerStackCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
static constexpr EventBits_t kConnectedBit = BIT0;
static constexpr EventBits_t kFailedBit = BIT1;

struct WifiCredentials {
    char ssid[33] = {};
    char password[64] = {};
};

enum class WorkerMode : uint8_t {
    ConnectSaved = 0,
    Provision,
    Recover,
};

static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static bool g_ready = false;
static bool g_configured = false;
static bool g_transition_running = false;
static bool g_connected = false;
static bool g_ble_fallback_enabled = true;
static bool g_recovery_requested = false;
static bool g_connecting = false;
static bool g_shutting_down = false;
static uint8_t g_connect_attempts = 0U;
static WifiServiceState g_state = WifiServiceState::Unconfigured;
static esp_err_t g_last_error = ESP_OK;
static WifiCredentials g_saved = {};
static WifiCredentials g_pending = {};
static bool g_pending_valid = false;
static WorkerMode g_worker_mode = WorkerMode::ConnectSaved;

static bool g_netif_initialized = false;
static bool g_event_loop_owned = false;
static bool g_wifi_initialized = false;
static bool g_wifi_started = false;
static esp_netif_t *g_sta_netif = nullptr;
static EventGroupHandle_t g_wifi_events = nullptr;
static esp_event_handler_instance_t g_wifi_handler = nullptr;
static esp_event_handler_instance_t g_ip_handler = nullptr;

static void set_state(WifiServiceState state, esp_err_t error = ESP_OK)
{
    portENTER_CRITICAL(&g_lock);
    g_state = state;
    g_last_error = error;
    portEXIT_CRITICAL(&g_lock);
}


static bool credentials_valid(const char *ssid, const char *password)
{
    if (ssid == nullptr || password == nullptr) return false;
    const size_t ssid_len = strlen(ssid);
    const size_t password_len = strlen(password);
    return ssid_len >= 1U && ssid_len <= 32U && password_len <= 63U;
}

static esp_err_t load_credentials(WifiCredentials *out)
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = {};

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (ret != ESP_OK) return ret;

    uint16_t schema = 0U;
    ret = nvs_get_u16(handle, "schema", &schema);
    if (ret != ESP_OK || schema != kNvsSchema) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    size_t ssid_size = sizeof(out->ssid);
    size_t password_size = sizeof(out->password);
    ret = nvs_get_str(handle, "ssid", out->ssid, &ssid_size);
    if (ret == ESP_OK) ret = nvs_get_str(handle, "pass", out->password, &password_size);
    nvs_close(handle);
    if (ret != ESP_OK) return ret;
    return credentials_valid(out->ssid, out->password) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t save_credentials(const WifiCredentials &credentials)
{
    if (!credentials_valid(credentials.ssid, credentials.password)) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u16(handle, "schema", kNvsSchema);
    if (ret == ESP_OK) ret = nvs_set_str(handle, "ssid", credentials.ssid);
    if (ret == ESP_OK) ret = nvs_set_str(handle, "pass", credentials.password);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

static bool wait_ble_disabled(TickType_t timeout)
{
    const TickType_t started = xTaskGetTickCount();
    while (static_cast<TickType_t>(xTaskGetTickCount() - started) < timeout) {
        ble_remote_service_update();
        BleRemoteSnapshot ble = {};
        if (ble_remote_service_get_snapshot(&ble) &&
            !ble.stack_initialized && ble.state == BleRemoteState::Disabled) {
            return true;
        }
        vTaskDelay(kBlePollDelay);
    }
    return false;
}

static void restore_ble_fallback()
{
    bool enabled = false;
    portENTER_CRITICAL(&g_lock);
    enabled = g_ble_fallback_enabled;
    g_state = WifiServiceState::RestoringBle;
    portEXIT_CRITICAL(&g_lock);

    ble_remote_service_set_enabled(enabled);
    ble_remote_service_update();
    if (!enabled) {
        ESP_LOGI(TAG, "Wi-Fi不可用：用户BLE设置=关闭，不启动回退BLE");
        return;
    }

    const TickType_t started = xTaskGetTickCount();
    while (static_cast<TickType_t>(xTaskGetTickCount() - started) < kBleRestoreTimeout) {
        ble_remote_service_update();
        BleRemoteSnapshot ble = {};
        if (ble_remote_service_get_snapshot(&ble) && ble.stack_initialized &&
            (ble.state == BleRemoteState::Advertising || ble.state == BleRemoteState::Connected)) {
            ESP_LOGI(TAG, "Wi-Fi回退：BLE已恢复 state=%s", ble_remote_service_state_name(ble.state));
            return;
        }
        vTaskDelay(kBlePollDelay);
    }
    ESP_LOGW(TAG, "Wi-Fi回退：BLE恢复仍在后台进行");
}

static void wifi_event_handler(void *, esp_event_base_t base, int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        bool retry = false;
        bool fail = false;
        bool request_recovery = false;

        portENTER_CRITICAL(&g_lock);
        if (!g_shutting_down) {
            if (g_connecting) {
                if (g_connect_attempts < kMaxConnectAttempts) {
                    ++g_connect_attempts;
                    retry = true;
                } else {
                    fail = true;
                }
            } else if (g_connected) {
                g_connected = false;
                g_state = WifiServiceState::Connecting;
                g_recovery_requested = true;
                request_recovery = true;
            }
        }
        portEXIT_CRITICAL(&g_lock);

        if (retry) {
            (void)esp_wifi_connect();
        } else if (fail && g_wifi_events != nullptr) {
            xEventGroupSetBits(g_wifi_events, kFailedBit);
        } else if (request_recovery) {
            ESP_LOGW(TAG, "Wi-Fi连接已丢失：进入10秒重连窗口，失败后恢复BLE");
        }
        return;
    }

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<const ip_event_got_ip_t *>(event_data);
        portENTER_CRITICAL(&g_lock);
        g_connected = true;
        g_connecting = false;
        g_state = WifiServiceState::Connected;
        g_last_error = ESP_OK;
        portEXIT_CRITICAL(&g_lock);
        if (event != nullptr) {
            ESP_LOGI(TAG, "Wi-Fi已连接：ssid=%s ip=" IPSTR,
                g_saved.ssid,
                IP2STR(&event->ip_info.ip));
        } else {
            ESP_LOGI(TAG, "Wi-Fi已连接：ssid=%s", g_saved.ssid);
        }
        if (g_wifi_events != nullptr) xEventGroupSetBits(g_wifi_events, kConnectedBit);
    }
}

static void cleanup_wifi()
{
    set_state(WifiServiceState::CleaningUp);
    wifi_remote_service_stop();
    portENTER_CRITICAL(&g_lock);
    g_shutting_down = true;
    g_connecting = false;
    g_connected = false;
    g_recovery_requested = false;
    portEXIT_CRITICAL(&g_lock);

    if (g_wifi_started) {
        (void)esp_wifi_stop();
        g_wifi_started = false;
    }
    if (g_wifi_handler != nullptr) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, g_wifi_handler);
        g_wifi_handler = nullptr;
    }
    if (g_ip_handler != nullptr) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, g_ip_handler);
        g_ip_handler = nullptr;
    }
    if (g_wifi_initialized) {
        (void)esp_wifi_deinit();
        g_wifi_initialized = false;
    }
    if (g_sta_netif != nullptr) {
        esp_netif_destroy_default_wifi(g_sta_netif);
        g_sta_netif = nullptr;
    }
    if (g_wifi_events != nullptr) {
        vEventGroupDelete(g_wifi_events);
        g_wifi_events = nullptr;
    }
    if (g_event_loop_owned) {
        (void)esp_event_loop_delete_default();
        g_event_loop_owned = false;
    }
    // ESP-IDF 5.5 的 esp_netif_deinit() 尚不支持；TCP/IP 栈按应用生命周期只初始化一次。
    // 这里仅释放本轮 STA netif / event loop / Wi-Fi Driver，后续重连继续复用 esp-netif。

    portENTER_CRITICAL(&g_lock);
    g_shutting_down = false;
    portEXIT_CRITICAL(&g_lock);
}

static esp_err_t init_wifi_stack()
{
    esp_err_t ret = ESP_OK;
    if (!g_netif_initialized) {
        ret = esp_netif_init();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
        g_netif_initialized = true;
    }

    ret = esp_event_loop_create_default();
    if (ret == ESP_OK) {
        g_event_loop_owned = true;
    } else if (ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    g_sta_netif = esp_netif_create_default_wifi_sta();
    if (g_sta_netif == nullptr) return ESP_ERR_NO_MEM;

    g_wifi_events = xEventGroupCreate();
    if (g_wifi_events == nullptr) return ESP_ERR_NO_MEM;

    ret = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        nullptr,
        &g_wifi_handler);
    if (ret != ESP_OK) return ret;
    ret = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        nullptr,
        &g_ip_handler);
    if (ret != ESP_OK) return ret;
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    init_config.static_rx_buf_num = kWifiStaticRxBuffers;
    init_config.dynamic_rx_buf_num = kWifiDynamicRxBuffers;
    init_config.dynamic_tx_buf_num = kWifiDynamicTxBuffers;
    ESP_LOGI(TAG,
        "Wi-Fi Low RAM档：static_rx=%d dynamic_rx=%d dynamic_tx=%d rx_ba_win=%d",
        init_config.static_rx_buf_num,
        init_config.dynamic_rx_buf_num,
        init_config.dynamic_tx_buf_num,
        init_config.rx_ba_win);
    ret = esp_wifi_init(&init_config);
    if (ret != ESP_OK) return ret;
    g_wifi_initialized = true;

    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) return ret;
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;

    wifi_config_t station = {};
    const size_t ssid_len = strlen(g_saved.ssid);
    const size_t password_len = strlen(g_saved.password);
    memcpy(station.sta.ssid, g_saved.ssid, ssid_len);
    if (password_len > 0U) memcpy(station.sta.password, g_saved.password, password_len);
    station.sta.threshold.authmode = WIFI_AUTH_OPEN;
    station.sta.pmf_cfg.capable = true;
    station.sta.pmf_cfg.required = false;
    ret = esp_wifi_set_config(WIFI_IF_STA, &station);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;
    g_wifi_started = true;
    return ESP_OK;
}

static esp_err_t wait_for_connection(TickType_t timeout)
{
    if (g_wifi_events == nullptr) return ESP_ERR_INVALID_STATE;
    xEventGroupClearBits(g_wifi_events, kConnectedBit | kFailedBit);
    portENTER_CRITICAL(&g_lock);
    g_connecting = true;
    g_connect_attempts = 1U;
    g_state = WifiServiceState::Connecting;
    portEXIT_CRITICAL(&g_lock);

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        portENTER_CRITICAL(&g_lock);
        g_connecting = false;
        portEXIT_CRITICAL(&g_lock);
        return ret;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        g_wifi_events,
        kConnectedBit | kFailedBit,
        pdTRUE,
        pdFALSE,
        timeout);

    portENTER_CRITICAL(&g_lock);
    g_connecting = false;
    portEXIT_CRITICAL(&g_lock);

    if ((bits & kConnectedBit) != 0U) return ESP_OK;
    return (bits & kFailedBit) != 0U ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

static void worker_finish(bool connected, esp_err_t error)
{
    portENTER_CRITICAL(&g_lock);
    g_connected = connected;
    g_last_error = error;
    g_state = connected ? WifiServiceState::Connected : WifiServiceState::Failed;
    g_transition_running = false;
    portEXIT_CRITICAL(&g_lock);
}

static void wifi_worker(void *)
{
    WorkerMode mode = WorkerMode::ConnectSaved;
    portENTER_CRITICAL(&g_lock);
    mode = g_worker_mode;
    portEXIT_CRITICAL(&g_lock);

    esp_err_t ret = ESP_OK;
    if (mode == WorkerMode::Provision) {
        // 让最后一个加密 GATT Write Response 有时间离开发送队列，再终止 BLE。
        vTaskDelay(kProvisionAckGrace);
    }

    if (mode != WorkerMode::Recover) {
        set_state(WifiServiceState::WaitingBleOff);
        ESP_LOGI(TAG, "%s：先完整停止BLE，再启动Wi-Fi",
            mode == WorkerMode::Provision ? "手机配网" : "开机自动连接");
        ble_remote_service_set_enabled(false);
        ble_remote_service_update();
        if (!wait_ble_disabled(kBleStopTimeout)) {
            ESP_LOGE(TAG, "BLE在6秒内未完整停止，取消Wi-Fi启动");
            ret = ESP_ERR_TIMEOUT;
            goto failed;
        }
    }

    if (mode == WorkerMode::Provision) {
        WifiCredentials pending = {};
        portENTER_CRITICAL(&g_lock);
        pending = g_pending;
        portEXIT_CRITICAL(&g_lock);

        set_state(WifiServiceState::SavingCredentials);
        ret = save_credentials(pending);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "保存Wi-Fi配置失败：%s", esp_err_to_name(ret));
            goto failed;
        }
        portENTER_CRITICAL(&g_lock);
        g_saved = pending;
        g_configured = true;
        g_pending_valid = false;
        portEXIT_CRITICAL(&g_lock);
        ESP_LOGI(TAG, "Wi-Fi配置已保存：ssid=%s password=%uB",
            g_saved.ssid,
            static_cast<unsigned>(strlen(g_saved.password)));
    }

    if (mode == WorkerMode::Recover) {
        ESP_LOGI(TAG, "Wi-Fi掉线：尝试在10秒内恢复现有STA连接");
        ret = wait_for_connection(kReconnectTimeout);
        if (ret == ESP_OK) {
            ret = wifi_remote_service_start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi已恢复但遥控服务启动失败：%s", esp_err_to_name(ret));
                cleanup_wifi();
                restore_ble_fallback();
                worker_finish(false, ret);
                vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
                return;
            }
            worker_finish(true, ESP_OK);
            vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
            return;
        }
        ESP_LOGW(TAG, "Wi-Fi重连失败：%s；释放Wi-Fi后恢复BLE", esp_err_to_name(ret));
        cleanup_wifi();
        restore_ble_fallback();
        worker_finish(false, ret);
        vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
        return;
    }

    set_state(WifiServiceState::WifiInit);
    ret = init_wifi_stack();
    if (ret != ESP_OK) goto failed;

    ret = wait_for_connection(kConnectTimeout);
    if (ret != ESP_OK) goto failed;

    ret = wifi_remote_service_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi已连接但遥控服务启动失败：%s", esp_err_to_name(ret));
        goto failed;
    }

    ESP_LOGI(TAG, "Wi-Fi接管成功：ssid=%s；BLE保持关闭；Wi-Fi遥控=READY", g_saved.ssid);
    worker_finish(true, ESP_OK);
    vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
    return;

failed:
    if (g_wifi_initialized || g_wifi_started || g_sta_netif != nullptr || g_wifi_events != nullptr) {
        cleanup_wifi();
    }
    ESP_LOGE(TAG, "Wi-Fi启动/连接失败：%s；恢复BLE回退链路", esp_err_to_name(ret));
    restore_ble_fallback();
    worker_finish(false, ret);
    vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
}

static esp_err_t start_worker(WorkerMode mode)
{
    portENTER_CRITICAL(&g_lock);
    if (!g_ready || g_transition_running) {
        portEXIT_CRITICAL(&g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (mode != WorkerMode::Recover && !g_configured && mode != WorkerMode::Provision) {
        portEXIT_CRITICAL(&g_lock);
        return ESP_ERR_NOT_FOUND;
    }
    g_transition_running = true;
    g_worker_mode = mode;
    g_last_error = ESP_OK;
    portEXIT_CRITICAL(&g_lock);

    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        wifi_worker,
        "wifi_ctl",
        kWorkerStackBytes,
        nullptr,
        kWorkerPriority,
        nullptr,
        kWorkerCore,
        kWorkerStackCaps);
    if (created == pdPASS) return ESP_OK;

    portENTER_CRITICAL(&g_lock);
    g_transition_running = false;
    g_last_error = ESP_ERR_NO_MEM;
    g_state = WifiServiceState::Failed;
    portEXIT_CRITICAL(&g_lock);
    return ESP_ERR_NO_MEM;
}

} // namespace

esp_err_t wifi_service_init()
{
    portENTER_CRITICAL(&g_lock);
    if (g_ready) {
        portEXIT_CRITICAL(&g_lock);
        return ESP_OK;
    }
    g_ready = true;
    g_configured = false;
    g_transition_running = false;
    g_connected = false;
    g_recovery_requested = false;
    g_last_error = ESP_OK;
    g_state = WifiServiceState::Unconfigured;
    portEXIT_CRITICAL(&g_lock);

    WifiCredentials loaded = {};
    const esp_err_t ret = load_credentials(&loaded);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&g_lock);
        g_saved = loaded;
        g_configured = true;
        g_state = WifiServiceState::ConfiguredIdle;
        portEXIT_CRITICAL(&g_lock);
        ESP_LOGI(TAG, "Wi-Fi配置已加载：ssid=%s", loaded.ssid);
        return ESP_OK;
    }
    if (ret == ESP_ERR_NOT_FOUND || ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "Wi-Fi尚未配置：等待手机通过BLE下发SSID/密码");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "读取Wi-Fi配置失败：%s；保持BLE模式", esp_err_to_name(ret));
    return ret;
}

void wifi_service_update()
{
    bool request_recovery = false;
    portENTER_CRITICAL(&g_lock);
    if (g_ready && g_recovery_requested && !g_transition_running) {
        g_recovery_requested = false;
        request_recovery = true;
    }
    portEXIT_CRITICAL(&g_lock);
    if (request_recovery) {
        const esp_err_t ret = start_worker(WorkerMode::Recover);
        if (ret != ESP_OK) {
            portENTER_CRITICAL(&g_lock);
            g_recovery_requested = true;
            portEXIT_CRITICAL(&g_lock);
        }
    }
}

void wifi_service_set_ble_fallback_enabled(bool enabled)
{
    portENTER_CRITICAL(&g_lock);
    g_ble_fallback_enabled = enabled;
    portEXIT_CRITICAL(&g_lock);
}

esp_err_t wifi_service_connect_saved()
{
    portENTER_CRITICAL(&g_lock);
    const bool configured = g_ready && g_configured;
    portEXIT_CRITICAL(&g_lock);
    if (!configured) return ESP_ERR_NOT_FOUND;
    return start_worker(WorkerMode::ConnectSaved);
}

esp_err_t wifi_service_submit_credentials(const char *ssid, const char *password)
{
    if (!credentials_valid(ssid, password)) return ESP_ERR_INVALID_ARG;

    const size_t ssid_len = strlen(ssid);
    const size_t password_len = strlen(password);
    portENTER_CRITICAL(&g_lock);
    if (!g_ready || g_transition_running || g_connected) {
        portEXIT_CRITICAL(&g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&g_pending, 0, sizeof(g_pending));
    memcpy(g_pending.ssid, ssid, ssid_len);
    if (password_len > 0U) memcpy(g_pending.password, password, password_len);
    g_pending_valid = true;
    portEXIT_CRITICAL(&g_lock);

    const esp_err_t ret = start_worker(WorkerMode::Provision);
    if (ret != ESP_OK) {
        portENTER_CRITICAL(&g_lock);
        g_pending_valid = false;
        portEXIT_CRITICAL(&g_lock);
    }
    return ret;
}

bool wifi_service_has_credentials()
{
    portENTER_CRITICAL(&g_lock);
    const bool configured = g_ready && g_configured;
    portEXIT_CRITICAL(&g_lock);
    return configured;
}

bool wifi_service_owns_radio()
{
    portENTER_CRITICAL(&g_lock);
    const bool owns = g_transition_running || g_connected || g_recovery_requested ||
        g_state == WifiServiceState::WifiInit || g_state == WifiServiceState::Connecting ||
        g_state == WifiServiceState::CleaningUp || g_state == WifiServiceState::RestoringBle;
    portEXIT_CRITICAL(&g_lock);
    return owns;
}

bool wifi_service_get_snapshot(WifiServiceSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    portENTER_CRITICAL(&g_lock);
    out_snapshot->ready = g_ready;
    out_snapshot->configured = g_configured;
    out_snapshot->transition_running = g_transition_running;
    out_snapshot->connected = g_connected;
    out_snapshot->ble_fallback_enabled = g_ble_fallback_enabled;
    out_snapshot->state = g_state;
    out_snapshot->last_error = g_last_error;
    memcpy(out_snapshot->ssid, g_saved.ssid, sizeof(out_snapshot->ssid));
    portEXIT_CRITICAL(&g_lock);
    return out_snapshot->ready;
}

const char *wifi_service_state_name(WifiServiceState state)
{
    switch (state) {
        case WifiServiceState::Unconfigured: return "未配置";
        case WifiServiceState::ConfiguredIdle: return "已保存";
        case WifiServiceState::WaitingBleOff: return "关闭蓝牙";
        case WifiServiceState::SavingCredentials: return "保存配置";
        case WifiServiceState::WifiInit: return "初始化";
        case WifiServiceState::Connecting: return "连接中";
        case WifiServiceState::Connected: return "已连接";
        case WifiServiceState::CleaningUp: return "清理中";
        case WifiServiceState::RestoringBle: return "恢复蓝牙";
        case WifiServiceState::Failed: return "连接失败";
        default: return "未知";
    }
}
