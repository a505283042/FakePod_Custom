#include "player_state.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "nas_library_source.h"

static const char *TAG = "播放器状态";
static bool g_ready = false;
static PlayerMediaSource g_source = PlayerMediaSource::Local;

struct NasListContext
{
    bool ready = false;
    PlayerListType type = PlayerListType::AllTracks;
    uint32_t catalog_generation = 0U;
    uint32_t group_index = UINT32_MAX;
    uint32_t group_id = UINT32_MAX;
    uint32_t position = 0U;
    uint16_t decade_start = 0U;
    bool decade_unknown = false;
};

static NasListContext g_nas = {};

const char *player_media_source_name(PlayerMediaSource source)
{
    return source == PlayerMediaSource::Nas ? "NAS" : "LOCAL";
}

static bool nas_context_valid()
{
    return g_nas.ready && nas_library_source_ready() &&
        g_nas.catalog_generation != 0U &&
        g_nas.catalog_generation == nas_library_source_generation();
}

static bool nas_resolve(uint32_t *out_track, uint32_t *out_count)
{
    if (!nas_context_valid()) return false;

    uint32_t track = UINT32_MAX;
    uint32_t count = 0U;
    switch (g_nas.type) {
        case PlayerListType::AllTracks:
            count = nas_library_source_track_count();
            if (g_nas.position >= count) return false;
            track = g_nas.position;
            break;
        case PlayerListType::Artist:
        {
            MediaArtistGroupViewV2 view = {};
            if (!nas_library_source_get_artist(g_nas.group_index, &view) ||
                view.generation != g_nas.catalog_generation ||
                view.artist_id != g_nas.group_id ||
                view.track_indices == nullptr || g_nas.position >= view.track_count) return false;
            count = view.track_count;
            track = view.track_indices[g_nas.position];
            break;
        }
        case PlayerListType::Album:
        {
            MediaAlbumGroupViewV2 view = {};
            if (!nas_library_source_get_album(g_nas.group_index, &view) ||
                view.generation != g_nas.catalog_generation ||
                view.album_id != g_nas.group_id ||
                view.track_indices == nullptr || g_nas.position >= view.track_count) return false;
            count = view.track_count;
            track = view.track_indices[g_nas.position];
            break;
        }
        case PlayerListType::Decade:
        {
            MediaDecadeGroupViewV2 view = {};
            if (!nas_library_source_get_decade(g_nas.group_index, &view) ||
                view.generation != g_nas.catalog_generation ||
                view.track_indices == nullptr || g_nas.position >= view.track_count ||
                view.decade_start != g_nas.decade_start || view.unknown != g_nas.decade_unknown) return false;
            count = view.track_count;
            track = view.track_indices[g_nas.position];
            break;
        }
    }
    if (track >= nas_library_source_track_count()) return false;
    if (out_track != nullptr) *out_track = track;
    if (out_count != nullptr) *out_count = count;
    return true;
}

static bool nas_bind(PlayerListType type, size_t group_index, size_t position)
{
    if (!nas_library_source_ready() || group_index > UINT32_MAX || position > UINT32_MAX) return false;
    NasListContext next = {};
    next.ready = true;
    next.type = type;
    next.catalog_generation = nas_library_source_generation();
    next.group_index = static_cast<uint32_t>(group_index);
    next.position = static_cast<uint32_t>(position);
    next.group_id = UINT32_MAX;

    switch (type) {
        case PlayerListType::AllTracks:
            if (position >= nas_library_source_track_count()) return false;
            next.group_index = UINT32_MAX;
            break;
        case PlayerListType::Artist:
        {
            MediaArtistGroupViewV2 view = {};
            if (!nas_library_source_get_artist(group_index, &view) || position >= view.track_count) return false;
            next.group_id = view.artist_id;
            break;
        }
        case PlayerListType::Album:
        {
            MediaAlbumGroupViewV2 view = {};
            if (!nas_library_source_get_album(group_index, &view) || position >= view.track_count) return false;
            next.group_id = view.album_id;
            break;
        }
        case PlayerListType::Decade:
        {
            MediaDecadeGroupViewV2 view = {};
            if (!nas_library_source_get_decade(group_index, &view) || position >= view.track_count) return false;
            next.decade_start = view.decade_start;
            next.decade_unknown = view.unknown;
            break;
        }
    }

    g_nas = next;
    uint32_t track = UINT32_MAX;
    if (!nas_resolve(&track, nullptr)) {
        g_nas = {};
        return false;
    }
    g_source = PlayerMediaSource::Nas;
    return true;
}

static bool nas_get_list_snapshot(PlayerListSnapshot *out)
{
    if (out == nullptr || !nas_context_valid()) return false;
    uint32_t track = UINT32_MAX;
    uint32_t count = 0U;
    if (!nas_resolve(&track, &count)) return false;
    PlayerListSnapshot s = {};
    s.ready = true;
    s.type = g_nas.type;
    s.catalog_generation = g_nas.catalog_generation;
    s.group_index = g_nas.group_index;
    s.group_id = g_nas.group_id;
    s.position = g_nas.position;
    s.track_count = count;
    s.track_index = track;
    s.decade_start = g_nas.decade_start;
    s.decade_unknown = g_nas.decade_unknown;
    *out = s;
    return true;
}

static bool nas_track_at_list_position(size_t position, size_t *out_track)
{
    if (out_track == nullptr || position > UINT32_MAX || !nas_context_valid()) return false;
    uint32_t track = UINT32_MAX;
    switch (g_nas.type) {
        case PlayerListType::AllTracks:
            if (position >= nas_library_source_track_count()) return false;
            track = static_cast<uint32_t>(position);
            break;
        case PlayerListType::Artist:
        {
            MediaArtistGroupViewV2 view = {};
            if (!nas_library_source_get_artist(g_nas.group_index, &view) || position >= view.track_count) return false;
            track = view.track_indices[position];
            break;
        }
        case PlayerListType::Album:
        {
            MediaAlbumGroupViewV2 view = {};
            if (!nas_library_source_get_album(g_nas.group_index, &view) || position >= view.track_count) return false;
            track = view.track_indices[position];
            break;
        }
        case PlayerListType::Decade:
        {
            MediaDecadeGroupViewV2 view = {};
            if (!nas_library_source_get_decade(g_nas.group_index, &view) || position >= view.track_count) return false;
            track = view.track_indices[position];
            break;
        }
    }
    if (track >= nas_library_source_track_count()) return false;
    *out_track = track;
    return true;
}

static bool nas_get_folder_queue(PlayerFolderQueueSnapshot *out)
{
    if (out == nullptr || !nas_context_valid()) return false;
    const PlayerFolderScope scope = nas_library_source_folder_scope();
    if (scope == PlayerFolderScope::All) {
        PlayerListSnapshot list = {};
        if (!nas_get_list_snapshot(&list)) return false;
        PlayerFolderQueueSnapshot q = {};
        q.ready = true;
        q.preferred_scope = PlayerFolderScope::All;
        q.effective_scope = PlayerFolderScope::All;
        q.catalog_generation = list.catalog_generation;
        q.context_id = 0x4E415300U ^ static_cast<uint32_t>(list.type) ^ list.group_index ^ list.group_id;
        q.position = list.position;
        q.track_count = list.track_count;
        q.track_index = list.track_index;
        q.current_in_queue = list.track_count > 0U && list.track_index != UINT32_MAX;
        *out = q;
        return true;
    }

    uint32_t current = UINT32_MAX;
    if (!nas_resolve(&current, nullptr)) return false;
    const uint32_t count = nas_library_source_scoped_track_count();
    PlayerFolderQueueSnapshot q = {};
    q.ready = true;
    q.preferred_scope = scope;
    q.effective_scope = scope;
    q.catalog_generation = nas_library_source_generation();
    q.context_id = nas_library_source_folder_context_id() ^ 0x4E415300U;
    q.track_count = count;
    q.track_index = current;
    q.current_in_queue = false;
    for (uint32_t i = 0U; i < count; ++i) {
        uint32_t track = UINT32_MAX;
        if (nas_library_source_scoped_track_at(i, &track) && track == current) {
            q.position = i;
            q.current_in_queue = true;
            break;
        }
    }
    *out = q;
    return true;
}

static bool nas_select_folder_position(size_t position)
{
    if (!nas_context_valid() || position > UINT32_MAX) return false;
    const PlayerFolderScope scope = nas_library_source_folder_scope();
    if (scope == PlayerFolderScope::All) {
        size_t track = 0U;
        if (!nas_track_at_list_position(position, &track)) return false;
        g_nas.position = static_cast<uint32_t>(position);
        return true;
    }
    uint32_t track = UINT32_MAX;
    if (!nas_library_source_scoped_track_at(static_cast<uint32_t>(position), &track)) return false;
    return nas_bind(PlayerListType::AllTracks, UINT32_MAX, track);
}

static void player_state_log_current(const char *action)
{
    PlayerFolderQueueSnapshot queue = {};
    if (player_state_get_folder_queue_snapshot(&queue) && queue.track_count > 0U && queue.track_index != UINT32_MAX) {
        const char *path = player_state_get_path();
        ESP_LOGI(TAG, "%s：来源=%s 范围=%s 实际=%s 位置=%lu/%lu track=%lu %s",
            action != nullptr ? action : "选择",
            player_media_source_name(g_source),
            player_playlist_folder_scope_name(queue.preferred_scope),
            player_playlist_folder_scope_name(queue.effective_scope),
            static_cast<unsigned long>(queue.current_in_queue ? queue.position + 1U : 0U),
            static_cast<unsigned long>(queue.track_count),
            static_cast<unsigned long>(queue.track_index),
            path != nullptr ? path : "<invalid>");
    }
}

esp_err_t player_state_init()
{
    if (!media_library_is_ready()) {
        ESP_LOGE(TAG, "音乐库尚未就绪");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = player_playlist_init();
    if (ret != ESP_OK) return ret;
    g_source = PlayerMediaSource::Local;
    g_nas = {};
    g_ready = true;
    player_state_log_current("当前歌曲");
    return ESP_OK;
}

bool player_state_is_ready()
{
    if (!g_ready) return false;
    return g_source == PlayerMediaSource::Nas ? nas_context_valid() : player_playlist_is_ready();
}

static bool player_state_find_track_by_path(const char *path, size_t *out_track_index)
{
    if (path == nullptr || path[0] == '\0' || out_track_index == nullptr) return false;
    size_t lo = 0U, hi = media_library_get_count();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        const char *candidate = media_library_get_path(mid);
        if (candidate == nullptr) return false;
        const int cmp = strcasecmp(candidate, path);
        if (cmp < 0) lo = mid + 1U; else hi = mid;
    }
    if (lo >= media_library_get_count()) return false;
    const char *candidate = media_library_get_path(lo);
    if (candidate == nullptr || strcasecmp(candidate, path) != 0) return false;
    *out_track_index = lo;
    return true;
}

bool player_state_rebind_after_catalog_reload(const char *preferred_path, size_t fallback_track_index)
{
    if (!media_library_is_ready()) return false;
    const PlayerMediaSource previous_source = g_source;
    const esp_err_t ret = player_playlist_init();
    if (ret != ESP_OK) return false;
    g_ready = true;
    if (media_library_get_count() > 0U) {
        size_t restored = 0U;
        const bool found = player_state_find_track_by_path(preferred_path, &restored);
        if (!found) restored = fallback_track_index < media_library_get_count() ? fallback_track_index : media_library_get_count() - 1U;
        if (!player_playlist_bind_all(restored)) return false;
    }
    g_source = previous_source == PlayerMediaSource::Nas && nas_context_valid()
        ? PlayerMediaSource::Nas : PlayerMediaSource::Local;
    return true;
}

PlayerMediaSource player_state_get_source() { return g_source; }

size_t player_state_get_index()
{
    if (!g_ready) return 0U;
    if (g_source == PlayerMediaSource::Nas) {
        uint32_t track = 0U;
        return nas_resolve(&track, nullptr) ? static_cast<size_t>(track) : 0U;
    }
    size_t track = 0U;
    return player_playlist_get_track_index(&track) ? track : 0U;
}

bool player_state_get_track_view(MediaTrackViewV2 *out_view)
{
    if (out_view == nullptr || !player_state_is_ready()) return false;
    const size_t track = player_state_get_index();
    return g_source == PlayerMediaSource::Nas
        ? nas_library_source_get_track_view(static_cast<uint32_t>(track), out_view)
        : media_catalog_v2_get_track_view(track, out_view);
}

const char *player_state_get_path()
{
    MediaTrackViewV2 view = {};
    return player_state_get_track_view(&view) ? view.path : nullptr;
}

MediaFormat player_state_get_format()
{
    MediaTrackViewV2 view = {};
    return player_state_get_track_view(&view) && view.row != nullptr ? view.row->format : MediaFormat::Unknown;
}

bool player_state_get_technical_info(MediaTechnicalInfo *out_info)
{
    if (out_info == nullptr || !player_state_is_ready()) return false;
    MediaTrackViewV2 view = {};
    if (!player_state_get_track_view(&view) || view.row == nullptr) return false;
    *out_info = view.row->technical;
    return true;
}

bool player_state_get_list_snapshot(PlayerListSnapshot *out_snapshot)
{
    return g_source == PlayerMediaSource::Nas ? nas_get_list_snapshot(out_snapshot)
        : (g_ready && player_playlist_get_snapshot(out_snapshot));
}

size_t player_state_get_list_position()
{
    PlayerFolderQueueSnapshot q = {};
    return player_state_get_folder_queue_snapshot(&q) ? q.position : 0U;
}

size_t player_state_get_list_count()
{
    PlayerFolderQueueSnapshot q = {};
    return player_state_get_folder_queue_snapshot(&q) ? q.track_count : 0U;
}

PlayerListType player_state_get_list_type()
{
    PlayerListSnapshot s = {};
    return player_state_get_list_snapshot(&s) ? s.type : PlayerListType::AllTracks;
}

bool player_state_copy_list_label(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U || !player_state_is_ready()) return false;
    PlayerFolderQueueSnapshot q = {};
    if (player_state_get_folder_queue_snapshot(&q) && q.effective_scope != PlayerFolderScope::All) {
        char path[PLAYER_FOLDER_PATH_MAX] = {};
        if (player_state_copy_folder_selection(q.effective_scope, path, sizeof(path))) {
            size_t n = strlen(path);
            while (n > 0U && path[n - 1U] == '/') path[--n] = '\0';
            const char *slash = strrchr(path, '/');
            snprintf(buffer, buffer_size, "%s", slash != nullptr ? slash + 1U : path);
            return true;
        }
    }
    if (g_source == PlayerMediaSource::Local) return player_playlist_copy_label(buffer, buffer_size);
    PlayerListSnapshot s = {};
    if (!nas_get_list_snapshot(&s)) return false;
    if (s.type == PlayerListType::AllTracks) { snprintf(buffer, buffer_size, "全部歌曲"); return true; }
    if (s.type == PlayerListType::Artist) { MediaArtistGroupViewV2 v={}; if (!nas_library_source_get_artist(s.group_index,&v)) return false; snprintf(buffer,buffer_size,"%s",v.name?v.name:""); return true; }
    if (s.type == PlayerListType::Album) { MediaAlbumGroupViewV2 v={}; if (!nas_library_source_get_album(s.group_index,&v)) return false; snprintf(buffer,buffer_size,"%s",v.title?v.title:""); return true; }
    if (s.decade_unknown) snprintf(buffer,buffer_size,"未知年代"); else snprintf(buffer,buffer_size,"%u年代",static_cast<unsigned>(s.decade_start));
    return true;
}

bool player_state_set_folder_scope(PlayerFolderScope scope)
{
    if (!g_ready) return false;
    return g_source == PlayerMediaSource::Nas
        ? nas_library_source_set_folder_scope(scope)
        : player_playlist_set_folder_scope(scope);
}

PlayerFolderScope player_state_get_folder_scope()
{
    return g_source == PlayerMediaSource::Nas ? nas_library_source_folder_scope() : player_playlist_get_folder_scope();
}

bool player_state_set_folder_selection(PlayerFolderScope scope, const char *folder_path)
{
    if (!g_ready) return false;
    return g_source == PlayerMediaSource::Nas
        ? nas_library_source_set_folder_selection(scope, folder_path)
        : player_playlist_set_folder_selection(scope, folder_path);
}

bool player_state_copy_folder_selection(PlayerFolderScope scope, char *buffer, size_t buffer_size)
{
    if (!g_ready) return false;
    return g_source == PlayerMediaSource::Nas
        ? nas_library_source_copy_folder_selection(scope, buffer, buffer_size)
        : player_playlist_copy_folder_selection(scope, buffer, buffer_size);
}

bool player_state_get_folder_queue_snapshot(PlayerFolderQueueSnapshot *out_snapshot)
{
    return g_source == PlayerMediaSource::Nas ? nas_get_folder_queue(out_snapshot)
        : (g_ready && player_playlist_get_folder_queue_snapshot(out_snapshot));
}

static bool player_state_select(bool selected, const char *action)
{
    if (!g_ready || !selected) return false;
    player_state_log_current(action);
    return true;
}

bool player_state_get_folder_queue_track_index_at_position(size_t position, size_t *out_track_index)
{
    if (out_track_index == nullptr || !player_state_is_ready()) return false;
    if (g_source == PlayerMediaSource::Local) {
        return player_playlist_get_folder_queue_track_index_at_position(position, out_track_index);
    }
    const PlayerFolderScope scope = nas_library_source_folder_scope();
    if (scope == PlayerFolderScope::All) {
        return nas_track_at_list_position(position, out_track_index);
    }
    if (position > UINT32_MAX) return false;
    uint32_t track = UINT32_MAX;
    if (!nas_library_source_scoped_track_at(static_cast<uint32_t>(position), &track)) return false;
    *out_track_index = static_cast<size_t>(track);
    return true;
}

bool player_state_select_folder_queue_position(size_t position)
{
    return player_state_select(g_source == PlayerMediaSource::Nas
        ? nas_select_folder_position(position)
        : player_playlist_select_folder_queue_position(position), "目录播放范围选择");
}

bool player_state_select_all_tracks(size_t position)
{
    const bool ok = player_playlist_bind_all(position);
    if (ok) g_source = PlayerMediaSource::Local;
    return player_state_select(ok, "切换全部歌曲");
}

bool player_state_select_artist_group(size_t group_index, size_t position)
{
    const bool ok = player_playlist_bind_artist(group_index, position);
    if (ok) g_source = PlayerMediaSource::Local;
    return player_state_select(ok, "切换歌手列表");
}

bool player_state_select_album_group(size_t group_index, size_t position)
{
    const bool ok = player_playlist_bind_album(group_index, position);
    if (ok) g_source = PlayerMediaSource::Local;
    return player_state_select(ok, "切换专辑列表");
}

bool player_state_select_decade_group(size_t group_index, size_t position)
{
    const bool ok = player_playlist_bind_decade(group_index, position);
    if (ok) g_source = PlayerMediaSource::Local;
    return player_state_select(ok, "切换年代列表");
}

bool player_state_select_nas_all_tracks(size_t position)
{
    return player_state_select(nas_bind(PlayerListType::AllTracks, UINT32_MAX, position), "切换NAS全部歌曲");
}

bool player_state_select_nas_folder_queue_position(size_t position)
{
    if (!nas_context_valid()) {
        if (!nas_bind(PlayerListType::AllTracks, UINT32_MAX, 0U)) return false;
    }
    g_source = PlayerMediaSource::Nas;
    return player_state_select(nas_select_folder_position(position), "NAS目录播放范围选择");
}

bool player_state_select_nas_artist_group(size_t group_index, size_t position)
{
    return player_state_select(nas_bind(PlayerListType::Artist, group_index, position), "切换NAS歌手列表");
}

bool player_state_select_nas_album_group(size_t group_index, size_t position)
{
    return player_state_select(nas_bind(PlayerListType::Album, group_index, position), "切换NAS专辑列表");
}

bool player_state_select_nas_decade_group(size_t group_index, size_t position)
{
    return player_state_select(nas_bind(PlayerListType::Decade, group_index, position), "切换NAS年代列表");
}

bool player_state_select_position(size_t position)
{
    if (g_source == PlayerMediaSource::Nas) {
        size_t track = 0U;
        if (!nas_track_at_list_position(position, &track)) return false;
        g_nas.position = static_cast<uint32_t>(position);
        return player_state_select(true, "直接选择NAS列表位置");
    }
    return player_state_select(player_playlist_select_position(position), "直接选择列表位置");
}

static bool state_move(int direction)
{
    PlayerFolderQueueSnapshot q = {};
    if (!player_state_get_folder_queue_snapshot(&q) || q.track_count == 0U) return false;
    uint32_t target = 0U;
    if (!q.current_in_queue) target = direction < 0 ? q.track_count - 1U : 0U;
    else if (direction < 0) target = q.position == 0U ? q.track_count - 1U : q.position - 1U;
    else target = (q.position + 1U) % q.track_count;
    return player_state_select_folder_queue_position(target);
}

bool player_state_previous()
{
    if (!g_ready) return false;
    if (g_source == PlayerMediaSource::Nas) return state_move(-1);
    if (!player_playlist_folder_previous()) return false;
    player_state_log_current("上一首");
    return true;
}

bool player_state_next()
{
    if (!g_ready) return false;
    if (g_source == PlayerMediaSource::Nas) return state_move(+1);
    if (!player_playlist_folder_next()) return false;
    player_state_log_current("下一首");
    return true;
}
