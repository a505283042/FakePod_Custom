#include "boot_state.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"

#include "app_build_info.h"
#include "app_diag_config.h"

#include "i2c_bus.h"
#include "cst820.h"
#include "qmi8658.h"
#include "audio_service.h"
#include "sdcard.h"
#include "display.h"
#include "media_library.h"
#include "player_state.h"
#include "player_control.h"
#include "ui_manager.h"
#include "system_runtime.h"


static const char *TAG =
    "启动";


static BootState g_state =
    BootState::CheckPsram;


// ============================================================
// 初始化启动状态机
// ============================================================

void boot_state_init()
{
    g_state =
        BootState::CheckPsram;


    ESP_LOGI(
        TAG,
        "FakePod Custom %s，阶段=%s，诊断=%s",
        FAKEPOD_FIRMWARE_VERSION,
        FAKEPOD_FIRMWARE_PHASE,
        APP_DIAG_PROFILE_NAME
    );

    // 正式启动从第一个真实阶段直接开始，不再保留历史串口等待状态。
}


BootRunResult boot_run()
{
    if (boot_state_has_error()) {
        return BootRunResult::Fatal;
    }
    if (boot_state_is_ready()) {
        return BootRunResult::Ready;
    }

    boot_state_update();

    if (boot_state_has_error()) {
        return BootRunResult::Fatal;
    }
    return boot_state_is_ready()
        ? BootRunResult::Ready
        : BootRunResult::Running;
}


// ============================================================
// 更新启动状态机
// ============================================================

void boot_state_update()
{
    switch (g_state) {

        // ====================================================
        // PSRAM
        // ====================================================

        case BootState::CheckPsram:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(
                TAG,
                "启动阶段：检查 PSRAM"
            );
#endif


            if (
                !esp_psram_is_initialized()
            ) {

                ESP_LOGE(
                    TAG,
                    "PSRAM 初始化失败"
                );

                g_state =
                    BootState::Error;

                break;
            }


#if APP_DIAG_BOOT_VERBOSE
            const size_t free_psram =
                heap_caps_get_free_size(
                    MALLOC_CAP_SPIRAM
                );
            ESP_LOGI(
                TAG,
                "PSRAM 正常，可用 %u KB",
                static_cast<unsigned>(
                    free_psram / 1024
                )
            );
#endif


            g_state =
                BootState::InitI2C;

            break;
        }


        // ====================================================
        // I2C
        // ====================================================

        case BootState::InitI2C:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(
                TAG,
                "启动阶段：初始化 I2C"
            );
#endif


            if (
                i2c_bus_init() !=
                ESP_OK
            ) {

                ESP_LOGE(
                    TAG,
                    "I2C 初始化失败"
                );

                g_state =
                    BootState::Error;

                break;
            }


#if APP_DIAG_BOOT_VERBOSE
            i2c_bus_scan();
#endif


            g_state =
                BootState::InitDisplay;

            break;
        }


        // ====================================================
        // CST820
        // ====================================================

        case BootState::InitTouch:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(
                TAG,
                "启动阶段：初始化触摸"
            );
#endif


            if (
                cst820_init() !=
                ESP_OK
            ) {

                ESP_LOGE(
                    TAG,
                    "触摸初始化失败"
                );

                g_state =
                    BootState::Error;

                break;
            }


            g_state =
                BootState::InitIMU;

            break;
        }


        // ====================================================
        // QMI8658
        // ====================================================

        case BootState::InitIMU:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(
                TAG,
                "启动阶段：初始化 IMU"
            );
#endif


            if (
                qmi8658_init() !=
                ESP_OK
            ) {

                ESP_LOGE(
                    TAG,
                    "IMU 初始化失败"
                );

                g_state =
                    BootState::Error;

                break;
            }


            g_state =
                BootState::InitAudioService;

            break;
        }

        // ====================================================
        // AudioTask + CS43131
        // ====================================================

        case BootState::InitAudioService:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(TAG, "启动阶段：启动正式 AudioTask");
#endif

            esp_err_t ret = audio_service_start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "AudioTask 启动失败：%s", esp_err_to_name(ret));
                g_state = BootState::Error;
                break;
            }

#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(TAG, "正式播放器音频服务已就绪；开机不再执行测试音");
#endif
            g_state = BootState::InitSDCard;
            break;
        }

        // ====================================================
        // TF 卡
        // ====================================================

        case BootState::InitSDCard:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(
                TAG,
                "启动阶段：初始化 TF 卡"
            );
#endif


            if (
                sdcard_init() !=
                ESP_OK
            ) {

                ESP_LOGE(
                    TAG,
                    "TF 卡初始化失败"
                );

                g_state =
                    BootState::Error;

                break;
            }


#if APP_DIAG_BOOT_VERBOSE
            // DEBUG/STRESS 才打印根目录，RELEASE 不做额外目录遍历。
            sdcard_debug_list_root();
#endif


            g_state =
                BootState::ScanMediaLibrary;

            break;
        }

        // ====================================================
        // 音乐库
        // ====================================================

        case BootState::ScanMediaLibrary:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(
                TAG,
                "启动阶段：扫描音乐库"
            );
#endif

            if (
                media_library_scan() !=
                ESP_OK
            ) {
                ESP_LOGE(
                    TAG,
                    "音乐库扫描失败"
                );

                g_state =
                    BootState::Error;

                break;
            }

            if (player_state_init() != ESP_OK) {
                ESP_LOGE(TAG, "播放器选择状态初始化失败");
                g_state = BootState::Error;
                break;
            }
            if (player_control_init() != ESP_OK) {
                ESP_LOGE(TAG, "播放器控制初始化失败");
                g_state = BootState::Error;
                break;
            }

            g_state =
                BootState::InitUI;

            break;
        }

        // ====================================================
        // CO5300 AMOLED
        // ====================================================

        case BootState::InitDisplay:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(
                TAG,
                "启动阶段：初始化 AMOLED"
            );
#endif


            if (
                display_init() !=
                ESP_OK
            ) {

                ESP_LOGE(
                    TAG,
                    "AMOLED 初始化失败"
                );

                g_state =
                    BootState::Error;

                break;
            }


            g_state =
                BootState::InitUIBootstrap;

            break;
        }


        // ====================================================
        // LVGL 启动核心 / 首帧
        // ====================================================

        case BootState::InitUIBootstrap:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(TAG, "启动阶段：建立 LVGL 启动页并等待首帧揭屏");
#endif

            if (ui_manager_bootstrap_init() != ESP_OK) {
                ESP_LOGE(TAG, "LVGL 启动核心初始化失败");
                g_state = BootState::Error;
                break;
            }

            g_state = BootState::InitTouch;
            break;
        }


        // ====================================================
        // LVGL 用户界面
        // ====================================================

        case BootState::InitUI:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(
                TAG,
                "启动阶段：初始化 LVGL 用户界面"
            );
#endif

            if (ui_manager_init() != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "LVGL 用户界面初始化失败"
                );

                g_state =
                    BootState::Error;

                break;
            }

            // UI 与全部核心依赖已经建立。先发布唯一 READY 边界；
            // Artwork/Cover/Lyrics 等后台服务由 system_loop 在 READY 之后统一启动。
            g_state =
                BootState::Ready;
            system_ready_publish();

            ESP_LOGI(
                TAG,
                "READY：tracks=%u PSRAM_free=%uKB",
                static_cast<unsigned>(media_library_get_count()),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U)
            );

            break;
        }


        case BootState::Ready:
        {
            break;
        }


        case BootState::Error:
        {
            break;
        }
    }
}


// ============================================================
// 是否启动完成
// ============================================================

bool boot_state_is_ready()
{
    return
        g_state ==
        BootState::Ready;
}


// ============================================================
// 是否启动失败
// ============================================================

bool boot_state_has_error()
{
    return
        g_state ==
        BootState::Error;
}


// ============================================================
// 获取当前启动状态
// ============================================================

BootState boot_state_get()
{
    return g_state;
}