#include "boot_state.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
#include "persistent_state.h"
#include "device_settings.h"
#include "usb_storage_service.h"
#include "power_service.h"
#include "gpio0_service.h"


static const char *TAG =
    "启动";


static BootState g_state =
    BootState::CheckPsram;

// 启动故障分级属于 Boot Orchestrator 私有实现，不泄漏到公共头文件。
enum class BootFailureLevel : uint8_t
{
    None = 0,
    Optional,
    Degraded,
    Fatal
};

static void boot_state_update();
static bool boot_state_is_degraded();
static bool boot_state_has_error();

static void boot_library_scan_event(
    MediaLibraryScanEvent event,
    uint32_t current_count,
    void *)
{
    if (event == MediaLibraryScanEvent::InitialBuild) {
        (void)ui_manager_show_library_build_progress(current_count);
    } else if (event == MediaLibraryScanEvent::ChangesDetected) {
        (void)ui_manager_show_library_update_progress(0U);
    } else if (event == MediaLibraryScanEvent::IncrementalAddedProgress) {
        (void)ui_manager_show_library_update_progress(current_count);
    }
}

static uint32_t g_degraded_issues = 0U;
static uint32_t g_optional_issues = 0U;
static BootIssue g_fatal_issue = BootIssue::None;
static esp_err_t g_fatal_error = ESP_OK;
static bool g_usb_storage_requested = false;

static uint32_t boot_issue_mask(BootIssue issue)
{
    return static_cast<uint32_t>(issue);
}

static const char *boot_fatal_ui_reason(BootIssue issue)
{
    switch (issue) {
        case BootIssue::PsramUnavailable:
            return "PSRAM unavailable";
        case BootIssue::I2cUnavailable:
            return "I2C unavailable";
        case BootIssue::DisplayUnavailable:
            return "Display unavailable";
        case BootIssue::UiBootstrapUnavailable:
            return "LVGL unavailable";
        case BootIssue::AudioUnavailable:
            return "Audio unavailable";
        case BootIssue::UiUnavailable:
            return "UI unavailable";
        case BootIssue::UsbStorageUnavailable:
            return "USB storage unavailable";
        default:
            return "Core startup failure";
    }
}

static bool boot_issue_recorded(BootIssue issue)
{
    const uint32_t mask = boot_issue_mask(issue);
    return (g_degraded_issues & mask) != 0U ||
        (g_optional_issues & mask) != 0U ||
        g_fatal_issue == issue;
}

static void boot_record_issue(
    BootFailureLevel level,
    BootIssue issue,
    esp_err_t error,
    const char *message
)
{
    const char *text = message != nullptr ? message : "未知启动故障";

    switch (level) {
        case BootFailureLevel::Optional:
            g_optional_issues |= boot_issue_mask(issue);
            ESP_LOGW(TAG, "可选功能不可用：%s：%s", text, esp_err_to_name(error));
            break;

        case BootFailureLevel::Degraded:
            g_degraded_issues |= boot_issue_mask(issue);
            ESP_LOGW(TAG, "降级启动：%s：%s", text, esp_err_to_name(error));
            break;

        case BootFailureLevel::Fatal:
        {
            g_fatal_issue = issue;
            g_fatal_error = error;
            ESP_LOGE(TAG, "致命启动故障：%s：%s", text, esp_err_to_name(error));
            g_state = BootState::Error;

            // 只有 LVGL 启动核心已经建立时才显示错误页；更早的硬件故障只保留串口诊断。
            // 无论错误页是否可用，顶层都会停止 Boot 和 system_loop，保持安全终态。
            const bool error_page_visible = ui_manager_show_boot_fatal(
                boot_fatal_ui_reason(issue),
                error
            );
            ESP_LOGE(
                TAG,
                "启动进入致命终态：issue=0x%08lX ui=%s",
                static_cast<unsigned long>(boot_issue_mask(issue)),
                error_page_visible ? "ERROR_PAGE" : "SERIAL_ONLY"
            );
            break;
        }

        case BootFailureLevel::None:
            break;
    }
}


// ============================================================
// 初始化启动状态机
// ============================================================

void boot_state_init()
{
    g_state =
        BootState::CheckPsram;
    g_degraded_issues = 0U;
    g_optional_issues = 0U;
    g_fatal_issue = BootIssue::None;
    g_fatal_error = ESP_OK;
    g_usb_storage_requested = false;


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
    if (g_state == BootState::UsbStorageService) {
        return BootRunResult::Service;
    }
    if (boot_state_is_ready()) {
        return boot_state_is_degraded()
            ? BootRunResult::ReadyDegraded
            : BootRunResult::Ready;
    }

    boot_state_update();

    if (boot_state_has_error()) {
        return BootRunResult::Fatal;
    }
    if (g_state == BootState::UsbStorageService) {
        return BootRunResult::Service;
    }
    if (!boot_state_is_ready()) {
        return BootRunResult::Running;
    }
    return boot_state_is_degraded()
        ? BootRunResult::ReadyDegraded
        : BootRunResult::Ready;
}


// ============================================================
// 更新启动状态机
// ============================================================

static void boot_state_update()
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

                boot_record_issue(
                    BootFailureLevel::Fatal,
                    BootIssue::PsramUnavailable,
                    ESP_FAIL,
                    "PSRAM 初始化失败"
                );
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
                BootState::InitPersistentState;

            break;
        }


        // ====================================================
        // NVS 持久化数据层（可选能力）
        // ====================================================

        case BootState::InitPersistentState:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(TAG, "启动阶段：初始化 NVS V1 持久化数据层");
#endif
            const esp_err_t persistence_ret = persistent_state_init();
            if (persistence_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Optional,
                    BootIssue::PersistenceUnavailable,
                    persistence_ret,
                    "NVS 持久化不可用，继续使用运行时默认值"
                );
            }

            // USB TF卡服务模式必须在普通SD/音频/媒体服务启动前决定。
            // Settings数据因此前移到Boot NVS阶段加载；system_runtime再次调用时会直接复用。
            const esp_err_t settings_ret = device_settings_init();
            if (settings_ret != ESP_OK) {
                ESP_LOGW(TAG, "设备设置提前加载失败：%s；保持普通串口启动", esp_err_to_name(settings_ret));
                g_usb_storage_requested = false;
            } else {
                DeviceSettingsSnapshot settings = {};
                g_usb_storage_requested =
                    device_settings_get_snapshot(&settings) && settings.usb_mode == DeviceUsbMode::TfCard;
                if (g_usb_storage_requested) {
                    ESP_LOGI(TAG, "检测到一次性TF卡USB文件管理请求");
                }
            }
            g_state = BootState::InitI2C;
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


            const esp_err_t i2c_ret = i2c_bus_init();
            if (i2c_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Fatal,
                    BootIssue::I2cUnavailable,
                    i2c_ret,
                    "I2C 总线初始化失败"
                );
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


            const esp_err_t touch_ret = cst820_init();
            if (touch_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Degraded,
                    BootIssue::TouchUnavailable,
                    touch_ret,
                    "CST820 触摸不可用，继续无触摸运行"
                );
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


            const esp_err_t imu_ret = qmi8658_init();
            if (imu_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Optional,
                    BootIssue::ImuUnavailable,
                    imu_ret,
                    "QMI8658 不可用，关闭姿态附加能力"
                );
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

            const esp_err_t ret = audio_service_start();
            if (ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Fatal,
                    BootIssue::AudioUnavailable,
                    ret,
                    "AudioTask / CS43131 音频核心启动失败"
                );
                break;
            }

#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(TAG, "正式播放器音频服务已就绪");
#endif
            if (!boot_issue_recorded(BootIssue::PersistenceUnavailable) &&
                !persistent_state_restore_audio()) {
                boot_record_issue(
                    BootFailureLevel::Optional,
                    BootIssue::PersistenceUnavailable,
                    ESP_FAIL,
                    "NVS 音量恢复失败，继续使用 AudioTask 默认音量"
                );
            }
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


            const esp_err_t storage_ret = sdcard_init();
            if (storage_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Degraded,
                    BootIssue::StorageUnavailable,
                    storage_ret,
                    "TF 卡不可用，跳过音乐库并进入无存储运行"
                );
                g_state = BootState::InitPlayer;
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

            MediaLibraryChangeSummary changes = {};
            const esp_err_t library_ret = media_library_scan(
                &changes,
                boot_library_scan_event,
                nullptr
            );
            if (library_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Degraded,
                    BootIssue::LibraryUnavailable,
                    library_ret,
                    "音乐库不可用，继续保留基础界面"
                );
            } else if (!changes.had_previous_catalog) {
                ESP_LOGI(
                    TAG,
                    "首次建立音乐库完成：tracks=%lu",
                    static_cast<unsigned long>(changes.current_count)
                );
                if (ui_manager_show_library_build_complete(changes.current_count)) {
                    vTaskDelay(pdMS_TO_TICKS(900));
                }
            } else if (changes.changed) {
                ESP_LOGI(
                    TAG,
                    "启动曲库增量更新：tracks=%lu->%lu 新增=%lu 删除=%lu 更新=%lu",
                    static_cast<unsigned long>(changes.previous_count),
                    static_cast<unsigned long>(changes.current_count),
                    static_cast<unsigned long>(changes.added_count),
                    static_cast<unsigned long>(changes.removed_count),
                    static_cast<unsigned long>(changes.updated_count)
                );
                if (ui_manager_show_library_update_complete(
                        changes.added_count,
                        changes.removed_count,
                        changes.updated_count)) {
                    // LVGL 刷新任务独立运行；只在真实曲库变化时短暂保留结果提示。
                    vTaskDelay(pdMS_TO_TICKS(900));
                }
            }

            g_state = BootState::InitPlayer;

            break;
        }

        // ====================================================
        // Player 顶层状态 / 控制
        // ====================================================

        case BootState::InitPlayer:
        {
#if APP_DIAG_BOOT_VERBOSE
            ESP_LOGI(TAG, "启动阶段：建立播放器顶层状态");
#endif

            if (media_library_is_ready()) {
                const esp_err_t state_ret = player_state_init();
                if (state_ret != ESP_OK) {
                    boot_record_issue(
                        BootFailureLevel::Degraded,
                        BootIssue::PlayerStateUnavailable,
                        state_ret,
                        "播放器选择状态不可用"
                    );
                }
            }

            const esp_err_t control_ret = player_control_init();
            if (control_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Degraded,
                    BootIssue::PlayerControlUnavailable,
                    control_ret,
                    "播放器控制不可用"
                );
            }

            if (control_ret == ESP_OK &&
                !boot_issue_recorded(BootIssue::PersistenceUnavailable) &&
                !persistent_state_restore_player()) {
                ESP_LOGW(TAG, "NVS Player 恢复未完全命中，已保留可用默认/降级选择");
            }

            g_state = BootState::InitUI;

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


            const esp_err_t display_ret = display_init();
            if (display_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Fatal,
                    BootIssue::DisplayUnavailable,
                    display_ret,
                    "AMOLED 显示核心初始化失败"
                );
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

            const esp_err_t ui_bootstrap_ret = ui_manager_bootstrap_init();
            if (ui_bootstrap_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Fatal,
                    BootIssue::UiBootstrapUnavailable,
                    ui_bootstrap_ret,
                    "LVGL 启动核心初始化失败"
                );
                break;
            }

            g_state = g_usb_storage_requested
                ? BootState::InitUsbStorageService
                : BootState::InitTouch;
            break;
        }

        // ====================================================
        // 一次性 TF 卡 USB MSC 服务模式
        // ====================================================
        case BootState::InitUsbStorageService:
        {
            // 先把持久化模式恢复为串口，确保服务模式只执行一次。
            // 即使本轮USB/SD初始化失败，下次重启也能回到普通系统修复问题。
            const esp_err_t reset_ret = device_settings_set_usb_mode(DeviceUsbMode::Serial);
            if (reset_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Fatal,
                    BootIssue::UsbStorageUnavailable,
                    reset_ret,
                    "无法复位USB模式，拒绝进入可能循环启动的服务模式"
                );
                break;
            }

            const esp_err_t service_ret = usb_storage_service_start();
            if (service_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Fatal,
                    BootIssue::UsbStorageUnavailable,
                    service_ret,
                    "TF卡 USB MSC 服务启动失败"
                );
                break;
            }

            (void)ui_manager_show_usb_storage_service();
            g_state = BootState::UsbStorageService;
            ESP_LOGI(TAG, "USB服务模式就绪：跳过触摸/音频/媒体库/完整UI；下次开机恢复串口模式");
            break;
        }

        case BootState::UsbStorageService:
        {
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

            const esp_err_t ui_ret = ui_manager_init();
            if (ui_ret != ESP_OK) {
                boot_record_issue(
                    BootFailureLevel::Fatal,
                    BootIssue::UiUnavailable,
                    ui_ret,
                    "LVGL 用户界面初始化失败"
                );
                break;
            }

            // CST820 初始化成功只代表硬件可访问；真正的用户输入能力还要求
            // LVGL indev 建立成功。Touch Fast Path 失败但同步读取仍可用时不会误报降级。
            if (!ui_manager_touch_available() && !boot_issue_recorded(BootIssue::TouchUnavailable)) {
                boot_record_issue(
                    BootFailureLevel::Degraded,
                    BootIssue::TouchUnavailable,
                    ui_manager_touch_error(),
                    "LVGL 触摸输入不可用，继续无触摸运行"
                );
            }

            // UI 与全部核心依赖已经建立。先发布唯一 READY 边界；
            // Artwork/Cover/Lyrics 等后台服务由 system_loop 在 READY 之后统一启动。
            g_state =
                BootState::Ready;
            system_ready_publish();

            ESP_LOGI(
                TAG,
                "READY：mode=%s tracks=%u degraded=0x%08lX optional=0x%08lX PSRAM_free=%uKB",
                boot_state_is_degraded() ? "DEGRADED" : "NORMAL",
                static_cast<unsigned>(media_library_get_count()),
                static_cast<unsigned long>(g_degraded_issues),
                static_cast<unsigned long>(g_optional_issues),
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


static bool boot_state_is_degraded()
{
    return g_degraded_issues != 0U;
}

bool boot_state_get_status(BootStatusSnapshot *out_status)
{
    if (out_status == nullptr) {
        return false;
    }
    out_status->state = g_state;
    out_status->degraded_issues = g_degraded_issues;
    out_status->optional_issues = g_optional_issues;
    out_status->fatal_issue = g_fatal_issue;
    out_status->fatal_error = g_fatal_error;
    return true;
}

// ============================================================
// 是否启动失败
// ============================================================

static bool boot_state_has_error()
{
    return
        g_state ==
        BootState::Error;
}
