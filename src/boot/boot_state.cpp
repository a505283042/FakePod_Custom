#include "boot_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"

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


static const char *TAG =
    "启动";


static BootState g_state =
    BootState::WaitStart;


static TickType_t g_start_tick =
    0;


// ============================================================
// 初始化启动状态机
// ============================================================

void boot_state_init()
{
    g_state =
        BootState::WaitStart;


    g_start_tick =
        xTaskGetTickCount();


    ESP_LOGI(
        TAG,
        "========================================"
    );

    ESP_LOGI(
        TAG,
        "FakePod Custom Firmware"
    );

    ESP_LOGI(
        TAG,
        "开始系统启动"
    );

    ESP_LOGI(
        TAG,
        "========================================"
    );


    ESP_LOGI(
        TAG,
        "等待 5 秒，便于连接串口监视器"
    );
}


// ============================================================
// 更新启动状态机
// ============================================================

void boot_state_update()
{
    switch (g_state) {

        // ====================================================
        // 等待串口监视器
        // ====================================================

        case BootState::WaitStart:
        {
            TickType_t elapsed =
                xTaskGetTickCount() -
                g_start_tick;


            if (
                elapsed <
                pdMS_TO_TICKS(5000)
            ) {

                return;
            }


            g_state =
                BootState::CheckPsram;

            break;
        }


        // ====================================================
        // 1. PSRAM
        // ====================================================

        case BootState::CheckPsram:
        {
            ESP_LOGI(
                TAG,
                "步骤 1/9：检查 PSRAM"
            );


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


            size_t free_psram =
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


            g_state =
                BootState::InitI2C;

            break;
        }


        // ====================================================
        // 2. I2C
        // ====================================================

        case BootState::InitI2C:
        {
            ESP_LOGI(
                TAG,
                "步骤 2/9：初始化 I2C"
            );


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


            i2c_bus_scan();


            g_state =
                BootState::InitTouch;

            break;
        }


        // ====================================================
        // 3. CST820
        // ====================================================

        case BootState::InitTouch:
        {
            ESP_LOGI(
                TAG,
                "步骤 3/9：初始化触摸"
            );


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
        // 4. QMI8658
        // ====================================================

        case BootState::InitIMU:
        {
            ESP_LOGI(
                TAG,
                "步骤 4/9：初始化 IMU"
            );


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
        // 5. AudioTask + CS43131
        // ====================================================

        case BootState::InitAudioService:
        {
            ESP_LOGI(TAG, "步骤 5/9：启动正式 AudioTask");

            esp_err_t ret = audio_service_start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "AudioTask 启动失败：%s", esp_err_to_name(ret));
                g_state = BootState::Error;
                break;
            }

            ESP_LOGI(TAG, "正式播放器音频服务已就绪；开机不再执行测试音");
            g_state = BootState::InitSDCard;
            break;
        }

        // ====================================================
        // 6. TF 卡
        // ====================================================

        case BootState::InitSDCard:
        {
            ESP_LOGI(
                TAG,
                "步骤 6/9：初始化 TF 卡"
            );


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


            // Bring-up 阶段保留目录打印
            sdcard_debug_list_root();


            g_state =
                BootState::ScanMediaLibrary;

            break;
        }

        // ====================================================
        // 7. 音乐库
        // ====================================================

        case BootState::ScanMediaLibrary:
        {
            ESP_LOGI(
                TAG,
                "步骤 7/9：扫描音乐库"
            );

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
                BootState::InitDisplay;

            break;
        }

        // ====================================================
        // 8. CO5300 AMOLED
        // ====================================================

        case BootState::InitDisplay:
        {
            ESP_LOGI(
                TAG,
                "步骤 8/9：初始化 AMOLED"
            );


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
                BootState::InitUI;

            break;
        }


        // ====================================================
        // 9. LVGL 用户界面
        // ====================================================

        case BootState::InitUI:
        {
            ESP_LOGI(
                TAG,
                "步骤 9/9：初始化 LVGL 用户界面"
            );

            if (ui_manager_init() != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "LVGL 用户界面初始化失败"
                );

                g_state =
                    BootState::Error;

                break;
            }

            g_state =
                BootState::Ready;

            ESP_LOGI(
                TAG,
                "========================================"
            );

            ESP_LOGI(
                TAG,
                "FakePod 基础硬件、AudioTask、音乐库与界面启动完成"
            );

            ESP_LOGI(
                TAG,
                "========================================"
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