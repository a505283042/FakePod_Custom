#include "player_playlist.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

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
static PlayerFolderScope g_folder_scope = PlayerFolderScope::All;
static char g_folder_level1_path[PLAYER_FOLDER_PATH_MAX] = {};
static char g_folder_level2_path[PLAYER_FOLDER_PATH_MAX] = {};

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


namespace {

static constexpr const char *kMusicRoot = "/sdcard/MUSIC";

struct FolderQueueDescriptor
{
    PlayerFolderQueueSnapshot snapshot = {};
    const char *prefix_path = nullptr;
    size_t prefix_length = 0U;
    uint32_t range_start = 0U;
    uint32_t range_end = 0U;
    bool direct_only = false;
};

static bool folder_scope_valid(PlayerFolderScope scope)
{
    return static_cast<uint8_t>(scope) <= static_cast<uint8_t>(PlayerFolderScope::Level2);
}

static uint32_t folder_context_hash(const char *text, size_t length)
{
    // FNV-1a，ASCII 路径部分统一折叠大小写；只用于 Shuffle 上下文身份，不用于安全校验。
    uint32_t hash = 2166136261U;
    for (size_t i = 0U; text != nullptr && i < length; ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<unsigned char>(ch - 'A' + 'a');
        }
        hash ^= static_cast<uint32_t>(ch);
        hash *= 16777619U;
    }
    return hash;
}

static bool folder_path_under_music_root(const char *path)
{
    if (path == nullptr || path[0] == '\0') return false;
    const size_t root_length = strlen(kMusicRoot);
    const size_t path_length = strlen(path);
    return path_length > root_length &&
        strncasecmp(path, kMusicRoot, root_length) == 0 && path[root_length] == '/';
}

static bool folder_selection_path_valid(PlayerFolderScope scope, const char *path)
{
    if ((scope != PlayerFolderScope::Level1 && scope != PlayerFolderScope::Level2) ||
        !folder_path_under_music_root(path)) {
        return false;
    }

    const size_t length = strlen(path);
    const size_t root_length = strlen(kMusicRoot);
    if (length <= root_length + 2U || length >= PLAYER_FOLDER_PATH_MAX || path[length - 1U] != '/') {
        return false;
    }

    const char *relative = path + root_length + 1U;
    const char *first_slash = strchr(relative, '/');
    if (first_slash == nullptr) return false;

    if (scope == PlayerFolderScope::Level1) {
        return first_slash == path + length - 1U;
    }

    const char *second_slash = strchr(first_slash + 1U, '/');
    return second_slash != nullptr && second_slash == path + length - 1U;
}

static bool folder_path_matches(const char *path, const FolderQueueDescriptor &descriptor)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    if (descriptor.snapshot.effective_scope == PlayerFolderScope::All) {
        return true;
    }
    if (descriptor.prefix_path == nullptr || descriptor.prefix_length == 0U ||
        strncasecmp(path, descriptor.prefix_path, descriptor.prefix_length) != 0) {
        return false;
    }

    const char *remainder = path + descriptor.prefix_length;
    if (remainder[0] == '\0') {
        return false;
    }
    return !descriptor.direct_only || strchr(remainder, '/') == nullptr;
}

static bool folder_find_prefix_range(
    const char *prefix,
    size_t prefix_length,
    uint32_t *out_start,
    uint32_t *out_end)
{
    if (prefix == nullptr || prefix_length == 0U || out_start == nullptr || out_end == nullptr) {
        return false;
    }

    const size_t library_count = media_library_get_count();
    size_t lo = 0U;
    size_t hi = library_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        const char *path = media_library_get_path(mid);
        if (path == nullptr) {
            return false;
        }
        if (strncasecmp(path, prefix, prefix_length) < 0) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }

    const size_t start = lo;
    lo = start;
    hi = library_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        const char *path = media_library_get_path(mid);
        if (path == nullptr) {
            return false;
        }
        if (strncasecmp(path, prefix, prefix_length) <= 0) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    const size_t end = lo;

    if (start > UINT32_MAX || end > UINT32_MAX) {
        return false;
    }
    *out_start = static_cast<uint32_t>(start);
    *out_end = static_cast<uint32_t>(end);
    return true;
}

static bool folder_selection_track_count(
    PlayerFolderScope scope,
    const char *folder_path,
    uint32_t *out_track_count)
{
    if (!playlist_catalog_generation_valid() ||
        !folder_selection_path_valid(scope, folder_path)) {
        return false;
    }

    const size_t prefix_length = strlen(folder_path);
    uint32_t range_start = 0U;
    uint32_t range_end = 0U;
    if (!folder_find_prefix_range(folder_path, prefix_length, &range_start, &range_end)) {
        return false;
    }

    uint32_t count = 0U;
    if (scope == PlayerFolderScope::Level1) {
        count = range_end - range_start;
    } else {
        FolderQueueDescriptor descriptor = {};
        descriptor.snapshot.effective_scope = PlayerFolderScope::Level2;
        descriptor.prefix_path = folder_path;
        descriptor.prefix_length = prefix_length;
        descriptor.direct_only = true;
        for (uint32_t i = range_start; i < range_end; ++i) {
            if (folder_path_matches(media_library_get_path(i), descriptor)) {
                ++count;
            }
        }
    }

    if (out_track_count != nullptr) *out_track_count = count;
    return count > 0U;
}

static bool folder_option_candidate(
    PlayerFolderScope scope,
    const char *parent_path,
    const char *track_path,
    size_t *out_length)
{
    if (track_path == nullptr || out_length == nullptr) return false;

    if (scope == PlayerFolderScope::Level1) {
        if (!folder_path_under_music_root(track_path)) return false;
        const size_t root_length = strlen(kMusicRoot);
        const char *relative = track_path + root_length + 1U;
        const char *slash = strchr(relative, '/');
        if (slash == nullptr) return false;
        *out_length = static_cast<size_t>(slash - track_path) + 1U;
        return *out_length < PLAYER_FOLDER_PATH_MAX;
    }

    if (scope != PlayerFolderScope::Level2 ||
        !folder_selection_path_valid(PlayerFolderScope::Level1, parent_path)) {
        return false;
    }
    const size_t parent_length = strlen(parent_path);
    if (strncasecmp(track_path, parent_path, parent_length) != 0) return false;

    const char *relative = track_path + parent_length;
    const char *slash = strchr(relative, '/');
    if (slash == nullptr) return false;
    // 二级列表只播放二级目录直属音乐，因此只有该 Track 本身直属二级目录时才把目录列为候选。
    if (strchr(slash + 1U, '/') != nullptr) return false;

    *out_length = static_cast<size_t>(slash - track_path) + 1U;
    return *out_length < PLAYER_FOLDER_PATH_MAX;
}

static size_t folder_option_scan(
    PlayerFolderScope scope,
    const char *parent_path,
    size_t wanted_position,
    bool want_copy,
    char *path_buffer,
    size_t path_buffer_size,
    uint32_t *out_track_count)
{
    if (!playlist_catalog_generation_valid() ||
        (scope != PlayerFolderScope::Level1 && scope != PlayerFolderScope::Level2)) {
        return 0U;
    }
    if (want_copy && (path_buffer == nullptr || path_buffer_size == 0U)) return 0U;

    char current_option[PLAYER_FOLDER_PATH_MAX] = {};
    size_t option_count = 0U;
    uint32_t current_tracks = 0U;
    bool target_active = false;

    const size_t library_count = media_library_get_count();
    for (size_t i = 0U; i < library_count; ++i) {
        const char *track_path = media_library_get_path(i);
        size_t candidate_length = 0U;
        if (!folder_option_candidate(scope, parent_path, track_path, &candidate_length)) {
            continue;
        }

        const bool same = current_option[0] != '\0' &&
            strlen(current_option) == candidate_length &&
            strncasecmp(current_option, track_path, candidate_length) == 0;
        if (!same) {
            if (target_active) {
                if (out_track_count != nullptr) *out_track_count = current_tracks;
                return option_count;
            }
            if (candidate_length >= sizeof(current_option)) continue;
            memcpy(current_option, track_path, candidate_length);
            current_option[candidate_length] = '\0';
            current_tracks = 0U;
            target_active = want_copy && option_count == wanted_position;
            if (target_active) {
                if (candidate_length + 1U > path_buffer_size) return 0U;
                memcpy(path_buffer, current_option, candidate_length + 1U);
            }
            ++option_count;
        }
        ++current_tracks;
    }

    if (target_active && out_track_count != nullptr) *out_track_count = current_tracks;
    return option_count;
}

static uint32_t folder_playlist_context_id(const PlayerListSnapshot &playlist)
{
    // “总列表”仍沿用当前 PlayerListType（全部歌曲/歌手/专辑/年代）。
    // generation 会单独参与 Shuffle 上下文比较，这里只需要区分同一 generation 内的列表身份。
    uint32_t hash = 2166136261U;
    const uint32_t parts[] = {
        static_cast<uint32_t>(playlist.type),
        playlist.group_index,
        playlist.group_id,
        static_cast<uint32_t>(playlist.decade_start),
        playlist.decade_unknown ? 1U : 0U,
    };
    for (uint32_t value : parts) {
        for (uint8_t shift = 0U; shift < 32U; shift += 8U) {
            hash ^= (value >> shift) & 0xFFU;
            hash *= 16777619U;
        }
    }
    return hash;
}

static void folder_descriptor_set_all_library(FolderQueueDescriptor *descriptor)
{
    if (descriptor == nullptr) return;
    descriptor->snapshot.effective_scope = PlayerFolderScope::All;
    descriptor->prefix_path = nullptr;
    descriptor->prefix_length = 0U;
    descriptor->range_start = 0U;
    descriptor->range_end = static_cast<uint32_t>(media_library_get_count());
    descriptor->direct_only = false;
    descriptor->snapshot.context_id = folder_context_hash(kMusicRoot, strlen(kMusicRoot));
    descriptor->snapshot.current_in_queue =
        descriptor->snapshot.track_index < descriptor->range_end;
}

static bool folder_queue_build_descriptor(FolderQueueDescriptor *out_descriptor)
{
    if (out_descriptor == nullptr || !playlist_catalog_generation_valid()) return false;

    FolderQueueDescriptor descriptor = {};
    descriptor.snapshot.ready = true;
    descriptor.snapshot.preferred_scope = g_folder_scope;
    descriptor.snapshot.effective_scope = g_folder_scope;
    descriptor.snapshot.catalog_generation = g_context.catalog_generation;

    const size_t library_count = media_library_get_count();

    // 总列表不是强制“全部歌曲”，而是继续使用现有全部歌曲/歌手/专辑/年代上下文。
    if (g_folder_scope == PlayerFolderScope::All) {
        PlayerListSnapshot playlist = {};
        if (!player_playlist_get_snapshot(&playlist)) return false;
        descriptor.snapshot.effective_scope = PlayerFolderScope::All;
        descriptor.snapshot.catalog_generation = playlist.catalog_generation;
        descriptor.snapshot.position = playlist.position;
        descriptor.snapshot.track_count = playlist.track_count;
        descriptor.snapshot.track_index = playlist.track_index;
        descriptor.snapshot.current_in_queue = playlist.track_count > 0U && playlist.track_index != UINT32_MAX;
        descriptor.snapshot.context_id = folder_playlist_context_id(playlist);
        *out_descriptor = descriptor;
        return true;
    }

    if (library_count == 0U) {
        descriptor.snapshot.track_count = 0U;
        descriptor.snapshot.track_index = UINT32_MAX;
        descriptor.snapshot.current_in_queue = false;
        folder_descriptor_set_all_library(&descriptor);
        *out_descriptor = descriptor;
        return true;
    }

    uint32_t current_track = UINT32_MAX;
    if (!playlist_resolve(&current_track, nullptr, nullptr, nullptr, nullptr) ||
        current_track >= library_count) {
        return false;
    }
    descriptor.snapshot.track_index = current_track;

    const char *selected_path = g_folder_scope == PlayerFolderScope::Level1
        ? g_folder_level1_path
        : g_folder_level2_path;
    uint32_t selected_count = 0U;
    if (!folder_selection_track_count(g_folder_scope, selected_path, &selected_count)) {
        // 目录被删除/改名或尚未选择时，仅运行期回退总曲库；NVS 用户意图保留，设置页会显示“需重新选择”。
        folder_descriptor_set_all_library(&descriptor);
        descriptor.snapshot.position = current_track;
        descriptor.snapshot.track_count = static_cast<uint32_t>(library_count);
        *out_descriptor = descriptor;
        return true;
    }

    descriptor.prefix_path = selected_path;
    descriptor.prefix_length = strlen(selected_path);
    descriptor.direct_only = g_folder_scope == PlayerFolderScope::Level2;
    descriptor.snapshot.context_id = folder_context_hash(selected_path, descriptor.prefix_length);
    if (!folder_find_prefix_range(
            descriptor.prefix_path,
            descriptor.prefix_length,
            &descriptor.range_start,
            &descriptor.range_end)) {
        return false;
    }

    uint32_t position = 0U;
    uint32_t count = 0U;
    bool current_found = false;
    if (!descriptor.direct_only) {
        count = descriptor.range_end - descriptor.range_start;
        current_found = current_track >= descriptor.range_start && current_track < descriptor.range_end;
        if (current_found) position = current_track - descriptor.range_start;
    } else {
        for (uint32_t i = descriptor.range_start; i < descriptor.range_end; ++i) {
            if (!folder_path_matches(media_library_get_path(i), descriptor)) continue;
            if (i == current_track) {
                position = count;
                current_found = true;
            }
            ++count;
        }
    }

    // R2 允许当前音频暂时位于新目录之外。此时保持 Player 当前 Track 不变，
    // 下一首/上一首/EOF 会分别从新队列首/尾进入，不再把目录选择降级掉。
    descriptor.snapshot.position = current_found ? position : 0U;
    descriptor.snapshot.track_count = count;
    descriptor.snapshot.current_in_queue = current_found;
    *out_descriptor = descriptor;
    return true;
}

} // namespace

const char *player_playlist_folder_scope_name(PlayerFolderScope scope)
{
    switch (scope) {
        case PlayerFolderScope::All: return "总列表";
        case PlayerFolderScope::Level1: return "一级列表";
        case PlayerFolderScope::Level2: return "二级列表";
    }
    return "未知范围";
}

bool player_playlist_set_folder_scope(PlayerFolderScope scope)
{
    if (!folder_scope_valid(scope)) {
        return false;
    }
    g_folder_scope = scope;

    PlayerFolderQueueSnapshot queue = {};
    if (player_playlist_get_folder_queue_snapshot(&queue)) {
        ESP_LOGI(TAG,
            "目录播放范围：设置=%s 实际=%s 位置=%lu/%lu in_queue=%u track=%lu context=%08lx",
            player_playlist_folder_scope_name(queue.preferred_scope),
            player_playlist_folder_scope_name(queue.effective_scope),
            static_cast<unsigned long>(queue.current_in_queue && queue.track_count > 0U ? queue.position + 1U : 0U),
            static_cast<unsigned long>(queue.track_count),
            queue.current_in_queue ? 1U : 0U,
            static_cast<unsigned long>(queue.track_index),
            static_cast<unsigned long>(queue.context_id));
    } else {
        ESP_LOGI(TAG, "目录播放范围：设置=%s；当前音乐库尚无有效队列",
            player_playlist_folder_scope_name(scope));
    }
    return true;
}

PlayerFolderScope player_playlist_get_folder_scope()
{
    return g_folder_scope;
}

bool player_playlist_set_folder_selection(PlayerFolderScope scope, const char *folder_path)
{
    uint32_t track_count = 0U;
    if ((scope != PlayerFolderScope::Level1 && scope != PlayerFolderScope::Level2) ||
        !folder_selection_track_count(scope, folder_path, &track_count)) {
        return false;
    }

    char *target = scope == PlayerFolderScope::Level1
        ? g_folder_level1_path
        : g_folder_level2_path;
    const size_t length = strlen(folder_path);
    if (length + 1U > PLAYER_FOLDER_PATH_MAX) return false;
    memcpy(target, folder_path, length + 1U);

    ESP_LOGI(TAG, "目录选择：范围=%s tracks=%lu path=%s",
        player_playlist_folder_scope_name(scope),
        static_cast<unsigned long>(track_count),
        target);
    return true;
}

bool player_playlist_copy_folder_selection(
    PlayerFolderScope scope, char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U) return false;
    const char *source = nullptr;
    if (scope == PlayerFolderScope::Level1) source = g_folder_level1_path;
    else if (scope == PlayerFolderScope::Level2) source = g_folder_level2_path;
    else return false;
    if (source[0] == '\0' || strlen(source) + 1U > buffer_size) return false;
    snprintf(buffer, buffer_size, "%s", source);
    return true;
}

bool player_playlist_folder_selection_available(
    PlayerFolderScope scope, const char *folder_path, uint32_t *out_track_count)
{
    return folder_selection_track_count(scope, folder_path, out_track_count);
}

size_t player_playlist_get_folder_option_count(
    PlayerFolderScope scope, const char *parent_path)
{
    return folder_option_scan(scope, parent_path, 0U, false, nullptr, 0U, nullptr);
}

bool player_playlist_copy_folder_option_at(
    PlayerFolderScope scope,
    const char *parent_path,
    size_t position,
    char *path_buffer,
    size_t path_buffer_size,
    uint32_t *out_track_count)
{
    if (path_buffer == nullptr || path_buffer_size == 0U) return false;
    path_buffer[0] = '\0';
    uint32_t count = 0U;
    const size_t options = folder_option_scan(
        scope, parent_path, position, true, path_buffer, path_buffer_size, &count);
    if (position >= options || path_buffer[0] == '\0') return false;
    if (out_track_count != nullptr) *out_track_count = count;
    return true;
}

bool player_playlist_get_folder_queue_snapshot(PlayerFolderQueueSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }
    FolderQueueDescriptor descriptor = {};
    if (!folder_queue_build_descriptor(&descriptor)) {
        return false;
    }
    *out_snapshot = descriptor.snapshot;
    return true;
}

bool player_playlist_get_folder_queue_track_index_at_position(
    size_t position,
    size_t *out_track_index)
{
    if (out_track_index == nullptr || position > UINT32_MAX) {
        return false;
    }

    FolderQueueDescriptor descriptor = {};
    if (!folder_queue_build_descriptor(&descriptor) ||
        position >= descriptor.snapshot.track_count) {
        return false;
    }

    if (descriptor.snapshot.preferred_scope == PlayerFolderScope::All) {
        return player_playlist_get_track_index_at_position(position, out_track_index);
    }

    if (descriptor.snapshot.effective_scope == PlayerFolderScope::All) {
        *out_track_index = position;
        return true;
    }

    if (!descriptor.direct_only) {
        const uint32_t track_index = descriptor.range_start + static_cast<uint32_t>(position);
        if (track_index >= descriptor.range_end) {
            return false;
        }
        *out_track_index = track_index;
        return true;
    }

    uint32_t matched_position = 0U;
    for (uint32_t i = descriptor.range_start; i < descriptor.range_end; ++i) {
        if (!folder_path_matches(media_library_get_path(i), descriptor)) {
            continue;
        }
        if (matched_position == position) {
            *out_track_index = i;
            return true;
        }
        ++matched_position;
    }
    return false;
}

bool player_playlist_select_folder_queue_position(size_t position)
{
    FolderQueueDescriptor descriptor = {};
    if (!folder_queue_build_descriptor(&descriptor) ||
        position >= descriptor.snapshot.track_count) {
        return false;
    }

    if (descriptor.snapshot.preferred_scope == PlayerFolderScope::All) {
        // 总列表下保持原有“全部歌曲/歌手/专辑/年代”身份。
        return player_playlist_select_position(position);
    }

    size_t track_index = 0U;
    if (!player_playlist_get_folder_queue_track_index_at_position(position, &track_index)) {
        return false;
    }

    // 一级/二级目录队列可能跨越原 Artist/Album/Decade Group；真正发生目录内切歌后
    // 收敛为全局 Track 身份，避免继续持有与当前歌曲不匹配的分类 Group。
    if (!playlist_bind_base(PlayerListType::AllTracks, UINT32_MAX, track_index)) {
        return false;
    }

#if APP_DIAG_PLAYER_PLAYLIST
    PlayerFolderQueueSnapshot queue = {};
    if (player_playlist_get_folder_queue_snapshot(&queue)) {
        ESP_LOGI(TAG, "目录队列选择：范围=%s 位置=%lu/%lu track=%lu",
            player_playlist_folder_scope_name(queue.effective_scope),
            static_cast<unsigned long>(queue.position + 1U),
            static_cast<unsigned long>(queue.track_count),
            static_cast<unsigned long>(queue.track_index));
    }
#endif
    return true;
}

static bool player_playlist_folder_move(int direction)
{
    PlayerFolderQueueSnapshot queue = {};
    if (!player_playlist_get_folder_queue_snapshot(&queue) || queue.track_count == 0U) {
        return false;
    }

    uint32_t target_position = 0U;
    if (!queue.current_in_queue) {
        // 用户刚在设置里切到另一个目录时不打断当前音频：下一首从目录首开始，上一首从目录尾开始。
        target_position = direction < 0 ? queue.track_count - 1U : 0U;
    } else if (direction < 0) {
        target_position = queue.position == 0U ? queue.track_count - 1U : queue.position - 1U;
    } else {
        target_position = (queue.position + 1U) % queue.track_count;
    }
    return player_playlist_select_folder_queue_position(target_position);
}

bool player_playlist_folder_previous()
{
    return player_playlist_folder_move(-1);
}

bool player_playlist_folder_next()
{
    return player_playlist_folder_move(1);
}
