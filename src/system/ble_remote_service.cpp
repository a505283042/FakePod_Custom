#include "ble_remote_service.h"

#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(CONFIG_BT_NIMBLE_ENABLED) && CONFIG_BT_NIMBLE_ENABLED && \
    defined(CONFIG_BT_NIMBLE_ROLE_PERIPHERAL) && CONFIG_BT_NIMBLE_ROLE_PERIPHERAL && \
    defined(CONFIG_BT_NIMBLE_ROLE_BROADCASTER) && CONFIG_BT_NIMBLE_ROLE_BROADCASTER && \
    defined(CONFIG_BT_NIMBLE_GATT_SERVER) && CONFIG_BT_NIMBLE_GATT_SERVER && \
    defined(CONFIG_BT_NIMBLE_GAP_SERVICE) && CONFIG_BT_NIMBLE_GAP_SERVICE
#define FAKEPOD_BLE_FOUNDATION_ENABLED 1
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
extern "C" void ble_store_config_init(void);
#else
#define FAKEPOD_BLE_FOUNDATION_ENABLED 0
#endif

static const char *TAG = "BLE服务";

namespace {

static constexpr const char *kDeviceName = "FakePod";
static constexpr TickType_t kRetryDelay = pdMS_TO_TICKS(2000);
static constexpr TickType_t kStartTimeout = pdMS_TO_TICKS(5000);
static constexpr uint32_t kTransitionTaskStackBytes = 4096U;
static constexpr UBaseType_t kTransitionTaskPriority = 2U;
static constexpr BaseType_t kTransitionTaskCore = 1;

static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static bool g_ready = false;
static bool g_desired_enabled = false;
static bool g_stack_initialized = false;
static bool g_connected = false;
static bool g_transition_running = false;
static BleRemoteState g_state = BleRemoteState::Disabled;
static esp_err_t g_last_error = ESP_OK;
static TickType_t g_retry_due_tick = 0;
static TickType_t g_state_since_tick = 0;

#if FAKEPOD_BLE_FOUNDATION_ENABLED
static uint8_t g_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// R46.0.9：先建立最小 HID Consumer Control GATT 骨架，验证 Android 系统级 HID 枚举。
// 暂不绑定实体按键/手势，也不伪造 PnP VID/PID；后续识别稳定后再补完整 HOGP 配套服务。
static constexpr uint16_t kHidAppearance = 0x03C0U; // Generic Human Interface Device
static constexpr uint8_t kHidConsumerReportId = 1U;
static constexpr uint8_t kHidReportTypeInput = 1U;

static const ble_uuid16_t kHidServiceUuid = { BLE_UUID_TYPE_16, 0x1812 };
static const ble_uuid16_t kHidInformationUuid = { BLE_UUID_TYPE_16, 0x2A4A };
static const ble_uuid16_t kHidReportMapUuid = { BLE_UUID_TYPE_16, 0x2A4B };
static const ble_uuid16_t kHidControlPointUuid = { BLE_UUID_TYPE_16, 0x2A4C };
static const ble_uuid16_t kHidReportUuid = { BLE_UUID_TYPE_16, 0x2A4D };
static const ble_uuid16_t kHidReportReferenceUuid = { BLE_UUID_TYPE_16, 0x2908 };

// HID 1.11 / Country=0 / Normally Connectable；不宣称 Remote Wake。
static const uint8_t kHidInformation[] = { 0x11, 0x01, 0x00, 0x02 };

// Consumer Control：Play/Pause、Next、Previous、Volume+、Volume-，1 字节位图。
static const uint8_t kHidReportMap[] = {
    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage (Consumer Control)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       // Report ID (1)
    0x15, 0x00,       // Logical Minimum (0)
    0x25, 0x01,       // Logical Maximum (1)
    0x09, 0xCD,       // Play/Pause
    0x09, 0xB5,       // Scan Next Track
    0x09, 0xB6,       // Scan Previous Track
    0x09, 0xE9,       // Volume Increment
    0x09, 0xEA,       // Volume Decrement
    0x75, 0x01,       // Report Size (1)
    0x95, 0x05,       // Report Count (5)
    0x81, 0x02,       // Input (Data, Variable, Absolute)
    0x75, 0x03,       // 3-bit padding
    0x95, 0x01,
    0x81, 0x03,       // Input (Constant)
    0xC0,             // End Collection
};

static const uint8_t kHidInputReportReference[] = {
    kHidConsumerReportId,
    kHidReportTypeInput,
};

static uint8_t g_hid_input_report = 0U;
static uint16_t g_hid_input_val_handle = 0U;
static struct ble_gatt_dsc_def g_hid_input_dscs[2] = {};
static struct ble_gatt_chr_def g_hid_chrs[5] = {};
static struct ble_gatt_svc_def g_hid_svcs[2] = {};
#endif

static bool tick_due(TickType_t now, TickType_t due)
{
    return static_cast<int32_t>(now - due) >= 0;
}

static void set_state_locked(BleRemoteState state, esp_err_t error = ESP_OK)
{
    g_state = state;
    g_last_error = error;
    g_state_since_tick = xTaskGetTickCount();
}

static bool desired_enabled()
{
    portENTER_CRITICAL(&g_lock);
    const bool enabled = g_desired_enabled;
    portEXIT_CRITICAL(&g_lock);
    return enabled;
}

#if FAKEPOD_BLE_FOUNDATION_ENABLED

static int hid_append_read_value(struct ble_gatt_access_ctxt *ctxt, const void *data, size_t size)
{
    if (ctxt == nullptr || ctxt->om == nullptr || data == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, data, size) == 0
        ? 0
        : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_information_access(
    uint16_t,
    uint16_t,
    struct ble_gatt_access_ctxt *ctxt,
    void *)
{
    if (ctxt == nullptr || ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return hid_append_read_value(ctxt, kHidInformation, sizeof(kHidInformation));
}

static int hid_report_map_access(
    uint16_t,
    uint16_t,
    struct ble_gatt_access_ctxt *ctxt,
    void *)
{
    if (ctxt == nullptr || ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return hid_append_read_value(ctxt, kHidReportMap, sizeof(kHidReportMap));
}

static int hid_control_point_access(
    uint16_t,
    uint16_t,
    struct ble_gatt_access_ctxt *ctxt,
    void *)
{
    if (ctxt == nullptr || ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || ctxt->om == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (OS_MBUF_PKTLEN(ctxt->om) != 1U) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t command = 0U;
    if (os_mbuf_copydata(ctxt->om, 0, sizeof(command), &command) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (command > 1U) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    // HID Control Point：0=Suspend，1=Exit Suspend；当前阶段没有主动 Report TX。
    return 0;
}

static int hid_input_report_access(
    uint16_t,
    uint16_t,
    struct ble_gatt_access_ctxt *ctxt,
    void *)
{
    if (ctxt == nullptr || ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return hid_append_read_value(ctxt, &g_hid_input_report, sizeof(g_hid_input_report));
}

static int hid_report_reference_access(
    uint16_t,
    uint16_t,
    struct ble_gatt_access_ctxt *ctxt,
    void *)
{
    if (ctxt == nullptr || ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return hid_append_read_value(
        ctxt,
        kHidInputReportReference,
        sizeof(kHidInputReportReference));
}

static void init_hid_gatt_defs()
{
    memset(g_hid_input_dscs, 0, sizeof(g_hid_input_dscs));
    memset(g_hid_chrs, 0, sizeof(g_hid_chrs));
    memset(g_hid_svcs, 0, sizeof(g_hid_svcs));

    g_hid_input_report = 0U;
    g_hid_input_val_handle = 0U;

    g_hid_input_dscs[0].uuid = &kHidReportReferenceUuid.u;
    g_hid_input_dscs[0].att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC;
    g_hid_input_dscs[0].access_cb = hid_report_reference_access;

    g_hid_chrs[0].uuid = &kHidInformationUuid.u;
    g_hid_chrs[0].access_cb = hid_information_access;
    g_hid_chrs[0].flags = BLE_GATT_CHR_F_READ;

    g_hid_chrs[1].uuid = &kHidReportMapUuid.u;
    g_hid_chrs[1].access_cb = hid_report_map_access;
    g_hid_chrs[1].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC;

    g_hid_chrs[2].uuid = &kHidControlPointUuid.u;
    g_hid_chrs[2].access_cb = hid_control_point_access;
    g_hid_chrs[2].flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC;

    g_hid_chrs[3].uuid = &kHidReportUuid.u;
    g_hid_chrs[3].access_cb = hid_input_report_access;
    g_hid_chrs[3].descriptors = g_hid_input_dscs;
    g_hid_chrs[3].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC;
    g_hid_chrs[3].val_handle = &g_hid_input_val_handle;

    g_hid_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_hid_svcs[0].uuid = &kHidServiceUuid.u;
    g_hid_svcs[0].characteristics = g_hid_chrs;
}

static int register_hid_service()
{
    init_hid_gatt_defs();

    int rc = ble_gatts_count_cfg(g_hid_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(g_hid_svcs);
    return rc;
}

static bool should_advertise()
{
    portENTER_CRITICAL(&g_lock);
    const bool allowed = g_desired_enabled && g_state != BleRemoteState::Stopping;
    portEXIT_CRITICAL(&g_lock);
    return allowed;
}

static int start_advertising();

static int gap_event_cb(struct ble_gap_event *event, void *)
{
    if (event == nullptr) return 0;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                portENTER_CRITICAL(&g_lock);
                g_connected = true;
                g_conn_handle = event->connect.conn_handle;
                set_state_locked(BleRemoteState::Connected);
                portEXIT_CRITICAL(&g_lock);
                ESP_LOGI(TAG, "手机已连接：handle=%u", static_cast<unsigned>(event->connect.conn_handle));
            } else {
                ESP_LOGW(TAG, "BLE连接尝试失败：status=%d；继续广播", event->connect.status);
                if (should_advertise()) (void)start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            portENTER_CRITICAL(&g_lock);
            g_connected = false;
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (g_desired_enabled && g_state != BleRemoteState::Stopping) {
                set_state_locked(BleRemoteState::Advertising);
            }
            portEXIT_CRITICAL(&g_lock);
            ESP_LOGI(TAG, "BLE连接已断开：reason=%d", event->disconnect.reason);
            if (should_advertise()) (void)start_advertising();
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            if (should_advertise()) (void)start_advertising();
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE:
            // 成功加密属于正常路径，不再刷诊断日志；失败仍保留告警。
            if (event->enc_change.status != 0) {
                ESP_LOGW(TAG, "BLE加密失败：handle=%u status=%d",
                    static_cast<unsigned>(event->enc_change.conn_handle),
                    event->enc_change.status);
            }
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            struct ble_gap_conn_desc desc = {};
            const int find_rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
            if (find_rc != 0) {
                ESP_LOGW(TAG, "BLE重复配对读取连接失败：handle=%u rc=%d",
                    static_cast<unsigned>(event->repeat_pairing.conn_handle),
                    find_rc);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }

            // 手机删除配对后会丢失自己的 LTK；设备端仍保留旧 Bond 时，NimBLE 会报告重复配对。
            // 只删除当前 peer 的旧 Bond，并让同一条连接继续执行新的 SMP 配对。
            const int delete_rc = ble_store_util_delete_peer(&desc.peer_id_addr);
            if (delete_rc != 0) {
                ESP_LOGW(TAG, "BLE重复配对删除旧Bond失败：handle=%u rc=%d",
                    static_cast<unsigned>(event->repeat_pairing.conn_handle),
                    delete_rc);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }

            ESP_LOGI(TAG, "BLE重复配对：旧Bond已删除，继续重新配对：handle=%u",
                static_cast<unsigned>(event->repeat_pairing.conn_handle));
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == g_hid_input_val_handle) {
                ESP_LOGI(TAG, "BLE HID订阅：Consumer Control notify=%u",
                    static_cast<unsigned>(event->subscribe.cur_notify));
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "BLE MTU更新：handle=%u mtu=%u",
                static_cast<unsigned>(event->mtu.conn_handle),
                static_cast<unsigned>(event->mtu.value));
            return 0;

        default:
            return 0;
    }
}

static int start_advertising()
{
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<uint8_t *>(const_cast<char *>(kDeviceName));
    fields.name_len = strlen(kDeviceName);
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.tx_pwr_lvl_is_present = 1;
    fields.appearance = kHidAppearance;
    fields.appearance_is_present = 1;
    fields.uuids16 = &kHidServiceUuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // R46.0 只验证控制链路，不追求快速发现；500ms 广播降低后台负载和功耗。
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(500);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(510);

    rc = ble_gap_adv_start(
        g_own_addr_type,
        nullptr,
        BLE_HS_FOREVER,
        &params,
        gap_event_cb,
        nullptr);
    if (rc == BLE_HS_EALREADY) rc = 0;
    if (rc == 0) {
        portENTER_CRITICAL(&g_lock);
        if (!g_connected && g_desired_enabled && g_state != BleRemoteState::Stopping) {
            set_state_locked(BleRemoteState::Advertising);
        }
        portEXIT_CRITICAL(&g_lock);
    }
    return rc;
}

static void host_reset_cb(int reason)
{
    ESP_LOGW(TAG, "NimBLE Host reset：reason=%d", reason);
    portENTER_CRITICAL(&g_lock);
    if (g_desired_enabled && g_state != BleRemoteState::Stopping) {
        set_state_locked(BleRemoteState::RetryWait, ESP_FAIL);
        g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
    }
    portEXIT_CRITICAL(&g_lock);
}

static void log_bond_store_status()
{
#if defined(CONFIG_BT_NIMBLE_NVS_PERSIST) && CONFIG_BT_NIMBLE_NVS_PERSIST
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS] = {};
    int peer_count = 0;
    const int rc = ble_store_util_bonded_peers(
        peers,
        &peer_count,
        CONFIG_BT_NIMBLE_MAX_BONDS);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE Bond存储：NVS=ON peers=%d", peer_count);
    } else {
        ESP_LOGW(TAG, "BLE Bond存储读取失败：NVS=ON rc=%d", rc);
    }
#else
    ESP_LOGW(TAG, "BLE Bond存储：NVS=OFF");
#endif
}

static void host_sync_cb()
{
    log_bond_store_status();

    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc == 0) rc = start_advertising();

    if (rc != 0) {
        ESP_LOGE(TAG, "BLE同步后启动广播失败：rc=%d", rc);
        portENTER_CRITICAL(&g_lock);
        if (g_desired_enabled) {
            set_state_locked(BleRemoteState::RetryWait, ESP_FAIL);
            g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
        }
        portEXIT_CRITICAL(&g_lock);
        return;
    }

    ESP_LOGI(TAG, "BLE广播已启动：name=%s interval≈500ms", kDeviceName);
}

static void host_task(void *)
{
    ESP_LOGI(TAG, "NimBLE HostTask启动：Core=%d", xPortGetCoreID());
    nimble_port_run();
    // 官方 FreeRTOS port 要求 HostTask 在 nimble_port_run() 返回后完成自身反初始化。
    nimble_port_freertos_deinit();
}

static esp_err_t stop_stack()
{
    portENTER_CRITICAL(&g_lock);
    const bool initialized = g_stack_initialized;
    portEXIT_CRITICAL(&g_lock);
    if (!initialized) return ESP_OK;

    // nimble_port_stop() 自己会停止 GAP procedure 并终止现有连接。
    // 这里不能先手动 terminate，否则 stop 内部再次终止会命中 BLE_HS_EALREADY。
    const int stop_rc = nimble_port_stop();
    if (stop_rc != 0) {
        ESP_LOGW(TAG, "NimBLE停止失败：rc=%d", stop_rc);
        return ESP_FAIL;
    }

    // ESP-IDF 5.x 官方流程：stop 后 deinit；不额外调用不可逆的 bt_mem_release。
    nimble_port_deinit();

    portENTER_CRITICAL(&g_lock);
    g_stack_initialized = false;
    g_connected = false;
    g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    portEXIT_CRITICAL(&g_lock);
    return ESP_OK;
}

static esp_err_t start_stack()
{
    const int init_rc = nimble_port_init();
    if (init_rc != ESP_OK) {
        ESP_LOGW(TAG, "NimBLE初始化失败：rc=%d", init_rc);
        return init_rc == ESP_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    portENTER_CRITICAL(&g_lock);
    g_stack_initialized = true;
    g_connected = false;
    g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    portEXIT_CRITICAL(&g_lock);

    ble_hs_cfg.reset_cb = host_reset_cb;
    ble_hs_cfg.sync_cb = host_sync_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    const int appearance_rc = ble_svc_gap_device_appearance_set(kHidAppearance);
    if (appearance_rc != 0) {
        ESP_LOGW(TAG, "BLE HID Appearance设置失败：rc=%d", appearance_rc);
        (void)stop_stack();
        return ESP_FAIL;
    }

    const int hid_rc = register_hid_service();
    if (hid_rc != 0) {
        ESP_LOGW(TAG, "BLE HID服务注册失败：rc=%d", hid_rc);
        (void)stop_stack();
        return ESP_FAIL;
    }

    const int name_rc = ble_svc_gap_device_name_set(kDeviceName);
    if (name_rc != 0) {
        ESP_LOGW(TAG, "BLE设备名设置失败：rc=%d", name_rc);
        (void)stop_stack();
        return ESP_FAIL;
    }

    // NimBLE 安全存储接入默认 NVS，使 Bond/LTK 在设备重启后仍可恢复。
    ble_store_config_init();

    ESP_LOGI(TAG, "BLE HID Consumer Control已注册：ReportID=%u",
        static_cast<unsigned>(kHidConsumerReportId));
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

#endif // FAKEPOD_BLE_FOUNDATION_ENABLED

static void transition_task(void *)
{
#if FAKEPOD_BLE_FOUNDATION_ENABLED
    esp_err_t ret = ESP_OK;
    const bool enable = desired_enabled();

    portENTER_CRITICAL(&g_lock);
    const bool stack_initialized = g_stack_initialized;
    portEXIT_CRITICAL(&g_lock);

    if (!enable) {
        ret = stop_stack();
        portENTER_CRITICAL(&g_lock);
        if (ret == ESP_OK) {
            set_state_locked(BleRemoteState::Disabled);
        } else {
            set_state_locked(BleRemoteState::RetryWait, ret);
            g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
        }
        g_transition_running = false;
        portEXIT_CRITICAL(&g_lock);
        vTaskDelete(nullptr);
        return;
    }

    // RetryWait 可能来自 Host reset/广播失败；先完整收掉旧栈，再重新初始化。
    if (stack_initialized) {
        ret = stop_stack();
    }
    if (ret == ESP_OK && desired_enabled()) {
        portENTER_CRITICAL(&g_lock);
        set_state_locked(BleRemoteState::Starting);
        portEXIT_CRITICAL(&g_lock);
        ret = start_stack();
    }

    portENTER_CRITICAL(&g_lock);
    if (ret != ESP_OK) {
        set_state_locked(BleRemoteState::RetryWait, ret);
        g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
    } else if (!g_desired_enabled) {
        // 用户在启动过程中又关闭；下一轮 update() 会立刻进入 stop。
        set_state_locked(BleRemoteState::Stopping);
    }
    g_transition_running = false;
    portEXIT_CRITICAL(&g_lock);
#else
    portENTER_CRITICAL(&g_lock);
    set_state_locked(BleRemoteState::Unsupported, ESP_ERR_NOT_SUPPORTED);
    g_transition_running = false;
    portEXIT_CRITICAL(&g_lock);
#endif
    vTaskDelete(nullptr);
}

static bool start_transition_task(BleRemoteState state)
{
    portENTER_CRITICAL(&g_lock);
    if (g_transition_running) {
        portEXIT_CRITICAL(&g_lock);
        return true;
    }
    g_transition_running = true;
    set_state_locked(state);
    portEXIT_CRITICAL(&g_lock);

    const BaseType_t created = xTaskCreatePinnedToCore(
        transition_task,
        "ble_ctl",
        kTransitionTaskStackBytes,
        nullptr,
        kTransitionTaskPriority,
        nullptr,
        kTransitionTaskCore);
    if (created == pdPASS) return true;

    portENTER_CRITICAL(&g_lock);
    g_transition_running = false;
    set_state_locked(BleRemoteState::RetryWait, ESP_ERR_NO_MEM);
    g_retry_due_tick = xTaskGetTickCount() + kRetryDelay;
    portEXIT_CRITICAL(&g_lock);
    ESP_LOGW(TAG, "BLE控制任务创建失败：NO_MEM；2秒后重试");
    return false;
}

} // namespace

esp_err_t ble_remote_service_init()
{
    portENTER_CRITICAL(&g_lock);
    if (g_ready) {
        const BleRemoteState state = g_state;
        portEXIT_CRITICAL(&g_lock);
        return state == BleRemoteState::Unsupported ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
    }
    g_ready = true;
    g_desired_enabled = false;
    g_stack_initialized = false;
    g_connected = false;
    g_transition_running = false;
    g_last_error = ESP_OK;
    g_retry_due_tick = 0;
#if FAKEPOD_BLE_FOUNDATION_ENABLED
    set_state_locked(BleRemoteState::Disabled);
#else
    set_state_locked(BleRemoteState::Unsupported, ESP_ERR_NOT_SUPPORTED);
#endif
    const BleRemoteState state = g_state;
    portEXIT_CRITICAL(&g_lock);

#if FAKEPOD_BLE_FOUNDATION_ENABLED
    (void)state;
    ESP_LOGI(TAG, "BLE Foundation就绪：NimBLE Peripheral，默认关闭");
    return ESP_OK;
#else
    ESP_LOGW(TAG, "BLE Foundation代码已就绪，但 sdkconfig 尚未完整启用 NimBLE Peripheral/Broadcaster/GATT Server/GAP Service");
    return state == BleRemoteState::Unsupported ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
#endif
}

void ble_remote_service_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&g_lock);
    if (!g_ready) {
        portEXIT_CRITICAL(&g_lock);
        return;
    }
    g_desired_enabled = enabled;
    g_retry_due_tick = 0;
    portEXIT_CRITICAL(&g_lock);
    ESP_LOGI(TAG, "BLE用户意图：%s", enabled ? "开启" : "关闭");
}

void ble_remote_service_update()
{
    portENTER_CRITICAL(&g_lock);
    if (!g_ready || g_transition_running) {
        portEXIT_CRITICAL(&g_lock);
        return;
    }
    const bool desired = g_desired_enabled;
    BleRemoteState state = g_state;
    const TickType_t retry_due = g_retry_due_tick;
    const TickType_t state_since = g_state_since_tick;
    portEXIT_CRITICAL(&g_lock);

    if (state == BleRemoteState::Unsupported) return;

    const TickType_t now = xTaskGetTickCount();
    if (desired) {
        if (state == BleRemoteState::Advertising || state == BleRemoteState::Connected) return;
        if (state == BleRemoteState::Stopping) return;
        if (state == BleRemoteState::Starting && !tick_due(now, state_since + kStartTimeout)) return;
        if (state == BleRemoteState::Starting) {
            ESP_LOGW(TAG, "BLE启动5秒仍未同步，执行完整重启");
            portENTER_CRITICAL(&g_lock);
            set_state_locked(BleRemoteState::RetryWait, ESP_ERR_TIMEOUT);
            g_retry_due_tick = now;
            portEXIT_CRITICAL(&g_lock);
            state = BleRemoteState::RetryWait;
        }
        if (state == BleRemoteState::RetryWait && retry_due != 0 && !tick_due(now, retry_due)) return;
        (void)start_transition_task(BleRemoteState::Starting);
        return;
    }

    if (state == BleRemoteState::Disabled) return;
    if (state == BleRemoteState::RetryWait && retry_due != 0 && !tick_due(now, retry_due)) return;
    (void)start_transition_task(BleRemoteState::Stopping);
}

bool ble_remote_service_get_snapshot(BleRemoteSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    portENTER_CRITICAL(&g_lock);
    out_snapshot->ready = g_ready;
    out_snapshot->desired_enabled = g_desired_enabled;
    out_snapshot->stack_initialized = g_stack_initialized;
    out_snapshot->connected = g_connected;
    out_snapshot->state = g_state;
    out_snapshot->last_error = g_last_error;
    portEXIT_CRITICAL(&g_lock);
    return out_snapshot->ready;
}

const char *ble_remote_service_state_name(BleRemoteState state)
{
    switch (state) {
        case BleRemoteState::Disabled: return "关闭";
        case BleRemoteState::Starting: return "启动中";
        case BleRemoteState::Advertising: return "广播中";
        case BleRemoteState::Connected: return "已连接";
        case BleRemoteState::Stopping: return "关闭中";
        case BleRemoteState::RetryWait: return "故障重试";
        case BleRemoteState::Unsupported: return "未启用NimBLE";
        default: return "未知";
    }
}
