#include "media_groups_v2.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "media_catalog_v2.h"

static const char *TAG = "曲库分组V2";
static const MusicCatalogV2 *s_sort_catalog = nullptr;

static_assert(sizeof(MediaEntityTrackGroupV2) == 12, "MediaEntityTrackGroupV2 layout changed");
static_assert(sizeof(MediaDecadeTrackGroupV2) == 16, "MediaDecadeTrackGroupV2 layout changed");

static void *groups_psram_alloc(size_t bytes)
{
    if (bytes == 0) {
        return nullptr;
    }
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static const char *safe_pool_str(const MusicCatalogV2 *catalog, uint32_t offset)
{
    const char *text = media_catalog_v2_pool_str(catalog, offset);
    return text != nullptr ? text : "";
}

static bool track_has_direct_artist_ref(const MusicCatalogV2 *catalog, const TrackRowV2 &track, uint32_t artist_id)
{
    if (catalog == nullptr || artist_id >= catalog->artist_count) {
        return false;
    }
    for (uint32_t i = 0; i < track.artist_ref_count; ++i) {
        if (catalog->track_artist_refs[track.artist_ref_start + i].artist_id == artist_id) {
            return true;
        }
    }
    return false;
}

static bool track_has_artist(const MusicCatalogV2 *catalog, const TrackRowV2 &track, uint32_t artist_id)
{
    if (catalog == nullptr || artist_id >= catalog->artist_count) {
        return false;
    }
    for (uint32_t i = 0; i < track.artist_ref_count; ++i) {
        const TrackArtistRefV2 &ref = catalog->track_artist_refs[track.artist_ref_start + i];
        if (ref.artist_id == artist_id) {
            return true;
        }
    }
    if (track.album_id != MEDIA_CATALOG_INVALID_ID_V2 && track.album_id < catalog->album_count) {
        return catalog->albums[track.album_id].album_artist_id == artist_id;
    }
    return false;
}

static uint16_t track_effective_year(const TrackRowV2 &track)
{
    if ((track.metadata_flags & MEDIA_TRACK_META_HAS_ORIGINAL_YEAR_V2) != 0U) {
        return track.original_year;
    }
    if ((track.metadata_flags & MEDIA_TRACK_META_HAS_RELEASE_YEAR_V2) != 0U) {
        return track.release_year;
    }
    return 0U;
}

static uint16_t track_effective_decade(const TrackRowV2 &track)
{
    const uint16_t year = track_effective_year(track);
    return year == 0U ? 0U : static_cast<uint16_t>((year / 10U) * 10U);
}

static uint16_t track_effective_disc(const TrackRowV2 &track)
{
    return (track.metadata_flags & MEDIA_TRACK_META_HAS_DISC_NUMBER_V2) != 0U
        ? track.disc_number : 1U;
}

static int compare_track_path(uint32_t left_index, uint32_t right_index)
{
    if (s_sort_catalog == nullptr || left_index >= s_sort_catalog->track_count ||
        right_index >= s_sort_catalog->track_count) {
        return left_index < right_index ? -1 : (left_index > right_index ? 1 : 0);
    }
    const char *left = safe_pool_str(s_sort_catalog, s_sort_catalog->tracks[left_index].path_off);
    const char *right = safe_pool_str(s_sort_catalog, s_sort_catalog->tracks[right_index].path_off);
    const int path_cmp = strcasecmp(left, right);
    if (path_cmp != 0) {
        return path_cmp;
    }
    return left_index < right_index ? -1 : (left_index > right_index ? 1 : 0);
}

static int qsort_album_track_compare(const void *left, const void *right)
{
    const uint32_t left_index = *static_cast<const uint32_t *>(left);
    const uint32_t right_index = *static_cast<const uint32_t *>(right);
    if (s_sort_catalog == nullptr || left_index >= s_sort_catalog->track_count ||
        right_index >= s_sort_catalog->track_count) {
        return compare_track_path(left_index, right_index);
    }
    const TrackRowV2 &a = s_sort_catalog->tracks[left_index];
    const TrackRowV2 &b = s_sort_catalog->tracks[right_index];
    const uint16_t disc_a = track_effective_disc(a);
    const uint16_t disc_b = track_effective_disc(b);
    if (disc_a != disc_b) {
        return disc_a < disc_b ? -1 : 1;
    }
    const bool has_track_a = (a.metadata_flags & MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2) != 0U;
    const bool has_track_b = (b.metadata_flags & MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2) != 0U;
    if (has_track_a != has_track_b) {
        return has_track_a ? -1 : 1;
    }
    if (has_track_a && a.track_number != b.track_number) {
        return a.track_number < b.track_number ? -1 : 1;
    }
    return compare_track_path(left_index, right_index);
}

static int qsort_decade_track_compare(const void *left, const void *right)
{
    const uint32_t left_index = *static_cast<const uint32_t *>(left);
    const uint32_t right_index = *static_cast<const uint32_t *>(right);
    if (s_sort_catalog == nullptr || left_index >= s_sort_catalog->track_count ||
        right_index >= s_sort_catalog->track_count) {
        return compare_track_path(left_index, right_index);
    }
    const uint16_t year_a = track_effective_year(s_sort_catalog->tracks[left_index]);
    const uint16_t year_b = track_effective_year(s_sort_catalog->tracks[right_index]);
    if (year_a != year_b) {
        // 未知年代分组中两者都会是 0；正常年代内按真实年份升序。
        return year_a < year_b ? -1 : 1;
    }
    return compare_track_path(left_index, right_index);
}

static int qsort_artist_group_compare(const void *left, const void *right)
{
    const MediaEntityTrackGroupV2 &a = *static_cast<const MediaEntityTrackGroupV2 *>(left);
    const MediaEntityTrackGroupV2 &b = *static_cast<const MediaEntityTrackGroupV2 *>(right);
    if (s_sort_catalog == nullptr || a.entity_id >= s_sort_catalog->artist_count ||
        b.entity_id >= s_sort_catalog->artist_count) {
        return a.entity_id < b.entity_id ? -1 : (a.entity_id > b.entity_id ? 1 : 0);
    }
    const char *name_a = safe_pool_str(s_sort_catalog, s_sort_catalog->artists[a.entity_id].name_off);
    const char *name_b = safe_pool_str(s_sort_catalog, s_sort_catalog->artists[b.entity_id].name_off);
    const int cmp = strcasecmp(name_a, name_b);
    if (cmp != 0) {
        return cmp;
    }
    return a.entity_id < b.entity_id ? -1 : (a.entity_id > b.entity_id ? 1 : 0);
}

static int qsort_album_group_compare(const void *left, const void *right)
{
    const MediaEntityTrackGroupV2 &a = *static_cast<const MediaEntityTrackGroupV2 *>(left);
    const MediaEntityTrackGroupV2 &b = *static_cast<const MediaEntityTrackGroupV2 *>(right);
    if (s_sort_catalog == nullptr || a.entity_id >= s_sort_catalog->album_count ||
        b.entity_id >= s_sort_catalog->album_count) {
        return a.entity_id < b.entity_id ? -1 : (a.entity_id > b.entity_id ? 1 : 0);
    }
    const AlbumRowV2 &album_a = s_sort_catalog->albums[a.entity_id];
    const AlbumRowV2 &album_b = s_sort_catalog->albums[b.entity_id];
    const int title_cmp = strcasecmp(
        safe_pool_str(s_sort_catalog, album_a.title_off),
        safe_pool_str(s_sort_catalog, album_b.title_off)
    );
    if (title_cmp != 0) {
        return title_cmp;
    }
    const int artist_cmp = strcasecmp(
        safe_pool_str(s_sort_catalog, album_a.display_artist_off),
        safe_pool_str(s_sort_catalog, album_b.display_artist_off)
    );
    if (artist_cmp != 0) {
        return artist_cmp;
    }
    return a.entity_id < b.entity_id ? -1 : (a.entity_id > b.entity_id ? 1 : 0);
}

void media_groups_v2_release(MusicCatalogV2 *catalog)
{
    if (catalog == nullptr) {
        return;
    }
    heap_caps_free(catalog->artist_groups);
    heap_caps_free(catalog->album_groups);
    heap_caps_free(catalog->decade_groups);
    heap_caps_free(catalog->artist_group_track_pool);
    heap_caps_free(catalog->album_group_track_pool);
    heap_caps_free(catalog->decade_group_track_pool);
    catalog->artist_groups = nullptr;
    catalog->album_groups = nullptr;
    catalog->decade_groups = nullptr;
    catalog->artist_group_track_pool = nullptr;
    catalog->album_group_track_pool = nullptr;
    catalog->decade_group_track_pool = nullptr;
    catalog->artist_group_count = 0;
    catalog->album_group_count = 0;
    catalog->decade_group_count = 0;
    catalog->artist_group_track_count = 0;
    catalog->album_group_track_count = 0;
    catalog->decade_group_track_count = 0;
}

struct DecadeBuildV2
{
    uint16_t decade = 0;
    uint16_t reserved0 = 0;
    uint32_t count = 0;
};

static int qsort_decade_build_compare(const void *left, const void *right)
{
    const DecadeBuildV2 &a = *static_cast<const DecadeBuildV2 *>(left);
    const DecadeBuildV2 &b = *static_cast<const DecadeBuildV2 *>(right);
    // decade=0（未知）固定排在所有真实年代最后。
    if (a.decade == 0U || b.decade == 0U) {
        if (a.decade == b.decade) {
            return 0;
        }
        return a.decade == 0U ? 1 : -1;
    }
    return a.decade < b.decade ? -1 : (a.decade > b.decade ? 1 : 0);
}

static uint32_t find_decade_slot(const DecadeBuildV2 *items, uint32_t count, uint16_t decade)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].decade == decade) {
            return i;
        }
    }
    return UINT32_MAX;
}

static esp_err_t count_groups(
    const MusicCatalogV2 *catalog,
    uint32_t *artist_counts,
    uint32_t *album_counts,
    DecadeBuildV2 **out_decades,
    uint32_t *out_decade_count,
    uint32_t *out_artist_memberships,
    uint32_t *out_album_memberships
)
{
    if (catalog == nullptr || out_decades == nullptr || out_decade_count == nullptr ||
        out_artist_memberships == nullptr || out_album_memberships == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    DecadeBuildV2 *decades = nullptr;
    uint32_t decade_count = 0;
    uint32_t decade_capacity = 0;
    uint32_t artist_memberships = 0;
    uint32_t album_memberships = 0;

    for (uint32_t track_index = 0; track_index < catalog->track_count; ++track_index) {
        const TrackRowV2 &track = catalog->tracks[track_index];
        for (uint32_t ref_i = 0; ref_i < track.artist_ref_count; ++ref_i) {
            const uint32_t artist_id = catalog->track_artist_refs[track.artist_ref_start + ref_i].artist_id;
            bool duplicate = false;
            for (uint32_t prior = 0; prior < ref_i; ++prior) {
                if (catalog->track_artist_refs[track.artist_ref_start + prior].artist_id == artist_id) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                if (artist_counts[artist_id] == UINT32_MAX || artist_memberships == UINT32_MAX) {
                    heap_caps_free(decades);
                    return ESP_ERR_INVALID_SIZE;
                }
                artist_counts[artist_id]++;
                artist_memberships++;
            }
        }
        if (track.album_id != MEDIA_CATALOG_INVALID_ID_V2) {
            if (album_counts[track.album_id] == UINT32_MAX || album_memberships == UINT32_MAX) {
                heap_caps_free(decades);
                return ESP_ERR_INVALID_SIZE;
            }
            album_counts[track.album_id]++;
            album_memberships++;

            const uint32_t album_artist_id = catalog->albums[track.album_id].album_artist_id;
            if (album_artist_id != MEDIA_CATALOG_INVALID_ID_V2 &&
                !track_has_direct_artist_ref(catalog, track, album_artist_id)) {
                if (artist_counts[album_artist_id] == UINT32_MAX || artist_memberships == UINT32_MAX) {
                    heap_caps_free(decades);
                    return ESP_ERR_INVALID_SIZE;
                }
                artist_counts[album_artist_id]++;
                artist_memberships++;
            }
        }

        const uint16_t decade = track_effective_decade(track);
        uint32_t slot = find_decade_slot(decades, decade_count, decade);
        if (slot == UINT32_MAX) {
            if (decade_count == decade_capacity) {
                uint32_t next = decade_capacity == 0U ? 8U : decade_capacity * 2U;
                void *grown = heap_caps_realloc(
                    decades, static_cast<size_t>(next) * sizeof(DecadeBuildV2),
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                );
                if (grown == nullptr) {
                    heap_caps_free(decades);
                    return ESP_ERR_NO_MEM;
                }
                decades = static_cast<DecadeBuildV2 *>(grown);
                decade_capacity = next;
            }
            slot = decade_count++;
            decades[slot] = {};
            decades[slot].decade = decade;
        }
        if (decades[slot].count == UINT32_MAX) {
            heap_caps_free(decades);
            return ESP_ERR_INVALID_SIZE;
        }
        decades[slot].count++;
    }

    if (decade_count > 1U) {
        qsort(decades, decade_count, sizeof(DecadeBuildV2), qsort_decade_build_compare);
    }
    *out_decades = decades;
    *out_decade_count = decade_count;
    *out_artist_memberships = artist_memberships;
    *out_album_memberships = album_memberships;
    return ESP_OK;
}

esp_err_t media_groups_v2_build(MusicCatalogV2 *catalog)
{
    if (catalog == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    media_groups_v2_release(catalog);
    if (catalog->track_count == 0U) {
        return ESP_OK;
    }

    uint32_t *artist_counts = catalog->artist_count > 0U
        ? static_cast<uint32_t *>(groups_psram_alloc(static_cast<size_t>(catalog->artist_count) * sizeof(uint32_t))) : nullptr;
    uint32_t *album_counts = catalog->album_count > 0U
        ? static_cast<uint32_t *>(groups_psram_alloc(static_cast<size_t>(catalog->album_count) * sizeof(uint32_t))) : nullptr;
    if ((catalog->artist_count > 0U && artist_counts == nullptr) ||
        (catalog->album_count > 0U && album_counts == nullptr)) {
        heap_caps_free(artist_counts);
        heap_caps_free(album_counts);
        return ESP_ERR_NO_MEM;
    }
    if (artist_counts != nullptr) {
        memset(artist_counts, 0, static_cast<size_t>(catalog->artist_count) * sizeof(uint32_t));
    }
    if (album_counts != nullptr) {
        memset(album_counts, 0, static_cast<size_t>(catalog->album_count) * sizeof(uint32_t));
    }

    DecadeBuildV2 *decade_builds = nullptr;
    uint32_t decade_count = 0;
    uint32_t artist_memberships = 0;
    uint32_t album_memberships = 0;
    esp_err_t ret = count_groups(catalog, artist_counts, album_counts, &decade_builds, &decade_count,
        &artist_memberships, &album_memberships);
    if (ret != ESP_OK) {
        heap_caps_free(artist_counts);
        heap_caps_free(album_counts);
        return ret;
    }

    MediaEntityTrackGroupV2 *artist_groups = catalog->artist_count > 0U
        ? static_cast<MediaEntityTrackGroupV2 *>(groups_psram_alloc(
            static_cast<size_t>(catalog->artist_count) * sizeof(MediaEntityTrackGroupV2))) : nullptr;
    MediaEntityTrackGroupV2 *album_groups = catalog->album_count > 0U
        ? static_cast<MediaEntityTrackGroupV2 *>(groups_psram_alloc(
            static_cast<size_t>(catalog->album_count) * sizeof(MediaEntityTrackGroupV2))) : nullptr;
    MediaDecadeTrackGroupV2 *decade_groups = decade_count > 0U
        ? static_cast<MediaDecadeTrackGroupV2 *>(groups_psram_alloc(
            static_cast<size_t>(decade_count) * sizeof(MediaDecadeTrackGroupV2))) : nullptr;
    uint32_t *artist_pool = artist_memberships > 0U
        ? static_cast<uint32_t *>(groups_psram_alloc(static_cast<size_t>(artist_memberships) * sizeof(uint32_t))) : nullptr;
    uint32_t *album_pool = album_memberships > 0U
        ? static_cast<uint32_t *>(groups_psram_alloc(static_cast<size_t>(album_memberships) * sizeof(uint32_t))) : nullptr;
    uint32_t *decade_pool = catalog->track_count > 0U
        ? static_cast<uint32_t *>(groups_psram_alloc(static_cast<size_t>(catalog->track_count) * sizeof(uint32_t))) : nullptr;
    uint32_t *artist_cursor = catalog->artist_count > 0U
        ? static_cast<uint32_t *>(groups_psram_alloc(static_cast<size_t>(catalog->artist_count) * sizeof(uint32_t))) : nullptr;
    uint32_t *album_cursor = catalog->album_count > 0U
        ? static_cast<uint32_t *>(groups_psram_alloc(static_cast<size_t>(catalog->album_count) * sizeof(uint32_t))) : nullptr;
    uint32_t *decade_cursor = decade_count > 0U
        ? static_cast<uint32_t *>(groups_psram_alloc(static_cast<size_t>(decade_count) * sizeof(uint32_t))) : nullptr;

    const bool alloc_failed =
        (catalog->artist_count > 0U && (artist_groups == nullptr || artist_cursor == nullptr)) ||
        (catalog->album_count > 0U && (album_groups == nullptr || album_cursor == nullptr)) ||
        (decade_count > 0U && (decade_groups == nullptr || decade_cursor == nullptr)) ||
        (artist_memberships > 0U && artist_pool == nullptr) ||
        (album_memberships > 0U && album_pool == nullptr) ||
        decade_pool == nullptr;
    if (alloc_failed) {
        heap_caps_free(artist_counts);
        heap_caps_free(album_counts);
        heap_caps_free(decade_builds);
        heap_caps_free(artist_groups);
        heap_caps_free(album_groups);
        heap_caps_free(decade_groups);
        heap_caps_free(artist_pool);
        heap_caps_free(album_pool);
        heap_caps_free(decade_pool);
        heap_caps_free(artist_cursor);
        heap_caps_free(album_cursor);
        heap_caps_free(decade_cursor);
        return ESP_ERR_NO_MEM;
    }

    uint32_t offset = 0U;
    for (uint32_t artist_id = 0; artist_id < catalog->artist_count; ++artist_id) {
        artist_groups[artist_id] = {artist_id, offset, artist_counts[artist_id]};
        artist_cursor[artist_id] = offset;
        offset += artist_counts[artist_id];
    }
    offset = 0U;
    for (uint32_t album_id = 0; album_id < catalog->album_count; ++album_id) {
        album_groups[album_id] = {album_id, offset, album_counts[album_id]};
        album_cursor[album_id] = offset;
        offset += album_counts[album_id];
    }
    offset = 0U;
    for (uint32_t i = 0; i < decade_count; ++i) {
        decade_groups[i] = {};
        decade_groups[i].track_index_start = offset;
        decade_groups[i].track_count = decade_builds[i].count;
        decade_groups[i].decade_start = decade_builds[i].decade;
        decade_groups[i].flags = decade_builds[i].decade == 0U ? MEDIA_DECADE_GROUP_UNKNOWN_V2 : MEDIA_DECADE_GROUP_NONE_V2;
        decade_cursor[i] = offset;
        offset += decade_builds[i].count;
    }

    for (uint32_t track_index = 0; track_index < catalog->track_count; ++track_index) {
        const TrackRowV2 &track = catalog->tracks[track_index];
        for (uint32_t ref_i = 0; ref_i < track.artist_ref_count; ++ref_i) {
            const uint32_t artist_id = catalog->track_artist_refs[track.artist_ref_start + ref_i].artist_id;
            bool duplicate = false;
            for (uint32_t prior = 0; prior < ref_i; ++prior) {
                if (catalog->track_artist_refs[track.artist_ref_start + prior].artist_id == artist_id) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                artist_pool[artist_cursor[artist_id]++] = track_index;
            }
        }
        if (track.album_id != MEDIA_CATALOG_INVALID_ID_V2) {
            album_pool[album_cursor[track.album_id]++] = track_index;
            const uint32_t album_artist_id = catalog->albums[track.album_id].album_artist_id;
            if (album_artist_id != MEDIA_CATALOG_INVALID_ID_V2 &&
                !track_has_direct_artist_ref(catalog, track, album_artist_id)) {
                artist_pool[artist_cursor[album_artist_id]++] = track_index;
            }
        }
        const uint16_t decade = track_effective_decade(track);
        const uint32_t decade_slot = find_decade_slot(decade_builds, decade_count, decade);
        if (decade_slot == UINT32_MAX) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        decade_pool[decade_cursor[decade_slot]++] = track_index;
    }

    if (ret == ESP_OK) {
        s_sort_catalog = catalog;
        for (uint32_t i = 0; i < catalog->album_count; ++i) {
            const MediaEntityTrackGroupV2 &group = album_groups[i];
            if (group.track_count > 1U) {
                qsort(album_pool + group.track_index_start, group.track_count, sizeof(uint32_t), qsort_album_track_compare);
            }
        }
        for (uint32_t i = 0; i < decade_count; ++i) {
            const MediaDecadeTrackGroupV2 &group = decade_groups[i];
            if (group.track_count > 1U) {
                qsort(decade_pool + group.track_index_start, group.track_count, sizeof(uint32_t), qsort_decade_track_compare);
            }
        }
        if (catalog->artist_count > 1U) {
            qsort(artist_groups, catalog->artist_count, sizeof(MediaEntityTrackGroupV2), qsort_artist_group_compare);
        }
        if (catalog->album_count > 1U) {
            qsort(album_groups, catalog->album_count, sizeof(MediaEntityTrackGroupV2), qsort_album_group_compare);
        }
        s_sort_catalog = nullptr;
    }

    heap_caps_free(artist_counts);
    heap_caps_free(album_counts);
    heap_caps_free(decade_builds);
    heap_caps_free(artist_cursor);
    heap_caps_free(album_cursor);
    heap_caps_free(decade_cursor);

    if (ret != ESP_OK) {
        heap_caps_free(artist_groups);
        heap_caps_free(album_groups);
        heap_caps_free(decade_groups);
        heap_caps_free(artist_pool);
        heap_caps_free(album_pool);
        heap_caps_free(decade_pool);
        return ret;
    }

    catalog->artist_groups = artist_groups;
    catalog->album_groups = album_groups;
    catalog->decade_groups = decade_groups;
    catalog->artist_group_track_pool = artist_pool;
    catalog->album_group_track_pool = album_pool;
    catalog->decade_group_track_pool = decade_pool;
    catalog->artist_group_count = catalog->artist_count;
    catalog->album_group_count = catalog->album_count;
    catalog->decade_group_count = decade_count;
    catalog->artist_group_track_count = artist_memberships;
    catalog->album_group_track_count = album_memberships;
    catalog->decade_group_track_count = catalog->track_count;

    ret = media_groups_v2_validate(catalog);
    if (ret != ESP_OK) {
        media_groups_v2_release(catalog);
        return ret;
    }

    ESP_LOGI(TAG,
        "分组构建完成：artists=%lu memberships=%lu albums=%lu memberships=%lu decades=%lu memberships=%lu PSRAM=%uB",
        static_cast<unsigned long>(catalog->artist_group_count),
        static_cast<unsigned long>(catalog->artist_group_track_count),
        static_cast<unsigned long>(catalog->album_group_count),
        static_cast<unsigned long>(catalog->album_group_track_count),
        static_cast<unsigned long>(catalog->decade_group_count),
        static_cast<unsigned long>(catalog->decade_group_track_count),
        static_cast<unsigned>(media_groups_v2_psram_bytes(catalog)));
    return ESP_OK;
}

static bool artist_group_order_valid(const MusicCatalogV2 *catalog, uint32_t index)
{
    if (index == 0U) {
        return true;
    }
    const MediaEntityTrackGroupV2 &prev = catalog->artist_groups[index - 1U];
    const MediaEntityTrackGroupV2 &curr = catalog->artist_groups[index];
    const char *prev_name = safe_pool_str(catalog, catalog->artists[prev.entity_id].name_off);
    const char *curr_name = safe_pool_str(catalog, catalog->artists[curr.entity_id].name_off);
    const int cmp = strcasecmp(prev_name, curr_name);
    return cmp < 0 || (cmp == 0 && prev.entity_id < curr.entity_id);
}

static bool album_group_order_valid(const MusicCatalogV2 *catalog, uint32_t index)
{
    if (index == 0U) {
        return true;
    }
    const MediaEntityTrackGroupV2 &prev = catalog->album_groups[index - 1U];
    const MediaEntityTrackGroupV2 &curr = catalog->album_groups[index];
    const AlbumRowV2 &prev_album = catalog->albums[prev.entity_id];
    const AlbumRowV2 &curr_album = catalog->albums[curr.entity_id];
    int cmp = strcasecmp(safe_pool_str(catalog, prev_album.title_off), safe_pool_str(catalog, curr_album.title_off));
    if (cmp < 0) {
        return true;
    }
    if (cmp > 0) {
        return false;
    }
    cmp = strcasecmp(safe_pool_str(catalog, prev_album.display_artist_off), safe_pool_str(catalog, curr_album.display_artist_off));
    return cmp < 0 || (cmp == 0 && prev.entity_id < curr.entity_id);
}

esp_err_t media_groups_v2_validate(const MusicCatalogV2 *catalog)
{
    if (catalog == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (catalog->track_count == 0U) {
        return catalog->artist_group_count == 0U && catalog->album_group_count == 0U &&
            catalog->decade_group_count == 0U && catalog->artist_group_track_count == 0U &&
            catalog->album_group_track_count == 0U && catalog->decade_group_track_count == 0U
            ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    if (catalog->artist_group_count != catalog->artist_count ||
        catalog->album_group_count != catalog->album_count ||
        catalog->decade_group_track_count != catalog->track_count ||
        (catalog->artist_group_count > 0U && catalog->artist_groups == nullptr) ||
        (catalog->album_group_count > 0U && catalog->album_groups == nullptr) ||
        catalog->decade_group_count == 0U || catalog->decade_groups == nullptr ||
        (catalog->artist_group_track_count > 0U && catalog->artist_group_track_pool == nullptr) ||
        (catalog->album_group_track_count > 0U && catalog->album_group_track_pool == nullptr) ||
        catalog->decade_group_track_pool == nullptr) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint64_t artist_total = 0U;
    for (uint32_t i = 0; i < catalog->artist_group_count; ++i) {
        const MediaEntityTrackGroupV2 &group = catalog->artist_groups[i];
        if (group.entity_id >= catalog->artist_count ||
            group.track_index_start > catalog->artist_group_track_count ||
            group.track_count > catalog->artist_group_track_count - group.track_index_start ||
            group.track_count == 0U || !artist_group_order_valid(catalog, i)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint32_t previous_track = UINT32_MAX;
        for (uint32_t j = 0; j < group.track_count; ++j) {
            const uint32_t track_index = catalog->artist_group_track_pool[group.track_index_start + j];
            if (track_index >= catalog->track_count || !track_has_artist(catalog, catalog->tracks[track_index], group.entity_id) ||
                (previous_track != UINT32_MAX && previous_track >= track_index)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            previous_track = track_index;
        }
        artist_total += group.track_count;
    }
    if (artist_total != catalog->artist_group_track_count) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint64_t album_total = 0U;
    s_sort_catalog = catalog;
    for (uint32_t i = 0; i < catalog->album_group_count; ++i) {
        const MediaEntityTrackGroupV2 &group = catalog->album_groups[i];
        if (group.entity_id >= catalog->album_count ||
            group.track_index_start > catalog->album_group_track_count ||
            group.track_count > catalog->album_group_track_count - group.track_index_start ||
            group.track_count == 0U || !album_group_order_valid(catalog, i)) {
            s_sort_catalog = nullptr;
            return ESP_ERR_INVALID_RESPONSE;
        }
        for (uint32_t j = 0; j < group.track_count; ++j) {
            const uint32_t track_index = catalog->album_group_track_pool[group.track_index_start + j];
            if (track_index >= catalog->track_count || catalog->tracks[track_index].album_id != group.entity_id ||
                (j > 0U && qsort_album_track_compare(
                    &catalog->album_group_track_pool[group.track_index_start + j - 1U],
                    &catalog->album_group_track_pool[group.track_index_start + j]) >= 0)) {
                s_sort_catalog = nullptr;
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        album_total += group.track_count;
    }
    if (album_total != catalog->album_group_track_count) {
        s_sort_catalog = nullptr;
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint64_t decade_total = 0U;
    for (uint32_t i = 0; i < catalog->decade_group_count; ++i) {
        const MediaDecadeTrackGroupV2 &group = catalog->decade_groups[i];
        const bool unknown = (group.flags & MEDIA_DECADE_GROUP_UNKNOWN_V2) != 0U;
        if ((group.flags & ~MEDIA_DECADE_GROUP_UNKNOWN_V2) != 0U || group.reserved0 != 0U ||
            group.track_index_start > catalog->decade_group_track_count ||
            group.track_count > catalog->decade_group_track_count - group.track_index_start ||
            group.track_count == 0U || unknown != (group.decade_start == 0U) ||
            (i > 0U && !unknown && catalog->decade_groups[i - 1U].decade_start >= group.decade_start) ||
            (i + 1U < catalog->decade_group_count && unknown)) {
            s_sort_catalog = nullptr;
            return ESP_ERR_INVALID_RESPONSE;
        }
        for (uint32_t j = 0; j < group.track_count; ++j) {
            const uint32_t track_index = catalog->decade_group_track_pool[group.track_index_start + j];
            if (track_index >= catalog->track_count || track_effective_decade(catalog->tracks[track_index]) != group.decade_start ||
                (j > 0U && qsort_decade_track_compare(
                    &catalog->decade_group_track_pool[group.track_index_start + j - 1U],
                    &catalog->decade_group_track_pool[group.track_index_start + j]) >= 0)) {
                s_sort_catalog = nullptr;
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        decade_total += group.track_count;
    }
    s_sort_catalog = nullptr;
    return decade_total == catalog->decade_group_track_count ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

size_t media_groups_v2_psram_bytes(const MusicCatalogV2 *catalog)
{
    if (catalog == nullptr) {
        return 0U;
    }
    return static_cast<size_t>(catalog->artist_group_count) * sizeof(MediaEntityTrackGroupV2) +
        static_cast<size_t>(catalog->album_group_count) * sizeof(MediaEntityTrackGroupV2) +
        static_cast<size_t>(catalog->decade_group_count) * sizeof(MediaDecadeTrackGroupV2) +
        static_cast<size_t>(catalog->artist_group_track_count) * sizeof(uint32_t) +
        static_cast<size_t>(catalog->album_group_track_count) * sizeof(uint32_t) +
        static_cast<size_t>(catalog->decade_group_track_count) * sizeof(uint32_t);
}

size_t media_groups_v2_artist_count()
{
    const MusicCatalogV2 *catalog = media_catalog_v2_current();
    return catalog != nullptr ? catalog->artist_group_count : 0U;
}

size_t media_groups_v2_album_count()
{
    const MusicCatalogV2 *catalog = media_catalog_v2_current();
    return catalog != nullptr ? catalog->album_group_count : 0U;
}

size_t media_groups_v2_decade_count()
{
    const MusicCatalogV2 *catalog = media_catalog_v2_current();
    return catalog != nullptr ? catalog->decade_group_count : 0U;
}

bool media_groups_v2_get_artist(size_t group_index, MediaArtistGroupViewV2 *out_view)
{
    const MusicCatalogV2 *catalog = media_catalog_v2_current();
    if (catalog == nullptr || out_view == nullptr || group_index >= catalog->artist_group_count) {
        return false;
    }
    const MediaEntityTrackGroupV2 &group = catalog->artist_groups[group_index];
    MediaArtistGroupViewV2 view = {};
    view.generation = catalog->generation;
    view.artist_id = group.entity_id;
    view.name = safe_pool_str(catalog, catalog->artists[group.entity_id].name_off);
    view.track_indices = catalog->artist_group_track_pool + group.track_index_start;
    view.track_count = group.track_count;
    *out_view = view;
    return true;
}

bool media_groups_v2_get_album(size_t group_index, MediaAlbumGroupViewV2 *out_view)
{
    const MusicCatalogV2 *catalog = media_catalog_v2_current();
    if (catalog == nullptr || out_view == nullptr || group_index >= catalog->album_group_count) {
        return false;
    }
    const MediaEntityTrackGroupV2 &group = catalog->album_groups[group_index];
    const AlbumRowV2 &album = catalog->albums[group.entity_id];
    MediaAlbumGroupViewV2 view = {};
    view.generation = catalog->generation;
    view.album_id = group.entity_id;
    view.title = safe_pool_str(catalog, album.title_off);
    view.artist = safe_pool_str(catalog, album.display_artist_off);
    view.track_indices = catalog->album_group_track_pool + group.track_index_start;
    view.track_count = group.track_count;
    view.flags = album.flags;
    view.release_year = album.release_year;
    view.original_year = album.original_year;
    *out_view = view;
    return true;
}

bool media_groups_v2_get_decade(size_t group_index, MediaDecadeGroupViewV2 *out_view)
{
    const MusicCatalogV2 *catalog = media_catalog_v2_current();
    if (catalog == nullptr || out_view == nullptr || group_index >= catalog->decade_group_count) {
        return false;
    }
    const MediaDecadeTrackGroupV2 &group = catalog->decade_groups[group_index];
    MediaDecadeGroupViewV2 view = {};
    view.generation = catalog->generation;
    view.decade_start = group.decade_start;
    view.unknown = (group.flags & MEDIA_DECADE_GROUP_UNKNOWN_V2) != 0U;
    view.track_indices = catalog->decade_group_track_pool + group.track_index_start;
    view.track_count = group.track_count;
    *out_view = view;
    return true;
}
