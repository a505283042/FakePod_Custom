#include "system_runtime.h"

#include "esp_err.h"
#include "esp_log.h"

#include "audio_spectrum_snapshot.h"
#include "audio_service.h"
#include "app_manager.h"
#include "music_app_adapter.h"
#include "settings_app.h"
#include "ebook_app.h"
#include "visual_music_app.h"
#include "video_app.h"
#include "sdcard.h"
#include "media_catalog_v2.h"
#include "artwork_loader.h"
#include "cover_surface_cache.h"
#include "lyrics/lyrics_service.h"
#include "power_service.h"
#include "gpio0_service.h"
#include "device_settings.h"
#include "battery_service.h"
#include "screen_lock_simple.h"

static const char *TAG = "运行期";

static bool g_ready_published = false;
static bool g_background_start_attempted = false;

static AudioOutputMode runtime_audio_output_mode(DeviceAudioOutputMode mode)
{
    switch (mode) {
        case DeviceAudioOutputMode::HighImpedanceHeadphones:
            return AudioOutputMode::HighImpedanceHeadphones;
        case DeviceAudioOutputMode::LineOut:
            return AudioOutputMode::LineOut;
        case DeviceAudioOutputMode::NormalHeadphones:
        default:
            return AudioOutputMode::NormalHeadphones;
    }
}

void system_ready_publish()
{
    // Boot 与 system_loop 当前都运行在 loopTask；这里只发布轻量边界，
    // 不在 UI 初始化调用栈里创建任何后台任务。
    g_ready_published = true;
}

void system_runtime_update()
{
    if (!g_ready_published || g_background_start_attempted) {
        return;
    }

    // 先置位，保证即使某个可选服务失败也不会每 10ms 重复创建任务。
    g_background_start_attempted = true;

    const esp_err_t app_ret = app_manager_init();
    if (app_ret != ESP_OK) {
        ESP_LOGW(TAG, "App Manager 初始化失败：%s；继续沿用Legacy Music前台", esp_err_to_name(app_ret));
    }
    const esp_err_t music_adapter_ret = app_ret == ESP_OK
        ? music_app_adapter_bind()
        : ESP_ERR_INVALID_STATE;
    if (music_adapter_ret != ESP_OK) {
        ESP_LOGW(TAG, "Music Lifecycle Adapter 绑定失败：%s；继续保持Legacy保护",
            esp_err_to_name(music_adapter_ret));
    }
    const esp_err_t ebook_ret = app_ret == ESP_OK
        ? ebook_app_register()
        : ESP_ERR_INVALID_STATE;
    if (ebook_ret != ESP_OK) {
        ESP_LOGW(TAG, "Ebook APP 注册失败：%s；Launcher仍保持选择但不会进入",
            esp_err_to_name(ebook_ret));
    }
    const esp_err_t visual_music_ret = app_ret == ESP_OK
        ? visual_music_app_register()
        : ESP_ERR_INVALID_STATE;
    if (visual_music_ret != ESP_OK) {
        ESP_LOGW(TAG, "电子音流 APP 注册失败：%s；Launcher仍保持选择但不会进入",
            esp_err_to_name(visual_music_ret));
    }
    const esp_err_t video_ret = app_ret == ESP_OK
        ? video_app_register()
        : ESP_ERR_INVALID_STATE;
    if (video_ret != ESP_OK) {
        ESP_LOGW(TAG, "Video APP 注册失败：%s；Launcher仍保持选择但不会进入",
            esp_err_to_name(video_ret));
    }

    const esp_err_t device_settings_ret = device_settings_init();
    if (device_settings_ret != ESP_OK) {
        ESP_LOGW(TAG, "设备设置NVS初始化失败：%s；Settings仍使用RAM默认值",
            esp_err_to_name(device_settings_ret));
    }
    if (device_settings_ret == ESP_OK) {
        DeviceSettingsSnapshot settings_snapshot = {};
        if (device_settings_get_snapshot(&settings_snapshot)) {
            const esp_err_t brightness_ret =
                screen_lock_simple_set_normal_brightness(settings_snapshot.brightness_level);
            if (brightness_ret != ESP_OK) {
                ESP_LOGW(TAG, "恢复屏幕亮度失败：level=%u %s",
                    static_cast<unsigned>(settings_snapshot.brightness_level),
                    esp_err_to_name(brightness_ret));
            }

            screen_lock_simple_configure_auto_off(
                settings_snapshot.auto_screen_off_seconds,
                settings_snapshot.aod_enabled);

            if (!audio_service_set_output_mode(
                    runtime_audio_output_mode(settings_snapshot.audio_output_mode), true)) {
                ESP_LOGW(TAG, "恢复CS43131输出档失败：%s；继续使用普通耳机安全档",
                    device_settings_audio_output_mode_name(settings_snapshot.audio_output_mode));
            }
        }
    }

    const esp_err_t settings_ret = app_ret == ESP_OK
        ? settings_app_register()
        : ESP_ERR_INVALID_STATE;
    if (settings_ret != ESP_OK) {
        ESP_LOGW(TAG, "Settings APP 注册失败：%s；Launcher仍保持选择但不会进入",
            esp_err_to_name(settings_ret));
    }

    const esp_err_t battery_ret = battery_service_init();
    if (battery_ret != ESP_OK) {
        ESP_LOGW(TAG, "电池ADC服务初始化失败：%s；V1暂不提供电量数据", esp_err_to_name(battery_ret));
    }

    const esp_err_t power_ret = power_service_init();
    if (power_ret != ESP_OK) {
        ESP_LOGW(TAG, "GPIO48 关机保存不可用：%s；硬件3秒断电仍保持原行为", esp_err_to_name(power_ret));
    }

    const esp_err_t auxkey_ret = gpio0_service_init();
    if (auxkey_ret != ESP_OK) {
        ESP_LOGW(TAG, "GPIO0 辅助键（音量/切歌 + 锁/AOD/熄屏长按）不可用：%s", esp_err_to_name(auxkey_ret));
    }

    const esp_err_t spectrum_ret = audio_spectrum_snapshot_start();
    if (spectrum_ret != ESP_OK) {
        ESP_LOGW(TAG, "SpectrumFFTTask 启动失败，频谱功能降级：%s", esp_err_to_name(spectrum_ret));
    }

    esp_err_t artwork_ret = ESP_ERR_INVALID_STATE;
    esp_err_t surface_ret = ESP_ERR_INVALID_STATE;
    esp_err_t lyrics_ret = ESP_ERR_INVALID_STATE;
    const bool storage_services_available = sdcard_is_mounted() && media_catalog_v2_ready();

    if (storage_services_available) {
        artwork_ret = artwork_loader_start();
        if (artwork_ret == ESP_OK) {
            surface_ret = cover_surface_cache_start();
        } else {
            ESP_LOGW(TAG, "ArtworkTask 启动失败，继续无封面运行：%s", esp_err_to_name(artwork_ret));
        }

        if (artwork_ret == ESP_OK && surface_ret != ESP_OK) {
            ESP_LOGW(TAG, "CoverSurfaceTask 启动失败，保留压缩图回退：%s", esp_err_to_name(surface_ret));
        }

        lyrics_ret = lyrics_service_start();
        if (lyrics_ret != ESP_OK) {
            ESP_LOGW(TAG, "LyricsTask 启动失败，歌词功能降级：%s", esp_err_to_name(lyrics_ret));
        }
    }

    ESP_LOGI(
        TAG,
        "READY 后台服务：Apps=%s MusicAdapter=%s Ebook=%s VisualMusic=%s Video=%s DeviceSettings=%s Settings=%s Battery=%s PowerKey=%s AuxKey=%s Spectrum=%s Artwork=%s CoverSurface=%s Lyrics=%s",
        esp_err_to_name(app_ret),
        esp_err_to_name(music_adapter_ret),
        esp_err_to_name(ebook_ret),
        esp_err_to_name(visual_music_ret),
        esp_err_to_name(video_ret),
        esp_err_to_name(device_settings_ret),
        esp_err_to_name(settings_ret),
        esp_err_to_name(battery_ret),
        esp_err_to_name(power_ret),
        esp_err_to_name(auxkey_ret),
        esp_err_to_name(spectrum_ret),
        storage_services_available ? esp_err_to_name(artwork_ret) : "SKIPPED",
        storage_services_available && artwork_ret == ESP_OK ? esp_err_to_name(surface_ret) : "SKIPPED",
        storage_services_available ? esp_err_to_name(lyrics_ret) : "SKIPPED"
    );
}
