#include "player_state.h"

#include "esp_log.h"

static const char *TAG = "播放器状态";
static size_t g_current_index = 0;
static bool g_ready = false;

esp_err_t player_state_init()
{
    if (!media_library_is_ready()) {
        ESP_LOGE(TAG, "音乐库尚未就绪");
        return ESP_ERR_INVALID_STATE;
    }
    g_current_index = 0;
    g_ready = true;
    const size_t count = media_library_get_count();
    if (count == 0) {
        ESP_LOGI(TAG, "音乐库为空，当前没有选中歌曲");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "当前歌曲：1/%u %s",
        static_cast<unsigned>(count),
        media_library_get_path(g_current_index));
    return ESP_OK;
}

bool player_state_is_ready()
{
    return g_ready;
}

size_t player_state_get_index()
{
    return g_current_index;
}

const char *player_state_get_path()
{
    if (!g_ready || media_library_get_count() == 0) {
        return nullptr;
    }
    return media_library_get_path(g_current_index);
}

MediaFormat player_state_get_format()
{
    if (!g_ready || media_library_get_count() == 0) {
        return MediaFormat::Unknown;
    }
    return media_library_get_format(g_current_index);
}

bool player_state_previous()
{
    const size_t count = media_library_get_count();
    if (!g_ready || count == 0) {
        return false;
    }
    g_current_index = g_current_index == 0 ? count - 1 : g_current_index - 1;
    ESP_LOGI(TAG, "选择上一首：%u/%u %s",
        static_cast<unsigned>(g_current_index + 1),
        static_cast<unsigned>(count),
        media_library_get_path(g_current_index));
    return true;
}

bool player_state_next()
{
    const size_t count = media_library_get_count();
    if (!g_ready || count == 0) {
        return false;
    }
    g_current_index = (g_current_index + 1) % count;
    ESP_LOGI(TAG, "选择下一首：%u/%u %s",
        static_cast<unsigned>(g_current_index + 1),
        static_cast<unsigned>(count),
        media_library_get_path(g_current_index));
    return true;
}
