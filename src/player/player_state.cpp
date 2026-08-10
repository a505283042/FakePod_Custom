#include "player_state.h"

#include "esp_log.h"

static const char *TAG = "播放器状态";
static bool g_ready = false;

static void player_state_log_current(const char *action)
{
    PlayerListSnapshot snapshot = {};
    if (!player_playlist_get_snapshot(&snapshot)) {
        ESP_LOGW(TAG, "%s后播放列表不可用", action != nullptr ? action : "选择");
        return;
    }
    if (snapshot.track_count == 0U || snapshot.track_index == UINT32_MAX) {
        ESP_LOGI(TAG, "%s：当前列表为空", action != nullptr ? action : "选择");
        return;
    }
    const char *path = media_library_get_path(snapshot.track_index);
    ESP_LOGI(TAG, "%s：列表=%s 位置=%lu/%lu track=%lu %s",
        action != nullptr ? action : "选择",
        player_playlist_type_name(snapshot.type),
        static_cast<unsigned long>(snapshot.position + 1U),
        static_cast<unsigned long>(snapshot.track_count),
        static_cast<unsigned long>(snapshot.track_index),
        path != nullptr ? path : "<invalid>");
}

esp_err_t player_state_init()
{
    if (!media_library_is_ready()) {
        ESP_LOGE(TAG, "音乐库尚未就绪");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = player_playlist_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化播放列表上下文失败：%s", esp_err_to_name(ret));
        return ret;
    }
    g_ready = true;
    player_state_log_current("当前歌曲");
    return ESP_OK;
}

bool player_state_is_ready()
{
    return g_ready && player_playlist_is_ready();
}

size_t player_state_get_index()
{
    size_t track_index = 0U;
    return g_ready && player_playlist_get_track_index(&track_index) ? track_index : 0U;
}

const char *player_state_get_path()
{
    if (!g_ready) {
        return nullptr;
    }
    size_t track_index = 0U;
    if (!player_playlist_get_track_index(&track_index)) {
        return nullptr;
    }
    return media_library_get_path(track_index);
}

MediaFormat player_state_get_format()
{
    if (!g_ready) {
        return MediaFormat::Unknown;
    }
    size_t track_index = 0U;
    if (!player_playlist_get_track_index(&track_index)) {
        return MediaFormat::Unknown;
    }
    return media_library_get_format(track_index);
}

bool player_state_get_list_snapshot(PlayerListSnapshot *out_snapshot)
{
    return g_ready && player_playlist_get_snapshot(out_snapshot);
}

size_t player_state_get_list_position()
{
    PlayerListSnapshot snapshot = {};
    return player_state_get_list_snapshot(&snapshot) ? snapshot.position : 0U;
}

size_t player_state_get_list_count()
{
    PlayerListSnapshot snapshot = {};
    return player_state_get_list_snapshot(&snapshot) ? snapshot.track_count : 0U;
}

PlayerListType player_state_get_list_type()
{
    PlayerListSnapshot snapshot = {};
    return player_state_get_list_snapshot(&snapshot) ? snapshot.type : PlayerListType::AllTracks;
}

bool player_state_copy_list_label(char *buffer, size_t buffer_size)
{
    return g_ready && player_playlist_copy_label(buffer, buffer_size);
}

static bool player_state_select(bool selected, const char *action)
{
    if (!g_ready || !selected) {
        return false;
    }
    player_state_log_current(action);
    return true;
}

bool player_state_select_all_tracks(size_t position)
{
    return player_state_select(player_playlist_bind_all(position), "切换全部歌曲");
}

bool player_state_select_artist_group(size_t group_index, size_t position)
{
    return player_state_select(player_playlist_bind_artist(group_index, position), "切换歌手列表");
}

bool player_state_select_album_group(size_t group_index, size_t position)
{
    return player_state_select(player_playlist_bind_album(group_index, position), "切换专辑列表");
}

bool player_state_select_decade_group(size_t group_index, size_t position)
{
    return player_state_select(player_playlist_bind_decade(group_index, position), "切换年代列表");
}

bool player_state_select_position(size_t position)
{
    return player_state_select(player_playlist_select_position(position), "直接选择列表位置");
}

bool player_state_previous()
{
    if (!g_ready || !player_playlist_previous()) {
        return false;
    }
    player_state_log_current("上一首");
    return true;
}

bool player_state_next()
{
    if (!g_ready || !player_playlist_next()) {
        return false;
    }
    player_state_log_current("下一首");
    return true;
}
