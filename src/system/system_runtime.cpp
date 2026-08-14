#include "system_runtime.h"

#include "esp_err.h"
#include "esp_log.h"

#include "audio_spectrum_snapshot.h"
#include "app_manager.h"
#include "sdcard.h"
#include "media_catalog_v2.h"
#include "artwork_loader.h"
#include "cover_surface_cache.h"
#include "lyrics/lyrics_service.h"
#include "power_service.h"

static const char *TAG = "运行期";

static bool g_ready_published = false;
static bool g_background_start_attempted = false;

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

    const esp_err_t power_ret = power_service_init();
    if (power_ret != ESP_OK) {
        ESP_LOGW(TAG, "GPIO48 关机保存不可用：%s；硬件3秒断电仍保持原行为", esp_err_to_name(power_ret));
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
        "READY 后台服务：Apps=%s PowerKey=%s Spectrum=%s Artwork=%s CoverSurface=%s Lyrics=%s",
        esp_err_to_name(app_ret),
        esp_err_to_name(power_ret),
        esp_err_to_name(spectrum_ret),
        storage_services_available ? esp_err_to_name(artwork_ret) : "SKIPPED",
        storage_services_available && artwork_ret == ESP_OK ? esp_err_to_name(surface_ret) : "SKIPPED",
        storage_services_available ? esp_err_to_name(lyrics_ret) : "SKIPPED"
    );
}
