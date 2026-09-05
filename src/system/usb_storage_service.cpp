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
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_msc.h"
#include "tusb.h"

#include "board_pins.h"

namespace {

static const char *TAG = "USB存储";

enum class TinyUsbProfile : uint8_t {
    None = 0,
    CompositeMscCdc,
    CdcOnly,
};

static sdmmc_card_t *g_card = nullptr;
static sdmmc_host_t g_host = {};
static bool g_host_initialized = false;
static tinyusb_msc_storage_handle_t g_storage = nullptr;
static bool g_tinyusb_installed = false;
static TinyUsbProfile g_tinyusb_profile = TinyUsbProfile::None;
static bool g_cdc_initialized = false;
static bool g_console_redirected = false;
static bool g_active = false;
static volatile bool g_runtime_transitioning = false;
static volatile bool g_host_seen = false;
static volatile bool g_host_attached = false;
static volatile bool g_host_ejected = false;
static volatile bool g_auto_return_requested = false;
static TickType_t g_service_start_tick = 0;
static TickType_t g_host_release_tick = 0;

// V3.5: MSC 退出后仍留在 USB-OTG/TinyUSB，不再尝试回切内建 USB Serial/JTAG。
// 重新枚举时使用 CDC-only configuration descriptor，把 Windows 中残留的灰色磁盘接口彻底移除。
// CDC 保持 interface 0/1 和原默认端点 0x81/0x02/0x82，尽量让 Windows 沿用原 COM 号。
enum : uint8_t {
    kCdcControlInterface = 0,
    kCdcDataInterface,
    kCdcInterfaceCount,
};

static constexpr uint16_t kCdcOnlyConfigLen = TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN;
static const uint8_t k_cdc_only_fs_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        kCdcInterfaceCount,
        0,
        kCdcOnlyConfigLen,
        0,
        100),
    TUD_CDC_DESCRIPTOR(
        kCdcControlInterface,
        4,
        0x81,
        8,
        0x02,
        0x82,
        64),
};

static_assert(
    sizeof(k_cdc_only_fs_descriptor) == kCdcOnlyConfigLen,
    "CDC-only USB descriptor length mismatch");

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
            // 实机已确认 Windows “安全弹出”最终会把 MSC storage 切回 APP owner。
            // V3.5 以这条 storage-owner 事件作为唯一自动归还触发。
            if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
                g_host_seen = true;
                g_host_attached = false;
                g_host_ejected = true;
                g_host_release_tick = xTaskGetTickCount();
                // 实机 Windows 安全弹出已确认会走到 owner=APP。只用这个 storage-owner
                // 事件触发自动归还；不要用早期 USB DETACHED，后者在首次枚举时也会抖动。
                if (g_active) g_auto_return_requested = true;
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

static esp_err_t ensure_tinyusb_cdc_console()
{
    if (!g_tinyusb_installed) return ESP_ERR_INVALID_STATE;

    if (!g_cdc_initialized) {
        tinyusb_config_cdcacm_t cdc_cfg = {};
        cdc_cfg.cdc_port = TINYUSB_CDC_ACM_0;

        const esp_err_t cdc_ret = tinyusb_cdcacm_init(&cdc_cfg);
        if (cdc_ret != ESP_OK) {
            ESP_LOGE(TAG, "TinyUSB CDC初始化失败：%s", esp_err_to_name(cdc_ret));
            return cdc_ret;
        }
        g_cdc_initialized = true;
    }

    if (!g_console_redirected) {
        const esp_err_t console_ret = tinyusb_console_init(TINYUSB_CDC_ACM_0);
        if (console_ret != ESP_OK) {
            ESP_LOGE(TAG, "TinyUSB CDC控制台重定向失败：%s", esp_err_to_name(console_ret));
            return console_ret;
        }
        g_console_redirected = true;
    }

    return ESP_OK;
}

static esp_err_t reinstall_tinyusb_profile(TinyUsbProfile target_profile)
{
    if (target_profile == TinyUsbProfile::None) return ESP_ERR_INVALID_ARG;

    if (g_tinyusb_installed) {
        // console_deinit 必须先于 CDC/Device Stack；否则 stdout/stderr 仍指向即将销毁的 VFS。
        if (g_console_redirected) {
            const esp_err_t console_ret = tinyusb_console_deinit(TINYUSB_CDC_ACM_0);
            if (console_ret != ESP_OK) {
                ESP_LOGW(TAG, "TinyUSB CDC控制台解除重定向失败：%s；继续重枚举",
                    esp_err_to_name(console_ret));
            }
            g_console_redirected = false;
        }

        if (g_cdc_initialized) {
            const esp_err_t cdc_ret = tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
            if (cdc_ret != ESP_OK) {
                ESP_LOGW(TAG, "TinyUSB CDC反初始化失败：%s；继续重枚举",
                    esp_err_to_name(cdc_ret));
            }
            g_cdc_initialized = false;
        }

        const esp_err_t uninstall_ret = tinyusb_driver_uninstall();
        if (uninstall_ret != ESP_OK) {
            ESP_LOGE(TAG, "TinyUSB设备卸载失败：%s", esp_err_to_name(uninstall_ret));
            return uninstall_ret;
        }
        g_tinyusb_installed = false;
        g_tinyusb_profile = TinyUsbProfile::None;

        // 给 Windows 一个明确的断开窗口，保证旧 configuration interfaces 被真正撤销。
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(tinyusb_device_event_cb);
    if (target_profile == TinyUsbProfile::CdcOnly) {
        tusb_cfg.descriptor.full_speed_config = k_cdc_only_fs_descriptor;
    }

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB %s安装失败：%s",
            target_profile == TinyUsbProfile::CdcOnly ? "CDC-only" : "MSC+CDC",
            esp_err_to_name(ret));
        return ret;
    }
    g_tinyusb_installed = true;
    g_tinyusb_profile = target_profile;

    ret = ensure_tinyusb_cdc_console();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB %s控制台建立失败：%s",
            target_profile == TinyUsbProfile::CdcOnly ? "CDC-only" : "MSC+CDC",
            esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "USB已重枚举为%s",
        target_profile == TinyUsbProfile::CdcOnly ? "CDC-only（无磁盘接口）" : "MSC+CDC");
    return ESP_OK;
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

esp_err_t usb_storage_service_start()
{
    if (g_active) return ESP_OK;

    g_host_seen = false;
    g_host_attached = false;
    g_host_ejected = false;
    g_auto_return_requested = false;
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

    if (!g_tinyusb_installed || g_tinyusb_profile != TinyUsbProfile::CompositeMscCdc) {
        // 首次进入时从内建 USB Serial/JTAG 切到 TinyUSB；后续再次进入时从
        // CDC-only 重新枚举回 MSC+CDC，让同一个 TF 文件管理流程可以反复使用。
        ret = reinstall_tinyusb_profile(TinyUsbProfile::CompositeMscCdc);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "TinyUSB MSC+CDC建立失败：%s", esp_err_to_name(ret));
            (void)tinyusb_msc_delete_storage(g_storage);
            g_storage = nullptr;
            raw_sd_cleanup();
            return ret;
        }
    }

    // reinstall_tinyusb_profile() 已保证 CDC console 在线；保留一次幂等检查，
    // 兼容未来从已安装 composite profile 直接建立新的 MSC storage。
    ret = ensure_tinyusb_cdc_console();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB CDC控制台建立失败：%s", esp_err_to_name(ret));
        (void)tinyusb_msc_delete_storage(g_storage);
        g_storage = nullptr;
        raw_sd_cleanup();
        return ret;
    }

    g_active = true;
    ESP_LOGI(TAG, "FakePod USB服务已启动：TF=MSC，日志串口=TinyUSB CDC；应用侧不会挂载/访问TF卡");
    return ESP_OK;
}

esp_err_t usb_storage_service_stop()
{
    if (!g_active && g_storage == nullptr && g_card == nullptr && !g_host_initialized) {
        // CDC-only profile 可以跨 MSC 会话常驻；它不属于“MSC仍需停止”的条件。
        return ESP_OK;
    }

    esp_err_t ret = delete_storage_after_host_release();
    if (ret != ESP_OK) return ret;

    // storage/LUN 已经安全删除，此刻先释放 raw SDMMC，确保下一步普通 VFS 可以重新接管。
    raw_sd_cleanup();
    g_active = false;
    g_auto_return_requested = false;

    // V3.5：仍留在 TinyUSB/USB-OTG，但从 MSC+CDC 重新枚举成 CDC-only。
    // 这不会回切 USB Serial/JTAG；只会让 COM 短暂断开后重新出现，同时灰色盘符消失。
    const esp_err_t usb_ret = reinstall_tinyusb_profile(TinyUsbProfile::CdcOnly);
    if (usb_ret != ESP_OK) {
        // TF owner 已经安全释放，不能把失败伪装成“MSC仍活动”。允许上层继续恢复 /sdcard；
        // 串口若未重新枚举，下一次整机重启仍可恢复正常 USB Serial/JTAG。
        ESP_LOGE(TAG, "CDC-only重枚举失败：%s；TF仍可归还FakePod，串口需重启恢复",
            esp_err_to_name(usb_ret));
    }

    g_host_seen = false;
    g_host_attached = false;
    g_host_ejected = false;
    g_service_start_tick = 0;
    g_host_release_tick = 0;
    ESP_LOGI(TAG, "USB MSC服务已停止：raw SDMMC已释放，USB目标=CDC-only，等待普通VFS重新挂载");
    return ESP_OK;
}

bool usb_storage_service_is_active()
{
    return g_active;
}

bool usb_storage_service_auto_return_requested()
{
    return g_active && !g_runtime_transitioning && g_auto_return_requested;
}

bool usb_storage_service_host_safe_to_return()
{
    if (!g_active || g_runtime_transitioning) return false;

    // V3.5 实机确认：Windows 安全弹出最终会产生 MSC MOUNT_COMPLETE owner=APP。
    // 首次 TinyUSB 枚举也可能出现 DETACHED，因此不再把 device detach 单独当作自动安全信号。
    if (g_auto_return_requested && g_host_release_tick != 0 &&
        xTaskGetTickCount() - g_host_release_tick >= pdMS_TO_TICKS(250)) {
        return true;
    }

    // 如果根本没有电脑枚举过设备，1.5 秒后仍允许手动恢复。
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

bool usb_storage_service_begin_runtime_return(bool user_confirmed_eject)
{
    if (!g_active || g_runtime_transitioning) return false;

    const bool host_reported_safe = usb_storage_service_host_safe_to_return();
    if (!host_reported_safe && !user_confirmed_eject) return false;

    if (!host_reported_safe && user_confirmed_eject) {
        // 手动按钮仍作为兜底；正常 V3.5 流程由 owner=APP 自动触发。
        ESP_LOGW(TAG, "使用用户确认执行MSC热归还：未收到owner=APP，确认电脑已安全弹出");
    }

    g_auto_return_requested = false;
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
