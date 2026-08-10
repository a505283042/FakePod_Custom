#include "player_playlist.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "media_catalog_v2.h"
#include "media_groups_v2.h"
#include "media_library.h"
#include "app_diag_config.h"

static const char *TAG = "播放列表";

struct PlayerListContext
{
    bool ready = false;
    PlayerListType type = PlayerListType::AllTracks;
    uint32_t catalog_generation = 0;
    uint32_t group_index = UINT32_MAX;
    uint32_t group_id = UINT32_MAX;
    uint32_t position = 0;
    uint16_t decade_start = 0;
    bool decade_unknown = false;
};

static PlayerListContext g_context = {};

const char *player_playlist_type_name(PlayerListType type)
{
    switch (type) {
        case PlayerListType::AllTracks: return "全部歌曲";
        case PlayerListType::Artist: return "歌手";
        case PlayerListType::Album: return "专辑";
        case PlayerListType::Decade: return "年代";
    }
    return "未知列表";
}

static bool playlist_catalog_generation_valid()
{
    return g_context.ready &&
        media_catalog_v2_ready() &&
        g_context.catalog_generation != 0U &&
        g_context.catalog_generation == media_catalog_v2_generation();
}

static bool playlist_resolve(
    uint32_t *out_track_index,
    uint32_t *out_track_count,
    uint32_t *out_group_id,
    uint16_t *out_decade_start,
    bool *out_decade_unknown)
{
    if (!playlist_catalog_generation_valid()) {
        return false;
    }

    uint32_t track_index = UINT32_MAX;
    uint32_t track_count = 0U;
    uint32_t group_id = g_context.group_id;
    uint16_t decade_start = g_context.decade_start;
    bool decade_unknown = g_context.decade_unknown;

    switch (g_context.type) {
        case PlayerListType::AllTracks:
            track_count = static_cast<uint32_t>(media_library_get_count());
            if (g_context.position >= track_count) {
                return false;
            }
            track_index = g_context.position;
            group_id = UINT32_MAX;
            decade_start = 0;
            decade_unknown = false;
            break;

        case PlayerListType::Artist:
        {
            MediaArtistGroupViewV2 view = {};
            if (!media_groups_v2_get_artist(g_context.group_index, &view) ||
                view.generation != g_context.catalog_generation ||
                view.artist_id != g_context.group_id ||
                g_context.position >= view.track_count ||
                view.track_indices == nullptr) {
                return false;
            }
            track_count = view.track_count;
            track_index = view.track_indices[g_context.position];
            group_id = view.artist_id;
            break;
        }

        case PlayerListType::Album:
        {
            MediaAlbumGroupViewV2 view = {};
            if (!media_groups_v2_get_album(g_context.group_index, &view) ||
                view.generation != g_context.catalog_generation ||
                view.album_id != g_context.group_id ||
                g_context.position >= view.track_count ||
                view.track_indices == nullptr) {
                return false;
            }
            track_count = view.track_count;
            track_index = view.track_indices[g_context.position];
            group_id = view.album_id;
            break;
        }

        case PlayerListType::Decade:
        {
            MediaDecadeGroupViewV2 view = {};
            if (!media_groups_v2_get_decade(g_context.group_index, &view) ||
                view.generation != g_context.catalog_generation ||
                view.decade_start != g_context.decade_start ||
                view.unknown != g_context.decade_unknown ||
                g_context.position >= view.track_count ||
                view.track_indices == nullptr) {
                return false;
            }
            track_count = view.track_count;
            track_index = view.track_indices[g_context.position];
            group_id = UINT32_MAX;
            decade_start = view.decade_start;
            decade_unknown = view.unknown;
            break;
        }
    }

    if (track_index >= media_library_get_count()) {
        return false;
    }
    if (out_track_index != nullptr) {
        *out_track_index = track_index;
    }
    if (out_track_count != nullptr) {
        *out_track_count = track_count;
    }
    if (out_group_id != nullptr) {
        *out_group_id = group_id;
    }
    if (out_decade_start != nullptr) {
        *out_decade_start = decade_start;
    }
    if (out_decade_unknown != nullptr) {
        *out_decade_unknown = decade_unknown;
    }
    return true;
}

static void playlist_log_selection(const char *action)
{
    PlayerListSnapshot snapshot = {};
    if (!player_playlist_get_snapshot(&snapshot)) {
        ESP_LOGW(TAG, "%s后列表已失效", action != nullptr ? action : "选择");
        return;
    }
#if APP_DIAG_PLAYER_PLAYLIST
    ESP_LOGI(TAG, "%s：类型=%s group=%lu 位置=%lu/%lu track=%lu generation=%lu",
        action != nullptr ? action : "选择",
        player_playlist_type_name(snapshot.type),
        static_cast<unsigned long>(snapshot.group_id),
        static_cast<unsigned long>(snapshot.position + 1U),
        static_cast<unsigned long>(snapshot.track_count),
        static_cast<unsigned long>(snapshot.track_index),
        static_cast<unsigned long>(snapshot.catalog_generation));
#else
    (void)action;
#endif
}

static bool playlist_bind_base(PlayerListType type, size_t group_index, size_t position)
{
    if (!media_library_is_ready() || !media_catalog_v2_ready()) {
        return false;
    }

    const uint32_t group_index_u32 = static_cast<uint32_t>(group_index);
    const uint32_t position_u32 = static_cast<uint32_t>(position);
    if (static_cast<size_t>(group_index_u32) != group_index ||
        static_cast<size_t>(position_u32) != position) {
        return false;
    }

    PlayerListContext next = {};
    next.ready = true;
    next.type = type;
    next.catalog_generation = media_catalog_v2_generation();
    next.group_index = group_index_u32;
    next.position = position_u32;
    next.group_id = UINT32_MAX;

    switch (type) {
        case PlayerListType::AllTracks:
            if (media_library_get_count() == 0U || position >= media_library_get_count()) {
                return false;
            }
            next.group_index = UINT32_MAX;
            break;

        case PlayerListType::Artist:
        {
            MediaArtistGroupViewV2 view = {};
            if (!media_groups_v2_get_artist(group_index, &view) ||
                view.generation != next.catalog_generation ||
                position >= view.track_count) {
                return false;
            }
            next.group_id = view.artist_id;
            break;
        }

        case PlayerListType::Album:
        {
            MediaAlbumGroupViewV2 view = {};
            if (!media_groups_v2_get_album(group_index, &view) ||
                view.generation != next.catalog_generation ||
                position >= view.track_count) {
                return false;
            }
            next.group_id = view.album_id;
            break;
        }

        case PlayerListType::Decade:
        {
            MediaDecadeGroupViewV2 view = {};
            if (!media_groups_v2_get_decade(group_index, &view) ||
                view.generation != next.catalog_generation ||
                position >= view.track_count) {
                return false;
            }
            next.decade_start = view.decade_start;
            next.decade_unknown = view.unknown;
            break;
        }
    }

    g_context = next;
    uint32_t track_index = UINT32_MAX;
    if (!playlist_resolve(&track_index, nullptr, nullptr, nullptr, nullptr)) {
        g_context = {};
        return false;
    }
    playlist_log_selection("绑定列表");
    return true;
}

esp_err_t player_playlist_init()
{
    g_context = {};
    if (!media_library_is_ready() || !media_catalog_v2_ready()) {
        ESP_LOGE(TAG, "MusicCatalogV2 尚未就绪");
        return ESP_ERR_INVALID_STATE;
    }
    if (media_library_get_count() == 0U) {
        g_context.ready = true;
        g_context.type = PlayerListType::AllTracks;
        g_context.catalog_generation = media_catalog_v2_generation();
        ESP_LOGI(TAG, "音乐库为空，已建立空的全部歌曲上下文：generation=%lu",
            static_cast<unsigned long>(g_context.catalog_generation));
        return ESP_OK;
    }
    if (!player_playlist_bind_all(0U)) {
        return ESP_FAIL;
    }
#if APP_DIAG_PLAYER_PLAYLIST
    ESP_LOGI(TAG, "PLAYLIST_TRACE: READY generation=%lu all=%u artist_groups=%u album_groups=%u decade_groups=%u",
        static_cast<unsigned long>(media_catalog_v2_generation()),
        static_cast<unsigned>(media_library_get_count()),
        static_cast<unsigned>(media_groups_v2_artist_count()),
        static_cast<unsigned>(media_groups_v2_album_count()),
        static_cast<unsigned>(media_groups_v2_decade_count()));
#endif
    return ESP_OK;
}

bool player_playlist_is_ready()
{
    if (!g_context.ready || !media_catalog_v2_ready()) {
        return false;
    }
    return g_context.catalog_generation == media_catalog_v2_generation();
}

bool player_playlist_bind_all(size_t position)
{
    return playlist_bind_base(PlayerListType::AllTracks, UINT32_MAX, position);
}

bool player_playlist_bind_artist(size_t group_index, size_t position)
{
    return playlist_bind_base(PlayerListType::Artist, group_index, position);
}

bool player_playlist_bind_album(size_t group_index, size_t position)
{
    return playlist_bind_base(PlayerListType::Album, group_index, position);
}

bool player_playlist_bind_decade(size_t group_index, size_t position)
{
    return playlist_bind_base(PlayerListType::Decade, group_index, position);
}

bool player_playlist_get_snapshot(PlayerListSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr || !g_context.ready) {
        return false;
    }

    PlayerListSnapshot snapshot = {};
    snapshot.ready = true;
    snapshot.type = g_context.type;
    snapshot.catalog_generation = g_context.catalog_generation;
    snapshot.group_index = g_context.group_index;
    snapshot.position = g_context.position;

    // 空音乐库只有“全部歌曲”空上下文；它仍可被 UI 读取，但没有当前 Track。
    if (g_context.type == PlayerListType::AllTracks && media_library_get_count() == 0U) {
        if (!playlist_catalog_generation_valid()) {
            return false;
        }
        snapshot.group_id = UINT32_MAX;
        snapshot.track_count = 0U;
        snapshot.track_index = UINT32_MAX;
        *out_snapshot = snapshot;
        return true;
    }

    if (!playlist_resolve(
            &snapshot.track_index,
            &snapshot.track_count,
            &snapshot.group_id,
            &snapshot.decade_start,
            &snapshot.decade_unknown)) {
        return false;
    }
    *out_snapshot = snapshot;
    return true;
}

bool player_playlist_get_track_index(size_t *out_track_index)
{
    if (out_track_index == nullptr) {
        return false;
    }
    uint32_t track_index = UINT32_MAX;
    if (!playlist_resolve(&track_index, nullptr, nullptr, nullptr, nullptr)) {
        return false;
    }
    *out_track_index = static_cast<size_t>(track_index);
    return true;
}

bool player_playlist_get_track_index_at_position(size_t position, size_t *out_track_index)
{
    if (out_track_index == nullptr || !playlist_catalog_generation_valid()) {
        return false;
    }
    if (position > UINT32_MAX) {
        return false;
    }

    const uint32_t pos = static_cast<uint32_t>(position);
    uint32_t track_index = UINT32_MAX;

    switch (g_context.type) {
        case PlayerListType::AllTracks:
            if (pos >= media_library_get_count()) return false;
            track_index = pos;
            break;

        case PlayerListType::Artist:
        {
            MediaArtistGroupViewV2 view = {};
            if (!media_groups_v2_get_artist(g_context.group_index, &view) ||
                view.generation != g_context.catalog_generation ||
                view.artist_id != g_context.group_id ||
                pos >= view.track_count || view.track_indices == nullptr) {
                return false;
            }
            track_index = view.track_indices[pos];
            break;
        }

        case PlayerListType::Album:
        {
            MediaAlbumGroupViewV2 view = {};
            if (!media_groups_v2_get_album(g_context.group_index, &view) ||
                view.generation != g_context.catalog_generation ||
                view.album_id != g_context.group_id ||
                pos >= view.track_count || view.track_indices == nullptr) {
                return false;
            }
            track_index = view.track_indices[pos];
            break;
        }

        case PlayerListType::Decade:
        {
            MediaDecadeGroupViewV2 view = {};
            if (!media_groups_v2_get_decade(g_context.group_index, &view) ||
                view.generation != g_context.catalog_generation ||
                view.decade_start != g_context.decade_start ||
                view.unknown != g_context.decade_unknown ||
                pos >= view.track_count || view.track_indices == nullptr) {
                return false;
            }
            track_index = view.track_indices[pos];
            break;
        }
    }

    if (track_index >= media_library_get_count()) {
        return false;
    }
    *out_track_index = static_cast<size_t>(track_index);
    return true;
}

bool player_playlist_select_position(size_t position)
{
    size_t track_index = 0U;
    if (!player_playlist_get_track_index_at_position(position, &track_index) || position > UINT32_MAX) {
        return false;
    }

    g_context.position = static_cast<uint32_t>(position);
    playlist_log_selection("直接选择位置");
    return true;
}

static bool playlist_move(int direction)
{
    PlayerListSnapshot snapshot = {};
    if (!player_playlist_get_snapshot(&snapshot) || snapshot.track_count == 0U) {
        return false;
    }
    if (direction < 0) {
        g_context.position = snapshot.position == 0U ? snapshot.track_count - 1U : snapshot.position - 1U;
    } else {
        g_context.position = (snapshot.position + 1U) % snapshot.track_count;
    }
    playlist_log_selection(direction < 0 ? "选择上一首" : "选择下一首");
    return true;
}

bool player_playlist_previous()
{
    return playlist_move(-1);
}

bool player_playlist_next()
{
    return playlist_move(1);
}

bool player_playlist_copy_label(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U) {
        return false;
    }
    buffer[0] = '\0';
    PlayerListSnapshot snapshot = {};
    if (!player_playlist_get_snapshot(&snapshot)) {
        return false;
    }

    switch (snapshot.type) {
        case PlayerListType::AllTracks:
            snprintf(buffer, buffer_size, "全部歌曲");
            return true;
        case PlayerListType::Artist:
        {
            MediaArtistGroupViewV2 view = {};
            if (!media_groups_v2_get_artist(snapshot.group_index, &view) || view.name == nullptr) {
                return false;
            }
            snprintf(buffer, buffer_size, "%s", view.name);
            return true;
        }
        case PlayerListType::Album:
        {
            MediaAlbumGroupViewV2 view = {};
            if (!media_groups_v2_get_album(snapshot.group_index, &view) || view.title == nullptr) {
                return false;
            }
            snprintf(buffer, buffer_size, "%s", view.title);
            return true;
        }
        case PlayerListType::Decade:
            if (snapshot.decade_unknown) {
                snprintf(buffer, buffer_size, "未知年代");
            } else {
                snprintf(buffer, buffer_size, "%u年代", static_cast<unsigned>(snapshot.decade_start));
            }
            return true;
    }
    return false;
}
