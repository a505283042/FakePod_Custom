#include "nas_library_source.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_catalog_store_v2.h"
#include "nas_catalog_service.h"
#include "system_paths.h"

static const char *TAG = "NAS曲库源";

namespace {

static MusicCatalogV2 g_catalog = {};
static uint32_t g_generation = 0U;
static size_t g_psram_bytes = 0U;
static PlayerFolderScope g_scope = PlayerFolderScope::All;
static char g_level1_path[PLAYER_FOLDER_PATH_MAX] = {};
static char g_level2_path[PLAYER_FOLDER_PATH_MAX] = {};
static uint32_t *g_scope_tracks = nullptr;
static uint32_t g_scope_track_count = 0U;

static const char *pool_str(uint32_t off)
{
    const char *value = media_catalog_v2_pool_str(&g_catalog, off);
    return value != nullptr ? value : "";
}

static bool path_has_prefix(const char *path, const char *prefix)
{
    if (path == nullptr || prefix == nullptr || prefix[0] == '\0') return false;
    return strncmp(path, prefix, strlen(prefix)) == 0;
}

static bool extract_level1(const char *path, const char **out_start, size_t *out_len, const char **out_after)
{
    if (path == nullptr || path[0] != '/') return false;
    const char *start = path + 1;
    const char *slash = strchr(start, '/');
    if (slash == nullptr || slash == start) return false;
    if (out_start != nullptr) *out_start = start;
    if (out_len != nullptr) *out_len = static_cast<size_t>(slash - start);
    if (out_after != nullptr) *out_after = slash + 1;
    return true;
}

static bool same_component(const char *a, size_t a_len, const char *b, size_t b_len)
{
    return a != nullptr && b != nullptr && a_len == b_len && strncmp(a, b, a_len) == 0;
}

static bool copy_component_path(char *out, size_t out_size, const char *prefix, const char *name, size_t name_len)
{
    if (out == nullptr || out_size == 0U || name == nullptr || name_len == 0U) return false;
    const int written = prefix != nullptr
        ? snprintf(out, out_size, "%s%.*s/", prefix, static_cast<int>(name_len), name)
        : snprintf(out, out_size, "/%.*s/", static_cast<int>(name_len), name);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

static bool level1_option_at(size_t wanted, bool require_level2, char *out_path, size_t out_path_size, uint32_t *out_count)
{
    size_t visible = 0U;
    uint32_t i = 0U;
    while (i < g_catalog.track_count) {
        const char *path = pool_str(g_catalog.tracks[i].path_off);
        const char *name = nullptr;
        size_t name_len = 0U;
        if (!extract_level1(path, &name, &name_len, nullptr)) {
            ++i;
            continue;
        }

        uint32_t track_count = 0U;
        bool has_level2 = false;
        uint32_t j = i;
        while (j < g_catalog.track_count) {
            const char *candidate = pool_str(g_catalog.tracks[j].path_off);
            const char *candidate_name = nullptr;
            size_t candidate_len = 0U;
            const char *candidate_after1 = nullptr;
            if (!extract_level1(candidate, &candidate_name, &candidate_len, &candidate_after1) ||
                !same_component(name, name_len, candidate_name, candidate_len)) {
                break;
            }
            ++track_count;
            if (candidate_after1 != nullptr) {
                const char *slash2 = strchr(candidate_after1, '/');
                if (slash2 != nullptr && slash2 != candidate_after1) has_level2 = true;
            }
            ++j;
        }

        if (!require_level2 || has_level2) {
            if (visible++ == wanted) {
                if (!copy_component_path(out_path, out_path_size, nullptr, name, name_len)) return false;
                if (out_count != nullptr) *out_count = track_count;
                return true;
            }
        }
        i = j > i ? j : i + 1U;
    }
    return false;
}

static bool level2_option_at(
    const char *parent_path,
    size_t wanted,
    char *out_path,
    size_t out_path_size,
    uint32_t *out_count)
{
    if (parent_path == nullptr || parent_path[0] == '\0') return false;
    const size_t prefix_len = strlen(parent_path);
    size_t visible = 0U;
    uint32_t i = 0U;
    while (i < g_catalog.track_count) {
        const char *path = pool_str(g_catalog.tracks[i].path_off);
        if (!path_has_prefix(path, parent_path)) {
            ++i;
            continue;
        }
        const char *rest = path + prefix_len;
        const char *slash = strchr(rest, '/');
        if (slash == nullptr || slash == rest) {
            ++i;
            continue;
        }
        const size_t name_len = static_cast<size_t>(slash - rest);

        uint32_t direct_count = 0U;
        uint32_t j = i;
        while (j < g_catalog.track_count) {
            const char *candidate = pool_str(g_catalog.tracks[j].path_off);
            if (!path_has_prefix(candidate, parent_path)) break;
            const char *candidate_rest = candidate + prefix_len;
            const char *candidate_slash = strchr(candidate_rest, '/');
            if (candidate_slash == nullptr ||
                !same_component(rest, name_len, candidate_rest,
                    static_cast<size_t>(candidate_slash - candidate_rest))) {
                break;
            }
            const char *after = candidate_slash + 1;
            if (after[0] != '\0' && strchr(after, '/') == nullptr) ++direct_count;
            ++j;
        }

        if (direct_count > 0U && visible++ == wanted) {
            if (!copy_component_path(out_path, out_path_size, parent_path, rest, name_len)) return false;
            if (out_count != nullptr) *out_count = direct_count;
            return true;
        }
        i = j > i ? j : i + 1U;
    }
    return false;
}

static bool path_matches_scope(const char *path)
{
    if (g_scope == PlayerFolderScope::All) return true;
    if (g_scope == PlayerFolderScope::Level1) {
        return g_level1_path[0] != '\0' && path_has_prefix(path, g_level1_path);
    }
    if (g_level2_path[0] == '\0' || !path_has_prefix(path, g_level2_path)) return false;
    const char *rest = path + strlen(g_level2_path);
    return rest[0] != '\0' && strchr(rest, '/') == nullptr;
}

static void release_scope_tracks()
{
    heap_caps_free(g_scope_tracks);
    g_scope_tracks = nullptr;
    g_scope_track_count = 0U;
}

static bool rebuild_scope_tracks()
{
    release_scope_tracks();
    if (g_scope == PlayerFolderScope::All) {
        g_scope_track_count = g_catalog.track_count;
        return true;
    }

    uint32_t count = 0U;
    for (uint32_t i = 0U; i < g_catalog.track_count; ++i) {
        if (path_matches_scope(pool_str(g_catalog.tracks[i].path_off))) ++count;
    }
    if (count == 0U) return true;

    g_scope_tracks = static_cast<uint32_t *>(
        heap_caps_malloc(static_cast<size_t>(count) * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_scope_tracks == nullptr) return false;

    uint32_t cursor = 0U;
    for (uint32_t i = 0U; i < g_catalog.track_count; ++i) {
        if (path_matches_scope(pool_str(g_catalog.tracks[i].path_off))) {
            g_scope_tracks[cursor++] = i;
        }
    }
    g_scope_track_count = cursor;
    return true;
}

static bool ensure_default_selection(PlayerFolderScope scope)
{
    if (scope == PlayerFolderScope::All) return true;
    if (scope == PlayerFolderScope::Level1) {
        uint32_t count = 0U;
        if (g_level1_path[0] != '\0' && nas_library_source_folder_selection_available(scope, g_level1_path, &count)) {
            return true;
        }
        return level1_option_at(0U, false, g_level1_path, sizeof(g_level1_path), nullptr);
    }

    uint32_t count = 0U;
    if (g_level2_path[0] != '\0' && nas_library_source_folder_selection_available(scope, g_level2_path, &count)) {
        return true;
    }
    if (g_level1_path[0] == '\0' ||
        nas_library_source_folder_option_count(PlayerFolderScope::Level2, g_level1_path) == 0U) {
        if (!level1_option_at(0U, true, g_level1_path, sizeof(g_level1_path), nullptr)) return false;
    }
    return level2_option_at(g_level1_path, 0U, g_level2_path, sizeof(g_level2_path), nullptr);
}

static uint32_t fnv1a32(const char *text, uint32_t hash)
{
    if (text == nullptr) return hash;
    while (*text != '\0') {
        hash ^= static_cast<uint8_t>(*text++);
        hash *= 16777619U;
    }
    return hash;
}

} // namespace

esp_err_t nas_library_source_open(
    PlayerFolderScope initial_scope,
    NasLibraryOpenStageCallback callback,
    void *callback_context)
{
    nas_library_source_close();
    NasCatalogSnapshot state = {};
    if (!nas_catalog_service_get_snapshot(&state) || !state.cached || state.track_count == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    const size_t before_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const int64_t open_started_us = esp_timer_get_time();
    int64_t stage_started_us = open_started_us;
    uint32_t index_crc = 0U;
    if (callback != nullptr) callback(NasLibraryOpenStage::ReadingCatalog, callback_context);
    esp_err_t ret = media_catalog_store_v2_load_catalog_only_cooperative(
        SystemPaths::kNasMusicIndexV2,
        SystemPaths::kNasMusicManifestV2,
        &g_catalog,
        &index_crc);
    if (ret != ESP_OK) {
        nas_library_source_close();
        return ret;
    }
    if (g_catalog.track_count != state.track_count) {
        nas_library_source_close();
        return ESP_ERR_INVALID_CRC;
    }
    const uint32_t catalog_ms =
        static_cast<uint32_t>((esp_timer_get_time() - stage_started_us) / 1000LL);

    // Catalog 的 SD 阶段已经结束；分组只做 PSRAM/CPU 工作。主动让出一次时间片后再进入分组，
    // 避免刚释放 SD 锁就继续在 Core1 上占用低优先级计算窗口。
    vTaskDelay(1);
    stage_started_us = esp_timer_get_time();
    if (callback != nullptr) callback(NasLibraryOpenStage::BuildingGroups, callback_context);
    ret = media_groups_v2_build(&g_catalog);
    if (ret != ESP_OK) {
        nas_library_source_close();
        return ret;
    }

    const uint32_t groups_ms =
        static_cast<uint32_t>((esp_timer_get_time() - stage_started_us) / 1000LL);
    vTaskDelay(1);
    stage_started_us = esp_timer_get_time();
    if (callback != nullptr) callback(NasLibraryOpenStage::PreparingScope, callback_context);

    g_generation = (index_crc ^ 0x80000000U);
    if (g_generation == 0U) g_generation = 1U;
    g_catalog.generation = g_generation;
    g_scope = initial_scope;
    if (g_scope != PlayerFolderScope::All &&
        g_scope != PlayerFolderScope::Level1 &&
        g_scope != PlayerFolderScope::Level2) {
        g_scope = PlayerFolderScope::All;
    }
    if (!ensure_default_selection(g_scope)) {
        ESP_LOGW(TAG, "NAS目录范围=%s无可用目录，本次回退总列表",
            player_playlist_folder_scope_name(g_scope));
        g_scope = PlayerFolderScope::All;
        g_level1_path[0] = '\0';
        g_level2_path[0] = '\0';
    }
    if (!rebuild_scope_tracks()) {
        nas_library_source_close();
        return ESP_ERR_NO_MEM;
    }

    const size_t after_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const uint32_t scope_ms =
        static_cast<uint32_t>((esp_timer_get_time() - stage_started_us) / 1000LL);
    const uint32_t total_ms =
        static_cast<uint32_t>((esp_timer_get_time() - open_started_us) / 1000LL);
    g_psram_bytes = before_psram >= after_psram ? before_psram - after_psram : 0U;
    ESP_LOGI(TAG,
        "共享曲库数据源已打开：tracks=%lu artists=%lu albums=%lu decades=%lu scope=%s psram=%uB groups=%uB internal=%u free_psram=%u stages=%lu/%lu/%lums total=%lums",
        static_cast<unsigned long>(g_catalog.track_count),
        static_cast<unsigned long>(g_catalog.artist_group_count),
        static_cast<unsigned long>(g_catalog.album_group_count),
        static_cast<unsigned long>(g_catalog.decade_group_count),
        player_playlist_folder_scope_name(g_scope),
        static_cast<unsigned>(g_psram_bytes),
        static_cast<unsigned>(media_groups_v2_psram_bytes(&g_catalog)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(after_psram),
        static_cast<unsigned long>(catalog_ms),
        static_cast<unsigned long>(groups_ms),
        static_cast<unsigned long>(scope_ms),
        static_cast<unsigned long>(total_ms));
    return ESP_OK;
}

void nas_library_source_close()
{
    release_scope_tracks();
    media_catalog_v2_release(&g_catalog);
    g_generation = 0U;
    g_psram_bytes = 0U;
    g_scope = PlayerFolderScope::All;
    g_level1_path[0] = '\0';
    g_level2_path[0] = '\0';
}

bool nas_library_source_ready()
{
    return g_generation != 0U && g_catalog.tracks != nullptr;
}

uint32_t nas_library_source_generation()
{
    return nas_library_source_ready() ? g_generation : 0U;
}

uint32_t nas_library_source_track_count()
{
    return nas_library_source_ready() ? g_catalog.track_count : 0U;
}

size_t nas_library_source_psram_bytes()
{
    return nas_library_source_ready() ? g_psram_bytes : 0U;
}

bool nas_library_source_get_track_view(uint32_t track_index, MediaTrackViewV2 *out_view)
{
    if (!nas_library_source_ready() || out_view == nullptr || track_index >= g_catalog.track_count) return false;
    const TrackRowV2 &track = g_catalog.tracks[track_index];
    MediaTrackViewV2 view = {};
    view.generation = g_generation;
    view.track_index = track_index;
    view.row = &track;
    view.path = pool_str(track.path_off);
    view.title = pool_str(track.title_off);
    view.artist = pool_str(track.display_artist_off);
    view.album = track.album_id == MEDIA_CATALOG_INVALID_ID_V2 || track.album_id >= g_catalog.album_count
        ? pool_str(0U) : pool_str(g_catalog.albums[track.album_id].title_off);
    *out_view = view;
    return true;
}

size_t nas_library_source_artist_count() { return nas_library_source_ready() ? g_catalog.artist_group_count : 0U; }
size_t nas_library_source_album_count() { return nas_library_source_ready() ? g_catalog.album_group_count : 0U; }
size_t nas_library_source_decade_count() { return nas_library_source_ready() ? g_catalog.decade_group_count : 0U; }

bool nas_library_source_get_artist(size_t group_index, MediaArtistGroupViewV2 *out_view)
{
    if (!nas_library_source_ready() || out_view == nullptr || group_index >= g_catalog.artist_group_count) return false;
    const MediaEntityTrackGroupV2 &group = g_catalog.artist_groups[group_index];
    if (group.entity_id >= g_catalog.artist_count) return false;
    MediaArtistGroupViewV2 view = {};
    view.generation = g_generation;
    view.artist_id = group.entity_id;
    view.name = pool_str(g_catalog.artists[group.entity_id].name_off);
    view.track_indices = g_catalog.artist_group_track_pool + group.track_index_start;
    view.track_count = group.track_count;
    *out_view = view;
    return true;
}

bool nas_library_source_get_album(size_t group_index, MediaAlbumGroupViewV2 *out_view)
{
    if (!nas_library_source_ready() || out_view == nullptr || group_index >= g_catalog.album_group_count) return false;
    const MediaEntityTrackGroupV2 &group = g_catalog.album_groups[group_index];
    if (group.entity_id >= g_catalog.album_count) return false;
    const AlbumRowV2 &album = g_catalog.albums[group.entity_id];
    MediaAlbumGroupViewV2 view = {};
    view.generation = g_generation;
    view.album_id = group.entity_id;
    view.title = pool_str(album.title_off);
    view.artist = pool_str(album.display_artist_off);
    view.track_indices = g_catalog.album_group_track_pool + group.track_index_start;
    view.track_count = group.track_count;
    view.flags = album.flags;
    view.release_year = album.release_year;
    view.original_year = album.original_year;
    *out_view = view;
    return true;
}

bool nas_library_source_get_decade(size_t group_index, MediaDecadeGroupViewV2 *out_view)
{
    if (!nas_library_source_ready() || out_view == nullptr || group_index >= g_catalog.decade_group_count) return false;
    const MediaDecadeTrackGroupV2 &group = g_catalog.decade_groups[group_index];
    MediaDecadeGroupViewV2 view = {};
    view.generation = g_generation;
    view.decade_start = group.decade_start;
    view.unknown = (group.flags & MEDIA_DECADE_GROUP_UNKNOWN_V2) != 0U;
    view.track_indices = g_catalog.decade_group_track_pool + group.track_index_start;
    view.track_count = group.track_count;
    *out_view = view;
    return true;
}

PlayerFolderScope nas_library_source_folder_scope()
{
    return g_scope;
}

bool nas_library_source_set_folder_scope(PlayerFolderScope scope)
{
    if (!nas_library_source_ready()) return false;
    if (scope != PlayerFolderScope::All && scope != PlayerFolderScope::Level1 && scope != PlayerFolderScope::Level2) return false;
    const PlayerFolderScope old = g_scope;
    g_scope = scope;
    if (!ensure_default_selection(scope) || !rebuild_scope_tracks()) {
        g_scope = old;
        (void)rebuild_scope_tracks();
        return false;
    }
    return true;
}

uint32_t nas_library_source_folder_context_id()
{
    uint32_t hash = 2166136261U;
    hash ^= static_cast<uint32_t>(g_scope);
    hash *= 16777619U;
    if (g_scope == PlayerFolderScope::Level1) hash = fnv1a32(g_level1_path, hash);
    else if (g_scope == PlayerFolderScope::Level2) hash = fnv1a32(g_level2_path, hash);
    return hash == 0U ? 1U : hash;
}

uint32_t nas_library_source_scoped_track_count()
{
    if (!nas_library_source_ready()) return 0U;
    return g_scope == PlayerFolderScope::All ? g_catalog.track_count : g_scope_track_count;
}

bool nas_library_source_scoped_track_at(uint32_t position, uint32_t *out_track_index)
{
    if (!nas_library_source_ready() || out_track_index == nullptr) return false;
    if (g_scope == PlayerFolderScope::All) {
        if (position >= g_catalog.track_count) return false;
        *out_track_index = position;
        return true;
    }
    if (position >= g_scope_track_count || g_scope_tracks == nullptr) return false;
    *out_track_index = g_scope_tracks[position];
    return true;
}

size_t nas_library_source_folder_option_count(PlayerFolderScope scope, const char *parent_path)
{
    if (!nas_library_source_ready()) return 0U;
    size_t count = 0U;
    char path[PLAYER_FOLDER_PATH_MAX] = {};
    if (scope == PlayerFolderScope::Level1) {
        while (level1_option_at(count, false, path, sizeof(path), nullptr)) ++count;
    } else if (scope == PlayerFolderScope::Level2) {
        while (level2_option_at(parent_path, count, path, sizeof(path), nullptr)) ++count;
    }
    return count;
}

bool nas_library_source_copy_folder_option_at(
    PlayerFolderScope scope,
    const char *parent_path,
    size_t position,
    char *out_path,
    size_t out_path_size,
    uint32_t *out_track_count)
{
    if (!nas_library_source_ready() || out_path == nullptr || out_path_size == 0U) return false;
    if (scope == PlayerFolderScope::Level1) {
        return level1_option_at(position, false, out_path, out_path_size, out_track_count);
    }
    if (scope == PlayerFolderScope::Level2) {
        return level2_option_at(parent_path, position, out_path, out_path_size, out_track_count);
    }
    return false;
}

bool nas_library_source_folder_selection_available(PlayerFolderScope scope, const char *path, uint32_t *out_track_count)
{
    if (!nas_library_source_ready() || path == nullptr || path[0] == '\0') return false;
    size_t count = nas_library_source_folder_option_count(PlayerFolderScope::Level1, nullptr);
    char candidate[PLAYER_FOLDER_PATH_MAX] = {};
    if (scope == PlayerFolderScope::Level1) {
        for (size_t i = 0U; i < count; ++i) {
            uint32_t tracks = 0U;
            if (level1_option_at(i, false, candidate, sizeof(candidate), &tracks) && strcasecmp(candidate, path) == 0) {
                if (out_track_count != nullptr) *out_track_count = tracks;
                return true;
            }
        }
        return false;
    }
    if (scope == PlayerFolderScope::Level2) {
        for (size_t parent_i = 0U;; ++parent_i) {
            if (!level1_option_at(parent_i, true, candidate, sizeof(candidate), nullptr)) break;
            const size_t child_count = nas_library_source_folder_option_count(PlayerFolderScope::Level2, candidate);
            char child[PLAYER_FOLDER_PATH_MAX] = {};
            for (size_t child_i = 0U; child_i < child_count; ++child_i) {
                uint32_t tracks = 0U;
                if (level2_option_at(candidate, child_i, child, sizeof(child), &tracks) && strcasecmp(child, path) == 0) {
                    if (out_track_count != nullptr) *out_track_count = tracks;
                    return true;
                }
            }
        }
    }
    return false;
}

bool nas_library_source_set_folder_selection(PlayerFolderScope scope, const char *path)
{
    if (!nas_library_source_ready() || path == nullptr || path[0] == '\0') return false;
    if (!nas_library_source_folder_selection_available(scope, path, nullptr)) return false;
    if (scope == PlayerFolderScope::Level1) {
        snprintf(g_level1_path, sizeof(g_level1_path), "%s", path);
    } else if (scope == PlayerFolderScope::Level2) {
        snprintf(g_level2_path, sizeof(g_level2_path), "%s", path);
        const size_t length = strlen(g_level2_path);
        if (length > 1U) {
            char parent[PLAYER_FOLDER_PATH_MAX] = {};
            snprintf(parent, sizeof(parent), "%s", g_level2_path);
            size_t n = strlen(parent);
            while (n > 0U && parent[n - 1U] == '/') parent[--n] = '\0';
            char *slash = strrchr(parent, '/');
            if (slash != nullptr && slash != parent) {
                slash[1] = '\0';
                snprintf(g_level1_path, sizeof(g_level1_path), "%s", parent);
            }
        }
    } else {
        return false;
    }
    return rebuild_scope_tracks();
}

bool nas_library_source_copy_folder_selection(PlayerFolderScope scope, char *out_path, size_t out_path_size)
{
    if (!nas_library_source_ready() || out_path == nullptr || out_path_size == 0U) return false;
    const char *path = scope == PlayerFolderScope::Level1 ? g_level1_path
        : (scope == PlayerFolderScope::Level2 ? g_level2_path : "");
    if (path[0] == '\0' || strlen(path) + 1U > out_path_size) return false;
    snprintf(out_path, out_path_size, "%s", path);
    return true;
}
