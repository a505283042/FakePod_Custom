#include "system_runtime.h"

#include "esp_err.h"
#include "esp_log.h"

#include "audio_spectrum_snapshot.h"
#include "artwork_loader.h"
#include "cover_surface_cache.h"
#include "lyrics/lyrics_service.h"

static const char *TAG = "运行期";

static bool g_ready_published = false;
static bool g_background_start_attempted = false;

void system_ready_publish()
{
    // Boot 与 system_loop 当前都运行在 loopTask；这里只发布轻量边界，
    // 不在 UI 初始化调用栈里创建任何后台任务。
    g_ready_published = true;
}

bool system_ready_is_published()
{
    return g_ready_published;
}

void system_runtime_update()
{
    if (!g_ready_published || g_background_start_attempted) {
        return;
    }

    // 先置位，保证即使某个可选服务失败也不会每 10ms 重复创建任务。
    g_background_start_attempted = true;

    const esp_err_t spectrum_ret = audio_spectrum_snapshot_start();
    if (spectrum_ret != ESP_OK) {
        ESP_LOGW(TAG, "SpectrumFFTTask 启动失败，频谱功能降级：%s", esp_err_to_name(spectrum_ret));
    }

    const esp_err_t artwork_ret = artwork_loader_start();
    esp_err_t surface_ret = ESP_ERR_INVALID_STATE;
    if (artwork_ret == ESP_OK) {
        surface_ret = cover_surface_cache_start();
    } else {
        ESP_LOGW(TAG, "ArtworkTask 启动失败，继续无封面运行：%s", esp_err_to_name(artwork_ret));
    }

    if (artwork_ret == ESP_OK && surface_ret != ESP_OK) {
        ESP_LOGW(TAG, "CoverSurfaceTask 启动失败，保留压缩图回退：%s", esp_err_to_name(surface_ret));
    }

    const esp_err_t lyrics_ret = lyrics_service_start();
    if (lyrics_ret != ESP_OK) {
        ESP_LOGW(TAG, "LyricsTask 启动失败，歌词功能降级：%s", esp_err_to_name(lyrics_ret));
    }

    ESP_LOGI(
        TAG,
        "READY 后台服务：Spectrum=%s Artwork=%s CoverSurface=%s Lyrics=%s",
        esp_err_to_name(spectrum_ret),
        esp_err_to_name(artwork_ret),
        artwork_ret == ESP_OK ? esp_err_to_name(surface_ret) : "SKIPPED",
        esp_err_to_name(lyrics_ret)
    );
}
