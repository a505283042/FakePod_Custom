#include "sdcard.h"

#include <dirent.h>
#include <stdio.h>

#include "driver/sdmmc_host.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "sdmmc_cmd.h"

#include "board_pins.h"
#include "storage_io.h"
#include "app_diag_config.h"


static const char *TAG = "TF卡";

#if APP_DIAG_BOOT_VERBOSE
#define SDCARD_BOOT_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define SDCARD_BOOT_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif


static sdmmc_card_t *g_card =
    nullptr;


static bool g_mounted =
    false;


static uint32_t g_capacity_mb =
    0;


// ============================================================
// 初始化 TF 卡
// ============================================================

esp_err_t sdcard_init()
{
    if (g_mounted) {
        return ESP_OK;
    }


    const esp_err_t storage_ret =
        storage_io_init();


    if (storage_ret != ESP_OK) {
        return storage_ret;
    }


    // 挂载本身也属于 SD 总线生命周期的一部分。启动阶段没有竞争者，
    // 但仍统一经过中央锁，保证后续热插拔/重新挂载时不会形成旁路。
    StorageSdLockGuard sd_lock;
    if (!sd_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }


    SDCARD_BOOT_LOGI("正在初始化 TF 卡");


    SDCARD_BOOT_LOGI(
        "SDMMC：CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d",
        FAKEPOD_SD_CLK,
        FAKEPOD_SD_CMD,
        FAKEPOD_SD_D0,
        FAKEPOD_SD_D1,
        FAKEPOD_SD_D2,
        FAKEPOD_SD_D3
    );


    // ========================================================
    // SDMMC Host
    // ========================================================

    sdmmc_host_t host =
        SDMMC_HOST_DEFAULT();


    // 稳定模式固定使用 20MHz。
    // 实测 40MHz High Speed 在首次大曲库连续目录查找时可能出现单次文件访问长时间不返回，
    // 即使卡片声明支持高速模式，也不能代表具体 PCB 走线、上拉和卡片组合在该频率下稳定。
    // 建库性能继续通过减少重复路径查找和分阶段建库优化，不再用提高总线频率换取稳定性。
    host.max_freq_khz =
        SDMMC_FREQ_DEFAULT;


    // ========================================================
    // SDMMC 引脚
    // ========================================================

    sdmmc_slot_config_t slot_config =
        SDMMC_SLOT_CONFIG_DEFAULT();


    slot_config.width = 4;


    slot_config.clk =
        static_cast<gpio_num_t>(
            FAKEPOD_SD_CLK
        );


    slot_config.cmd =
        static_cast<gpio_num_t>(
            FAKEPOD_SD_CMD
        );


    slot_config.d0 =
        static_cast<gpio_num_t>(
            FAKEPOD_SD_D0
        );


    slot_config.d1 =
        static_cast<gpio_num_t>(
            FAKEPOD_SD_D1
        );


    slot_config.d2 =
        static_cast<gpio_num_t>(
            FAKEPOD_SD_D2
        );


    slot_config.d3 =
        static_cast<gpio_num_t>(
            FAKEPOD_SD_D3
        );


    // ========================================================
    // FAT 文件系统配置
    // ========================================================

    esp_vfs_fat_mount_config_t mount_config =
        {};


    // 非常重要：
    // 挂载失败时绝对不自动格式化用户 TF 卡。
    mount_config.format_if_mount_failed =
        false;


    mount_config.max_files =
        16;


    mount_config.allocation_unit_size =
        16 * 1024;


    SDCARD_BOOT_LOGI("正在挂载 FAT 文件系统");


    esp_err_t ret =
        esp_vfs_fat_sdmmc_mount(
            "/sdcard",
            &host,
            &slot_config,
            &mount_config,
            &g_card
        );


    if (ret != ESP_OK) {

        ESP_LOGW(
            TAG,
            "TF 卡挂载失败：%s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    g_mounted = true;


    // 卡容量：
    // sector_size × sector_count
    uint64_t bytes =
        static_cast<uint64_t>(
            g_card->csd.capacity
        ) *
        g_card->csd.sector_size;


    g_capacity_mb =
        static_cast<uint32_t>(
            bytes / 1024 / 1024
        );


    ESP_LOGI(
        TAG,
        "TF 卡挂载成功"
    );


    SDCARD_BOOT_LOGI(
        "名称：%s",
        g_card->cid.name
    );


    SDCARD_BOOT_LOGI(
        "容量：%lu MB",
        static_cast<unsigned long>(
            g_capacity_mb
        )
    );


    SDCARD_BOOT_LOGI("总线宽度：4-bit");


    // real_freq_khz 是 SDMMC Host 最终实际采用的时钟；不要根据请求值假定一定跑满 40MHz。
    ESP_LOGI(
        TAG,
        "SDMMC运行参数：请求上限=%dkHz 卡支持上限=%lukHz 实际=%dkHz 总线=%u-bit",
        host.max_freq_khz,
        static_cast<unsigned long>(g_card->max_freq_khz),
        g_card->real_freq_khz,
        static_cast<unsigned>(1U << g_card->log_bus_width)
    );


    return ESP_OK;
}


// ============================================================
// USB MSC 运行时接管：卸载 VFS/FATFS，并由 IDF mount helper 释放原 SDMMC Host。
// ============================================================

esp_err_t sdcard_unmount_for_usb()
{
    if (!g_mounted || g_card == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    StorageSdLockGuard sd_lock(pdMS_TO_TICKS(1000));
    if (!sd_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }

    sdmmc_card_t *card = g_card;
    const esp_err_t ret = esp_vfs_fat_sdcard_unmount("/sdcard", card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB接管前卸载TF卡失败：%s", esp_err_to_name(ret));
        return ret;
    }

    g_card = nullptr;
    g_mounted = false;
    g_capacity_mb = 0;
    ESP_LOGI(TAG, "TF卡已从 /sdcard 卸载，VFS/普通SDMMC owner 已释放");
    return ESP_OK;
}


// ============================================================
// 是否已经挂载
// ============================================================

bool sdcard_is_mounted()
{
    return g_mounted;
}


// ============================================================
// 获取容量
// ============================================================

uint32_t sdcard_get_capacity_mb()
{
    return g_capacity_mb;
}


// ============================================================
// 调试：列出根目录
// ============================================================

void sdcard_debug_list_root()
{
    if (!g_mounted) {

        ESP_LOGW(
            TAG,
            "TF 卡尚未挂载，无法读取目录"
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "TF 卡根目录："
    );


    StorageSdLockGuard sd_lock;
    if (!sd_lock.locked()) {
        ESP_LOGE(TAG, "读取根目录前获取 SD 锁失败");
        return;
    }


    DIR *dir =
        opendir("/sdcard");


    if (dir == nullptr) {

        ESP_LOGE(
            TAG,
            "打开 TF 卡根目录失败"
        );

        return;
    }


    struct dirent *entry =
        nullptr;


    int count = 0;


    while (
        (entry = readdir(dir)) != nullptr
    ) {

        ESP_LOGI(
            TAG,
            "  [%02d] %s",
            count + 1,
            entry->d_name
        );


        count++;


        if (count >= 30) {

            ESP_LOGI(
                TAG,
                "目录项目过多，仅显示前 30 项"
            );

            break;
        }
    }


    closedir(dir);


    ESP_LOGI(
        TAG,
        "根目录读取完成，共显示 %d 项",
        count
    );
}