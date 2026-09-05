#include "storage_io.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "app_diag_config.h"

static const char *TAG = "存储协调";

static SemaphoreHandle_t g_sd_mutex = nullptr;
static volatile bool g_usb_handoff_blocked = false;
static TaskHandle_t g_usb_handoff_owner = nullptr;

esp_err_t storage_io_init()
{
    if (g_sd_mutex != nullptr) {
        return ESP_OK;
    }

    g_sd_mutex = xSemaphoreCreateRecursiveMutex();
    if (g_sd_mutex == nullptr) {
        ESP_LOGE(TAG, "创建全局 SD 递归互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(TAG, "全局 SD 递归互斥锁已就绪；音频/封面/扫描/索引统一串行访问 TF 卡");
#endif
    return ESP_OK;
}

bool storage_io_is_ready()
{
    return g_sd_mutex != nullptr;
}

bool storage_sd_lock(TickType_t timeout_ticks)
{
    if (g_sd_mutex == nullptr) {
        return false;
    }

    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (g_usb_handoff_blocked && current != g_usb_handoff_owner) {
        return false;
    }

    if (xSemaphoreTakeRecursive(g_sd_mutex, timeout_ticks) != pdTRUE) {
        return false;
    }

    // begin_usb_handoff() 可能在本任务等待 mutex 期间建立封锁；拿到锁后必须再检查一次，
    // 防止“先通过检查、后排队”的旧访问穿过 USB owner 边界。
    if (g_usb_handoff_blocked && current != g_usb_handoff_owner) {
        xSemaphoreGiveRecursive(g_sd_mutex);
        return false;
    }
    return true;
}

void storage_sd_unlock()
{
    if (g_sd_mutex != nullptr) {
        xSemaphoreGiveRecursive(g_sd_mutex);
    }
}

bool storage_io_begin_usb_handoff(TickType_t timeout_ticks)
{
    if (g_sd_mutex == nullptr || g_usb_handoff_blocked) {
        return false;
    }
    if (xSemaphoreTakeRecursive(g_sd_mutex, timeout_ticks) != pdTRUE) {
        return false;
    }

    // 此时所有已经进入 SD 临界区的任务都已经退出。先记录 owner，再发布 blocked。
    g_usb_handoff_owner = xTaskGetCurrentTaskHandle();
    g_usb_handoff_blocked = true;
    ESP_LOGI(TAG, "USB MSC接管闸门已关闭：普通TF访问停止");
    return true;
}

void storage_io_finish_usb_handoff(bool keep_blocked)
{
    if (g_sd_mutex == nullptr || g_usb_handoff_owner != xTaskGetCurrentTaskHandle()) {
        return;
    }

    if (keep_blocked) {
        // raw SDMMC / TinyUSB 不经过 VFS StorageSdLock；清掉 owner 后继续拒绝所有应用侧访问。
        g_usb_handoff_owner = nullptr;
    } else {
        g_usb_handoff_blocked = false;
        g_usb_handoff_owner = nullptr;
        ESP_LOGI(TAG, "USB MSC接管已取消：恢复普通TF访问");
    }
    xSemaphoreGiveRecursive(g_sd_mutex);
}

bool storage_io_usb_handoff_blocked()
{
    return g_usb_handoff_blocked;
}

bool storage_io_begin_usb_return(TickType_t timeout_ticks)
{
    if (g_sd_mutex == nullptr || !g_usb_handoff_blocked || g_usb_handoff_owner != nullptr) {
        return false;
    }
    if (xSemaphoreTakeRecursive(g_sd_mutex, timeout_ticks) != pdTRUE) {
        return false;
    }

    // MSC raw owner 已经停止；这里重新指定一个临时 owner，让 sdcard_init() 能递归进入
    // StorageSdLock，同时其他任务仍被 blocked 拒绝。
    if (!g_usb_handoff_blocked || g_usb_handoff_owner != nullptr) {
        xSemaphoreGiveRecursive(g_sd_mutex);
        return false;
    }
    g_usb_handoff_owner = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "USB MSC归还闸门已接管：开始恢复普通TF挂载");
    return true;
}

void storage_io_finish_usb_return(bool app_storage_ready)
{
    if (g_sd_mutex == nullptr || g_usb_handoff_owner != xTaskGetCurrentTaskHandle()) {
        return;
    }

    if (app_storage_ready) {
        g_usb_handoff_blocked = false;
        ESP_LOGI(TAG, "USB MSC归还完成：恢复普通TF访问");
    } else {
        ESP_LOGE(TAG, "USB MSC归还未完成：继续封锁普通TF访问");
    }
    g_usb_handoff_owner = nullptr;
    xSemaphoreGiveRecursive(g_sd_mutex);
}
