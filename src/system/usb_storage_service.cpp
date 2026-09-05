#include "usb_storage_service.h"

#include <stdint.h>
#include <stdlib.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include "board_pins.h"

namespace {

static const char *TAG = "USB存储";

static sdmmc_card_t *g_card = nullptr;
static sdmmc_host_t g_host = {};
static bool g_host_initialized = false;
static tinyusb_msc_storage_handle_t g_storage = nullptr;
static bool g_tinyusb_installed = false;
static bool g_active = false;
static volatile bool g_runtime_transitioning = false;
static volatile bool g_host_seen = false;
static volatile bool g_host_attached = false;
static volatile bool g_host_ejected = false;
static TickType_t g_service_start_tick = 0;
static TickType_t g_host_release_tick = 0;

static void raw_sd_cleanup()
{
    if (g_card != nullptr) {
        free(g_card);
        g_card = nullptr;
    }

    if (g_host_initialized) {
        if ((g_host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) != 0 && g_host.deinit_p != nullptr) {
            (void)g_host.deinit_p(g_host.slot);
        } else if (g_host.deinit != nullptr) {
            (void)g_host.deinit();
        }
        g_host_initialized = false;
    }
}

static esp_err_t raw_sd_init()
{
    g_host = SDMMC_HOST_DEFAULT();
    g_host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = static_cast<gpio_num_t>(FAKEPOD_SD_CLK);
    slot.cmd = static_cast<gpio_num_t>(FAKEPOD_SD_CMD);
    slot.d0 = static_cast<gpio_num_t>(FAKEPOD_SD_D0);
    slot.d1 = static_cast<gpio_num_t>(FAKEPOD_SD_D1);
    slot.d2 = static_cast<gpio_num_t>(FAKEPOD_SD_D2);
    slot.d3 = static_cast<gpio_num_t>(FAKEPOD_SD_D3);

    esp_err_t ret = g_host.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC Host初始化失败：%s", esp_err_to_name(ret));
        return ret;
    }
    g_host_initialized = true;

    ret = sdmmc_host_init_slot(g_host.slot, &slot);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC Slot初始化失败：%s", esp_err_to_name(ret));
        raw_sd_cleanup();
        return ret;
    }

    g_card = static_cast<sdmmc_card_t *>(calloc(1U, sizeof(sdmmc_card_t)));
    if (g_card == nullptr) {
        raw_sd_cleanup();
        return ESP_ERR_NO_MEM;
    }

    ret = sdmmc_card_init(&g_host, g_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TF卡初始化失败：%s", esp_err_to_name(ret));
        raw_sd_cleanup();
        return ret;
    }

    const uint64_t bytes =
        static_cast<uint64_t>(g_card->csd.capacity) * g_card->csd.sector_size;
    ESP_LOGI(TAG, "TF卡原始块设备就绪：%llu MB，未挂载到VFS",
        static_cast<unsigned long long>(bytes / 1024ULL / 1024ULL));
    return ESP_OK;
}

static void tinyusb_device_event_cb(tinyusb_event_t *event, void *)
{
    if (event == nullptr) return;

    switch (event->id) {
        case TINYUSB_EVENT_ATTACHED:
            g_host_seen = true;
            g_host_attached = true;
            g_host_ejected = false;
            g_host_release_tick = 0;
            ESP_LOGI(TAG, "USB Host已连接并完成配置");
            break;
        case TINYUSB_EVENT_DETACHED:
            g_host_seen = true;
            g_host_attached = false;
            g_host_ejected = true;
            g_host_release_tick = xTaskGetTickCount();
            ESP_LOGI(TAG, "USB Host已断开：TF卡可安全归还FakePod");
            break;
        default:
            break;
    }
}

static void storage_event_cb(
    tinyusb_msc_storage_handle_t,
    tinyusb_msc_event_t *event,
    void *)
{
    if (event == nullptr) return;

    switch (event->id) {
        case TINYUSB_MSC_EVENT_MOUNT_START:
            ESP_LOGI(TAG, "MSC挂载切换开始");
            break;
        case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
            ESP_LOGI(TAG, "MSC挂载完成：owner=%s",
                event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB" : "APP");
            // Host detach 时 esp_tinyusb 会把 storage 从 USB 归还到 APP mount point。
            // 应用仍受 StorageSdLock USB 闸门保护，不会在 V3 正式 teardown 前访问该 VFS。
            if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
                g_host_seen = true;
                g_host_attached = false;
                g_host_ejected = true;
                g_host_release_tick = xTaskGetTickCount();
            }
            break;
        case TINYUSB_MSC_EVENT_MOUNT_FAILED:
            ESP_LOGE(TAG, "MSC挂载失败");
            break;
        case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
            // FakePod 从不在设备端自动格式化用户 TF 卡。
            ESP_LOGE(TAG, "TF卡文件系统需要格式化；FakePod不会自动格式化");
            break;
        default:
            break;
    }
}

static esp_err_t delete_storage_after_host_release()
{
    if (g_storage == nullptr) return ESP_OK;

    // delete_storage 会拒绝仍有 deferred write 的 storage。Host 已安全弹出后保留 TinyUSB task
    // 最多 1 秒让队尾写入真正落盘，避免为了“快”而在缓存未清空时拆 LUN。
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    for (uint8_t retry = 0; retry < 40U; ++retry) {
        ret = tinyusb_msc_delete_storage(g_storage);
        if (ret == ESP_OK) {
            g_storage = nullptr;
            return ESP_OK;
        }
        if (ret != ESP_ERR_INVALID_STATE) break;
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    ESP_LOGE(TAG, "删除MSC storage失败：%s", esp_err_to_name(ret));
    return ret;
}

} // namespace

// TinyUSB core 对 START STOP UNIT 提供 weak callback；esp_tinyusb 2.x 没有替应用暴露
// “Windows安全弹出”专用事件，因此这里只接管这一条 SCSI 通知。
// load_eject=1,start=0 表示 Host 请求弹出介质；这里只记状态，不在 USB task 回调里做重挂载。
extern "C" bool tud_msc_start_stop_cb(
    uint8_t,
    uint8_t,
    bool start,
    bool load_eject)
{
    if (load_eject && !start) {
        g_host_seen = true;
        g_host_ejected = true;
        g_host_release_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "Host已发送MSC安全弹出命令：允许V3热返回");
    } else if (load_eject && start) {
        g_host_seen = true;
        g_host_attached = true;
        g_host_ejected = false;
        g_host_release_tick = 0;
    }
    return true;
}

esp_err_t usb_storage_service_start()
{
    if (g_active) return ESP_OK;

    g_host_seen = false;
    g_host_attached = false;
    g_host_ejected = false;
    g_service_start_tick = xTaskGetTickCount();
    g_host_release_tick = 0;

    esp_err_t ret = raw_sd_init();
    if (ret != ESP_OK) return ret;

    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    storage_cfg.fat_fs.base_path = nullptr;
    storage_cfg.fat_fs.config.max_files = 5;
    storage_cfg.fat_fs.format_flags = 0;
    storage_cfg.medium.card = g_card;

    ret = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &g_storage);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建SDMMC MSC存储失败：%s", esp_err_to_name(ret));
        raw_sd_cleanup();
        return ret;
    }

    ret = tinyusb_msc_set_storage_callback(storage_event_cb, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册MSC存储回调失败：%s", esp_err_to_name(ret));
        (void)tinyusb_msc_delete_storage(g_storage);
        g_storage = nullptr;
        raw_sd_cleanup();
        return ret;
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(tinyusb_device_event_cb);
    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB Device安装失败：%s", esp_err_to_name(ret));
        (void)tinyusb_msc_delete_storage(g_storage);
        g_storage = nullptr;
        raw_sd_cleanup();
        return ret;
    }

    g_tinyusb_installed = true;
    g_active = true;
    ESP_LOGI(TAG, "FakePod TF卡 USB MSC 服务已启动；应用侧不会挂载/访问TF卡");
    return ESP_OK;
}

esp_err_t usb_storage_service_stop()
{
    if (!g_active && g_storage == nullptr && !g_tinyusb_installed && g_card == nullptr) {
        return ESP_OK;
    }

    esp_err_t ret = delete_storage_after_host_release();
    if (ret != ESP_OK) return ret;

    if (g_tinyusb_installed) {
        ret = tinyusb_driver_uninstall();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "TinyUSB Device卸载失败：%s", esp_err_to_name(ret));
            // storage 已解绑，保留 raw SD owner 和 active 状态，允许用户再次点“恢复”重试卸载。
            return ret;
        }
        g_tinyusb_installed = false;
    }

    raw_sd_cleanup();
    g_active = false;
    g_host_seen = false;
    g_host_attached = false;
    g_host_ejected = false;
    g_service_start_tick = 0;
    g_host_release_tick = 0;
    ESP_LOGI(TAG, "USB MSC服务已停止：raw SDMMC/USB PHY已释放，等待普通VFS重新挂载");
    return ESP_OK;
}

bool usb_storage_service_is_active()
{
    return g_active;
}

bool usb_storage_service_host_safe_to_return()
{
    if (!g_active || g_runtime_transitioning) return false;

    // 安全弹出回调发生在 SCSI START STOP UNIT 处理路径里；给命令状态阶段和 detach/app-mount
    // 收尾留 250ms，再向 UI 开放“恢复”，避免在 Host 刚宣布放手的同一瞬间拆 LUN。
    if ((g_host_ejected || (g_host_seen && !g_host_attached)) && g_host_release_tick != 0 &&
        xTaskGetTickCount() - g_host_release_tick >= pdMS_TO_TICKS(250)) {
        return true;
    }

    // 如果根本没有电脑枚举过设备，1.5 秒后允许用户直接恢复；此时 Host 从未拿到块设备，
    // 不存在待刷新的文件系统写入。
    if (!g_host_seen && g_service_start_tick != 0 &&
        xTaskGetTickCount() - g_service_start_tick >= pdMS_TO_TICKS(1500)) {
        return true;
    }
    return false;
}

bool usb_storage_service_begin_runtime_transition()
{
    if (g_active || g_runtime_transitioning) return false;
    g_runtime_transitioning = true;
    return true;
}

void usb_storage_service_cancel_runtime_transition()
{
    if (!g_active) g_runtime_transitioning = false;
}

void usb_storage_service_finish_runtime_transition()
{
    if (g_active) g_runtime_transitioning = false;
}

bool usb_storage_service_begin_runtime_return()
{
    if (!g_active || g_runtime_transitioning || !usb_storage_service_host_safe_to_return()) {
        return false;
    }
    g_runtime_transitioning = true;
    return true;
}

void usb_storage_service_cancel_runtime_return()
{
    if (g_active) g_runtime_transitioning = false;
}

void usb_storage_service_finish_runtime_return(bool app_storage_ready)
{
    if (app_storage_ready && !g_active) {
        g_runtime_transitioning = false;
    }
    // app_storage_ready=false 时故意保留 transition gate，避免 TF 未恢复却重新放行业务访问。
}

bool usb_storage_service_blocks_normal_runtime()
{
    return g_runtime_transitioning || g_active;
}
