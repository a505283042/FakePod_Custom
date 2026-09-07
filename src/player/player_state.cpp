#include "player_state.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

static const char *TAG = "播放器状态";
static bool g_ready = false;

static bool player_state_select(bool selected, const char *action);

static void player_state_log_current(const char *action)
{
    if (player_playlist_get_folder_scope() != PlayerFolderScope::All) {
        PlayerFolderQueueSnapshot queue = {};
        if (player_playlist_get_folder_queue_snapshot(&queue) &&
            queue.track_count > 0U && queue.track_index != UINT32_MAX) {
            const char *path = media_library_get_path(queue.track_index);
            ESP_LOGI(TAG, "%s：范围=%s 实际=%s 位置=%lu/%lu track=%lu %s",
                action != nullptr ? action : "选择",
                player_playlist_folder_scope_name(queue.preferred_scope),
                player_playlist_folder_scope_name(queue.effective_scope),
                static_cast<unsigned long>(queue.current_in_queue ? queue.position + 1U : 0U),
                static_cast<unsigned long>(queue.track_count),
                static_cast<unsigned long>(queue.track_index),
                path != nullptr ? path : "<invalid>");
            return;
        }
    }

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

static bool player_state_find_track_by_path(const char *path, size_t *out_track_index)
{
    if (path == nullptr || path[0] == '\0' || out_track_index == nullptr) {
        return false;
    }

    size_t lo = 0U;
    size_t hi = media_library_get_count();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        const char *candidate = media_library_get_path(mid);
        if (candidate == nullptr) {
            return false;
        }
        const int cmp = strcasecmp(candidate, path);
        if (cmp < 0) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }

    if (lo >= media_library_get_count()) {
        return false;
    }
    const char *candidate = media_library_get_path(lo);
    if (candidate == nullptr || strcasecmp(candidate, path) != 0) {
        return false;
    }
    *out_track_index = lo;
    return true;
}

bool player_state_rebind_after_catalog_reload(const char *preferred_path, size_t fallback_track_index)
{
    if (!media_library_is_ready()) {
        g_ready = false;
        ESP_LOGE(TAG, "Catalog热刷新后音乐库未就绪，无法重绑Player");
        return false;
    }

    const esp_err_t ret = player_playlist_init();
    if (ret != ESP_OK) {
        g_ready = false;
        ESP_LOGE(TAG, "Catalog热刷新后重建播放列表失败：%s", esp_err_to_name(ret));
        return false;
    }
    g_ready = true;

    if (media_library_get_count() == 0U) {
        ESP_LOGI(TAG, "Catalog热刷新后曲库为空，Player已绑定空列表");
        return true;
    }

    size_t restored_track = 0U;
    const bool found = player_state_find_track_by_path(preferred_path, &restored_track);
    if (!found) {
        restored_track = fallback_track_index < media_library_get_count()
            ? fallback_track_index
            : media_library_get_count() - 1U;
    }
    if (!player_state_select_all_tracks(restored_track)) {
        ESP_LOGE(TAG, "Catalog热刷新后恢复选择失败：track=%u", static_cast<unsigned>(restored_track));
        return false;
    }

    if (found) {
        ESP_LOGI(TAG, "Catalog热刷新后恢复原歌曲：track=%u %s",
            static_cast<unsigned>(restored_track), preferred_path);
    } else {
        ESP_LOGW(TAG, "Catalog热刷新后原歌曲已不存在，回退相邻有效歌曲：track=%u old=%s",
            static_cast<unsigned>(restored_track), preferred_path != nullptr ? preferred_path : "<none>");
    }
    return true;
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
    if (player_playlist_get_folder_scope() != PlayerFolderScope::All) {
        PlayerFolderQueueSnapshot queue = {};
        return player_state_get_folder_queue_snapshot(&queue) ? queue.position : 0U;
    }
    PlayerListSnapshot snapshot = {};
    return player_state_get_list_snapshot(&snapshot) ? snapshot.position : 0U;
}

size_t player_state_get_list_count()
{
    if (player_playlist_get_folder_scope() != PlayerFolderScope::All) {
        PlayerFolderQueueSnapshot queue = {};
        return player_state_get_folder_queue_snapshot(&queue) ? queue.track_count : 0U;
    }
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
    if (!g_ready || buffer == nullptr || buffer_size == 0U) {
        return false;
    }
    if (player_playlist_get_folder_scope() != PlayerFolderScope::All) {
        PlayerFolderQueueSnapshot queue = {};
        if (!player_state_get_folder_queue_snapshot(&queue)) {
            return false;
        }
        if (queue.effective_scope == PlayerFolderScope::Level1 ||
            queue.effective_scope == PlayerFolderScope::Level2) {
            char folder_path[PLAYER_FOLDER_PATH_MAX] = {};
            if (player_playlist_copy_folder_selection(
                    queue.effective_scope, folder_path, sizeof(folder_path))) {
                size_t length = strlen(folder_path);
                while (length > 0U && folder_path[length - 1U] == '/') {
                    folder_path[--length] = '\0';
                }
                const char *slash = strrchr(folder_path, '/');
                const char *leaf = slash != nullptr ? slash + 1U : folder_path;
                if (leaf[0] != '\0') {
                    snprintf(buffer, buffer_size, "%s", leaf);
                    return true;
                }
            }
        }
        snprintf(buffer, buffer_size, "%s",
            player_playlist_folder_scope_name(queue.effective_scope));
        return true;
    }
    return player_playlist_copy_label(buffer, buffer_size);
}

bool player_state_set_folder_scope(PlayerFolderScope scope)
{
    return g_ready && player_playlist_set_folder_scope(scope);
}

PlayerFolderScope player_state_get_folder_scope()
{
    return player_playlist_get_folder_scope();
}

bool player_state_set_folder_selection(PlayerFolderScope scope, const char *folder_path)
{
    return g_ready && player_playlist_set_folder_selection(scope, folder_path);
}

bool player_state_copy_folder_selection(
    PlayerFolderScope scope, char *buffer, size_t buffer_size)
{
    return g_ready && player_playlist_copy_folder_selection(scope, buffer, buffer_size);
}

bool player_state_get_folder_queue_snapshot(PlayerFolderQueueSnapshot *out_snapshot)
{
    return g_ready && player_playlist_get_folder_queue_snapshot(out_snapshot);
}

bool player_state_select_folder_queue_position(size_t position)
{
    return player_state_select(
        player_playlist_select_folder_queue_position(position),
        "目录播放范围选择");
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
    if (!g_ready || !player_playlist_folder_previous()) {
        return false;
    }
    player_state_log_current("上一首");
    return true;
}

bool player_state_next()
{
    if (!g_ready || !player_playlist_folder_next()) {
        return false;
    }
    player_state_log_current("下一首");
    return true;
}
