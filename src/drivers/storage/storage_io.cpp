#include "storage_io.h"

#include "freertos/semphr.h"

#include "esp_log.h"
#include "app_diag_config.h"

static const char *TAG = "存储协调";

static SemaphoreHandle_t g_sd_mutex = nullptr;

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
    return xSemaphoreTakeRecursive(g_sd_mutex, timeout_ticks) == pdTRUE;
}

void storage_sd_unlock()
{
    if (g_sd_mutex != nullptr) {
        xSemaphoreGiveRecursive(g_sd_mutex);
    }
}
