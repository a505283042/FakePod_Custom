#include "media_catalog_v2.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "media_index_store.h"
#include "media_groups_v2.h"
#include "media_metadata.h"
#include "media_artwork.h"
#include "app_diag_config.h"

static const char *TAG = "曲库V2";

#if APP_DIAG_BOOT_VERBOSE
#define CATALOG_BOOT_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define CATALOG_BOOT_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif
static MusicCatalogV2 s_catalog = {};
static uint32_t s_generation_seq = 0;
static bool s_ready = false;

static void *catalog_psram_alloc(size_t bytes)
{
    if (bytes == 0) {
        return nullptr;
    }
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool pool_offset_valid(const StringPoolV2 &pool, uint32_t offset)
{
    if (pool.data == nullptr || offset >= pool.size) {
        return false;
    }
    return memchr(pool.data + offset, '\0', pool.size - offset) != nullptr;
}

const char *media_catalog_v2_pool_str(const MusicCatalogV2 *catalog, uint32_t offset)
{
    if (catalog == nullptr || !pool_offset_valid(catalog->pool, offset)) {
        return nullptr;
    }
    return catalog->pool.data + offset;
}

static const char *filename_from_path(const char *path)
{
    if (path == nullptr) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

static size_t title_length_without_extension(const char *path)
{
    const char *filename = filename_from_path(path);
    const char *dot = strrchr(filename, '.');
    if (dot == nullptr || dot == filename) {
        return strlen(filename);
    }
    return static_cast<size_t>(dot - filename);
}

static constexpr uint32_t TRACK_METADATA_KNOWN_MASK_V2 =
    MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2 |
    MEDIA_TRACK_META_HAS_TRACK_TOTAL_V2 |
    MEDIA_TRACK_META_HAS_DISC_NUMBER_V2 |
    MEDIA_TRACK_META_HAS_DISC_TOTAL_V2 |
    MEDIA_TRACK_META_HAS_RELEASE_YEAR_V2 |
    MEDIA_TRACK_META_HAS_ORIGINAL_YEAR_V2 |
    MEDIA_TRACK_META_SCANNED_V2 |
    MEDIA_TRACK_META_HAS_TITLE_TAG_V2 |
    MEDIA_TRACK_META_HAS_ARTIST_TAG_V2 |
    MEDIA_TRACK_META_HAS_ALBUM_TAG_V2 |
    MEDIA_TRACK_META_HAS_ALBUM_ARTIST_TAG_V2;

static constexpr uint32_t ALBUM_FLAGS_KNOWN_MASK_V2 =
    MEDIA_ALBUM_FLAG_HAS_RELEASE_YEAR_V2 |
    MEDIA_ALBUM_FLAG_HAS_ORIGINAL_YEAR_V2 |
    MEDIA_ALBUM_FLAG_MIXED_RELEASE_YEARS_V2 |
    MEDIA_ALBUM_FLAG_MIXED_ORIGINAL_YEARS_V2;

static constexpr uint32_t TRACK_ARTIST_REF_FLAGS_KNOWN_MASK_V2 =
    MEDIA_TRACK_ARTIST_REF_NONE_V2;

static constexpr uint32_t LYRICS_REF_FLAGS_KNOWN_MASK_V2 =
    MEDIA_LYRICS_REF_NEEDS_ID3_UNSYNC_V2 |
    MEDIA_LYRICS_REF_ID3_FRAME_PAYLOAD_V2;

static bool metadata_value_matches_flag(uint32_t flags, uint32_t bit, uint16_t value)
{
    return (flags & bit) != 0U ? value != 0U : value == 0U;
}

static bool metadata_year_valid(uint16_t year)
{
    // 当前只持久化四位公历年份；完整日期留给未来扩展，不占用 TrackRow 热路径。
    return year >= 1000U && year <= 9999U;
}

void media_catalog_v2_release(MusicCatalogV2 *catalog)
{
    if (catalog == nullptr) {
        return;
    }
    media_groups_v2_release(catalog);
    heap_caps_free(catalog->pool.data);
    heap_caps_free(catalog->tracks);
    heap_caps_free(catalog->artists);
    heap_caps_free(catalog->albums);
    heap_caps_free(catalog->track_artist_refs);
    heap_caps_free(catalog->lyrics_refs);
    heap_caps_free(catalog->artwork_refs);
    *catalog = {};
}

esp_err_t media_catalog_v2_validate(const MusicCatalogV2 *catalog)
{
    if (catalog == nullptr || catalog->pool.data == nullptr || catalog->pool.size == 0 ||
        catalog->pool.data[0] != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((catalog->track_count > 0 && catalog->tracks == nullptr) ||
        (catalog->artist_count > 0 && catalog->artists == nullptr) ||
        (catalog->album_count > 0 && catalog->albums == nullptr) ||
        (catalog->track_artist_ref_count > 0 && catalog->track_artist_refs == nullptr) ||
        (catalog->lyrics_ref_count > 0 && catalog->lyrics_refs == nullptr) ||
        (catalog->artwork_ref_count > 0 && catalog->artwork_refs == nullptr)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (uint32_t i = 0; i < catalog->artist_count; ++i) {
        const char *artist_name = media_catalog_v2_pool_str(catalog, catalog->artists[i].name_off);
        if (artist_name == nullptr || artist_name[0] == '\0') {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    for (uint32_t i = 0; i < catalog->album_count; ++i) {
        const AlbumRowV2 &album = catalog->albums[i];
        const bool has_release_year = (album.flags & MEDIA_ALBUM_FLAG_HAS_RELEASE_YEAR_V2) != 0U;
        const bool has_original_year = (album.flags & MEDIA_ALBUM_FLAG_HAS_ORIGINAL_YEAR_V2) != 0U;
        const bool mixed_release_years = (album.flags & MEDIA_ALBUM_FLAG_MIXED_RELEASE_YEARS_V2) != 0U;
        const bool mixed_original_years = (album.flags & MEDIA_ALBUM_FLAG_MIXED_ORIGINAL_YEARS_V2) != 0U;
        if (!pool_offset_valid(catalog->pool, album.title_off) ||
            !pool_offset_valid(catalog->pool, album.display_artist_off) ||
            (album.album_artist_id != MEDIA_CATALOG_INVALID_ID_V2 && album.album_artist_id >= catalog->artist_count) ||
            (album.artwork_track_id != MEDIA_CATALOG_INVALID_ID_V2 && album.artwork_track_id >= catalog->track_count) ||
            (album.flags & ~ALBUM_FLAGS_KNOWN_MASK_V2) != 0U ||
            (has_release_year && mixed_release_years) ||
            (has_original_year && mixed_original_years) ||
            (has_release_year ? !metadata_year_valid(album.release_year) : album.release_year != 0U) ||
            (has_original_year ? !metadata_year_valid(album.original_year) : album.original_year != 0U)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const char *album_title = media_catalog_v2_pool_str(catalog, album.title_off);
        const char *album_artist = media_catalog_v2_pool_str(catalog, album.display_artist_off);
        if (album_title == nullptr || album_title[0] == '\0' ||
            (album.album_artist_id != MEDIA_CATALOG_INVALID_ID_V2 &&
             (album_artist == nullptr || album_artist[0] == '\0'))) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    const char *previous_path = nullptr;
    uint32_t expected_artist_ref_start = 0;
    uint32_t expected_lyrics_ref_start = 0;
    for (uint32_t i = 0; i < catalog->track_count; ++i) {
        const TrackRowV2 &track = catalog->tracks[i];
        const char *path = media_catalog_v2_pool_str(catalog, track.path_off);
        if (path == nullptr || path[0] == '\0' ||
            !pool_offset_valid(catalog->pool, track.title_off) ||
            !pool_offset_valid(catalog->pool, track.display_artist_off) ||
            track.format > MediaFormat::NSFE ||
            (track.album_id != MEDIA_CATALOG_INVALID_ID_V2 && track.album_id >= catalog->album_count) ||
            (track.artwork_ref_id != MEDIA_CATALOG_INVALID_ID_V2 && track.artwork_ref_id >= catalog->artwork_ref_count) ||
            track.reserved0 != 0U || track.reserved1 != 0U ||
            expected_artist_ref_start > catalog->track_artist_ref_count ||
            track.artist_ref_start != expected_artist_ref_start ||
            track.artist_ref_count > catalog->track_artist_ref_count - expected_artist_ref_start ||
            expected_lyrics_ref_start > catalog->lyrics_ref_count ||
            track.lyrics_ref_start != expected_lyrics_ref_start ||
            track.lyrics_ref_count > catalog->lyrics_ref_count - expected_lyrics_ref_start) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (previous_path != nullptr && strcasecmp(previous_path, path) >= 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        previous_path = path;

        const char *display_artist = media_catalog_v2_pool_str(catalog, track.display_artist_off);
        if (track.artist_ref_count > 0 && (display_artist == nullptr || display_artist[0] == '\0')) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const bool metadata_scanned = (track.metadata_flags & MEDIA_TRACK_META_SCANNED_V2) != 0U;
        const uint32_t tag_presence_bits =
            MEDIA_TRACK_META_HAS_TITLE_TAG_V2 |
            MEDIA_TRACK_META_HAS_ARTIST_TAG_V2 |
            MEDIA_TRACK_META_HAS_ALBUM_TAG_V2 |
            MEDIA_TRACK_META_HAS_ALBUM_ARTIST_TAG_V2;
        if ((!metadata_scanned && (track.metadata_flags & tag_presence_bits) != 0U) ||
            ((track.metadata_flags & MEDIA_TRACK_META_HAS_ARTIST_TAG_V2) != 0U &&
             (track.artist_ref_count == 0U || display_artist == nullptr || display_artist[0] == '\0')) ||
            ((track.metadata_flags & MEDIA_TRACK_META_HAS_ALBUM_TAG_V2) != 0U &&
             track.album_id == MEDIA_CATALOG_INVALID_ID_V2) ||
            ((track.metadata_flags & MEDIA_TRACK_META_HAS_ALBUM_ARTIST_TAG_V2) != 0U &&
             (track.album_id == MEDIA_CATALOG_INVALID_ID_V2 ||
              catalog->albums[track.album_id].album_artist_id == MEDIA_CATALOG_INVALID_ID_V2))) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        for (uint32_t ref_index = 0; ref_index < track.artist_ref_count; ++ref_index) {
            const TrackArtistRefV2 &ref = catalog->track_artist_refs[track.artist_ref_start + ref_index];
            if (ref.artist_id >= catalog->artist_count ||
                (ref.flags & ~TRACK_ARTIST_REF_FLAGS_KNOWN_MASK_V2) != 0U) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        expected_artist_ref_start += track.artist_ref_count;

        for (uint32_t ref_index = 0; ref_index < track.lyrics_ref_count; ++ref_index) {
            const LyricsRefV2 &ref = catalog->lyrics_refs[track.lyrics_ref_start + ref_index];
            const bool source_valid = ref.source == MediaLyricsSourceV2::ExternalFile ||
                ref.source == MediaLyricsSourceV2::EmbeddedTag;
            const bool kind_valid = ref.kind == MediaLyricsKindV2::Unsynced ||
                ref.kind == MediaLyricsKindV2::Synced;
            const bool encoding_valid = ref.encoding <= MediaLyricsEncodingV2::Latin1;
            if (!source_valid || !kind_valid || !encoding_valid ||
                (ref.flags & ~LYRICS_REF_FLAGS_KNOWN_MASK_V2) != 0U || ref.reserved0 != 0U ||
                !pool_offset_valid(catalog->pool, ref.language_off)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (ref.source == MediaLyricsSourceV2::ExternalFile) {
                const char *lyrics_path = media_catalog_v2_pool_str(catalog, ref.path_off);
                if (lyrics_path == nullptr || lyrics_path[0] == '\0' || ref.data_offset != 0U || ref.data_size != 0U ||
                    ref.flags != MEDIA_LYRICS_REF_NONE_V2) {
                    return ESP_ERR_INVALID_RESPONSE;
                }
            } else {
                if (ref.path_off != 0U || ref.data_size == 0U || ref.data_offset > track.file_size_bytes ||
                    static_cast<uint64_t>(ref.data_size) > track.file_size_bytes - ref.data_offset) {
                    return ESP_ERR_INVALID_SIZE;
                }
            }
        }
        expected_lyrics_ref_start += track.lyrics_ref_count;

        if (track.artwork_ref_id != MEDIA_CATALOG_INVALID_ID_V2) {
            const ArtworkRefV2 &artwork = catalog->artwork_refs[track.artwork_ref_id];
            const bool source_valid = artwork.source == MediaArtworkSourceV2::Mp3Apic ||
                artwork.source == MediaArtworkSourceV2::FlacPicture ||
                artwork.source == MediaArtworkSourceV2::ExternalFile;
            const bool format_valid = artwork.format == MediaArtworkFormatV2::Jpeg ||
                artwork.format == MediaArtworkFormatV2::Png;
            if (!source_valid || !format_valid || artwork.data_size == 0U || artwork.reserved0 != 0U ||
                (artwork.flags & ~MEDIA_ARTWORK_REF_NEEDS_ID3_UNSYNC_V2) != 0U) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (artwork.source == MediaArtworkSourceV2::ExternalFile) {
                const char *artwork_path = media_catalog_v2_pool_str(catalog, artwork.path_off);
                if (artwork_path == nullptr || artwork_path[0] == '\0' || artwork.data_offset != 0U ||
                    artwork.flags != MEDIA_ARTWORK_REF_NONE_V2) {
                    return ESP_ERR_INVALID_RESPONSE;
                }
            } else {
                if (artwork.path_off != 0U || artwork.source_modified_time != 0 ||
                    artwork.data_offset > track.file_size_bytes ||
                    static_cast<uint64_t>(artwork.data_size) > track.file_size_bytes - artwork.data_offset ||
                    (artwork.source == MediaArtworkSourceV2::Mp3Apic && track.format != MediaFormat::MP3) ||
                    (artwork.source == MediaArtworkSourceV2::FlacPicture && track.format != MediaFormat::FLAC) ||
                    ((artwork.flags & MEDIA_ARTWORK_REF_NEEDS_ID3_UNSYNC_V2) != 0U &&
                     artwork.source != MediaArtworkSourceV2::Mp3Apic)) {
                    return ESP_ERR_INVALID_SIZE;
                }
            }
        }

        if ((track.metadata_flags & ~TRACK_METADATA_KNOWN_MASK_V2) != 0U ||
            !metadata_value_matches_flag(track.metadata_flags, MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2, track.track_number) ||
            !metadata_value_matches_flag(track.metadata_flags, MEDIA_TRACK_META_HAS_TRACK_TOTAL_V2, track.track_total) ||
            !metadata_value_matches_flag(track.metadata_flags, MEDIA_TRACK_META_HAS_DISC_NUMBER_V2, track.disc_number) ||
            !metadata_value_matches_flag(track.metadata_flags, MEDIA_TRACK_META_HAS_DISC_TOTAL_V2, track.disc_total) ||
            (((track.metadata_flags & MEDIA_TRACK_META_HAS_RELEASE_YEAR_V2) != 0U)
                ? !metadata_year_valid(track.release_year) : track.release_year != 0U) ||
            (((track.metadata_flags & MEDIA_TRACK_META_HAS_ORIGINAL_YEAR_V2) != 0U)
                ? !metadata_year_valid(track.original_year) : track.original_year != 0U) ||
            (((track.metadata_flags & (MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2 | MEDIA_TRACK_META_HAS_TRACK_TOTAL_V2)) ==
                 (MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2 | MEDIA_TRACK_META_HAS_TRACK_TOTAL_V2)) &&
                track.track_number > track.track_total) ||
            (((track.metadata_flags & (MEDIA_TRACK_META_HAS_DISC_NUMBER_V2 | MEDIA_TRACK_META_HAS_DISC_TOTAL_V2)) ==
                 (MEDIA_TRACK_META_HAS_DISC_NUMBER_V2 | MEDIA_TRACK_META_HAS_DISC_TOTAL_V2)) &&
                track.disc_number > track.disc_total)) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        const MediaTechnicalInfo &tech = track.technical;
        if (tech.audio_data_offset > track.file_size_bytes ||
            tech.metadata_end_offset > track.file_size_bytes ||
            tech.artwork_offset > track.file_size_bytes ||
            (tech.artwork_size > 0 &&
             (tech.artwork_offset > track.file_size_bytes ||
              static_cast<uint64_t>(tech.artwork_size) > track.file_size_bytes - tech.artwork_offset))) {
            return ESP_ERR_INVALID_SIZE;
        }
        if ((tech.flags & MEDIA_TECH_PARSED) != 0U &&
            (tech.sample_rate_hz == 0 || tech.channels == 0 || tech.bits_per_sample == 0)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (expected_artist_ref_start != catalog->track_artist_ref_count ||
        expected_lyrics_ref_start != catalog->lyrics_ref_count) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

struct CatalogArtistBuildV2
{
    const char *name = nullptr;
};

struct CatalogAlbumBuildV2
{
    const char *title = nullptr;
    const char *display_artist = nullptr;
    uint32_t album_artist_id = MEDIA_CATALOG_INVALID_ID_V2;
    uint32_t artwork_track_id = MEDIA_CATALOG_INVALID_ID_V2;
    uint32_t flags = MEDIA_ALBUM_FLAG_NONE_V2;
    uint16_t release_year = 0;
    uint16_t original_year = 0;
};

static bool checked_add_size(size_t *total, size_t add)
{
    if (total == nullptr || *total > UINT32_MAX || add > UINT32_MAX - *total) {
        return false;
    }
    *total += add;
    return true;
}

static uint32_t find_artist_build(const CatalogArtistBuildV2 *artists, uint32_t count, const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return MEDIA_CATALOG_INVALID_ID_V2;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (artists[i].name != nullptr && strcmp(artists[i].name, name) == 0) {
            return i;
        }
    }
    return MEDIA_CATALOG_INVALID_ID_V2;
}

static esp_err_t ensure_artist_build(
    CatalogArtistBuildV2 **artists,
    uint32_t *count,
    uint32_t *capacity,
    const char *name,
    uint32_t *out_id
)
{
    if (artists == nullptr || count == nullptr || capacity == nullptr || out_id == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_id = MEDIA_CATALOG_INVALID_ID_V2;
    if (name == nullptr || name[0] == '\0') {
        return ESP_OK;
    }
    const uint32_t existing = find_artist_build(*artists, *count, name);
    if (existing != MEDIA_CATALOG_INVALID_ID_V2) {
        *out_id = existing;
        return ESP_OK;
    }
    if (*count == UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (*count == *capacity) {
        uint32_t next = *capacity == 0 ? 16U : *capacity * 2U;
        if (next < *count || next > 100000U) {
            next = *count + 1U;
        }
        void *grown = heap_caps_realloc(
            *artists, static_cast<size_t>(next) * sizeof(CatalogArtistBuildV2),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (grown == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        *artists = static_cast<CatalogArtistBuildV2 *>(grown);
        *capacity = next;
    }
    (*artists)[*count].name = name;
    *out_id = (*count)++;
    return ESP_OK;
}

static uint32_t find_album_build(
    const CatalogAlbumBuildV2 *albums,
    uint32_t count,
    const char *title,
    const char *display_artist
)
{
    if (title == nullptr || title[0] == '\0') {
        return MEDIA_CATALOG_INVALID_ID_V2;
    }
    const char *artist = display_artist != nullptr ? display_artist : "";
    for (uint32_t i = 0; i < count; ++i) {
        if (albums[i].title != nullptr && strcmp(albums[i].title, title) == 0 &&
            strcmp(albums[i].display_artist != nullptr ? albums[i].display_artist : "", artist) == 0) {
            return i;
        }
    }
    return MEDIA_CATALOG_INVALID_ID_V2;
}

static void merge_album_year(
    uint32_t track_flags,
    uint32_t track_has_bit,
    uint16_t track_year,
    uint32_t album_has_bit,
    uint32_t album_mixed_bit,
    uint32_t *album_flags,
    uint16_t *album_year
)
{
    if (album_flags == nullptr || album_year == nullptr || (track_flags & track_has_bit) == 0U) {
        return;
    }
    if ((*album_flags & album_mixed_bit) != 0U) {
        return;
    }
    if ((*album_flags & album_has_bit) == 0U) {
        *album_flags |= album_has_bit;
        *album_year = track_year;
        return;
    }
    if (*album_year != track_year) {
        *album_flags &= ~album_has_bit;
        *album_flags |= album_mixed_bit;
        *album_year = 0U;
    }
}

static uint32_t append_pool_string(char *pool, size_t capacity, size_t *cursor, const char *text)
{
    if (pool == nullptr || cursor == nullptr || text == nullptr || text[0] == '\0') {
        return 0U;
    }
    const size_t length = strlen(text) + 1U;
    if (*cursor > capacity || length > capacity - *cursor || *cursor > UINT32_MAX) {
        return UINT32_MAX;
    }
    const uint32_t offset = static_cast<uint32_t>(*cursor);
    memcpy(pool + *cursor, text, length);
    *cursor += length;
    return offset;
}

esp_err_t media_catalog_v2_build_from_index_records(
    const MediaIndexRecord *records,
    size_t record_count,
    const char *path_pool,
    size_t path_pool_size,
    MusicCatalogV2 *out_catalog
)
{
    if (out_catalog == nullptr || record_count > UINT32_MAX ||
        (record_count > 0 && (records == nullptr || path_pool == nullptr || path_pool_size == 0))) {
        return ESP_ERR_INVALID_ARG;
    }
    media_catalog_v2_release(out_catalog);

    CatalogArtistBuildV2 *artist_builds = nullptr;
    CatalogAlbumBuildV2 *album_builds = nullptr;
    uint32_t artist_count = 0;
    uint32_t artist_capacity = 0;
    uint32_t album_count = 0;
    uint32_t album_capacity = 0;
    uint32_t artist_ref_count = 0;
    uint32_t lyrics_ref_count = 0;
    uint32_t artwork_ref_count = 0;

    // 第一遍只建立实体关系和计数，不复制字符串。metadata_build 的生命周期覆盖整个构建过程。
    for (size_t i = 0; i < record_count; ++i) {
        const MediaIndexRecord &record = records[i];
        if (record.path_offset >= path_pool_size ||
            memchr(path_pool + record.path_offset, '\0', path_pool_size - record.path_offset) == nullptr) {
            heap_caps_free(artist_builds);
            heap_caps_free(album_builds);
            return ESP_ERR_INVALID_RESPONSE;
        }
        const MediaMetadataBuildV2 *metadata = record.metadata_build;
        const MediaArtworkBuildV2 *artwork = record.artwork_build;
        if (artwork != nullptr && artwork->source != MediaArtworkSourceV2::None) {
            if (artwork_ref_count == UINT32_MAX) {
                heap_caps_free(artist_builds);
                heap_caps_free(album_builds);
                return ESP_ERR_INVALID_SIZE;
            }
            artwork_ref_count++;
        }
        if (metadata == nullptr) {
            continue;
        }
        if (artist_ref_count > UINT32_MAX - metadata->artist_count ||
            lyrics_ref_count > UINT32_MAX - metadata->lyrics_count) {
            heap_caps_free(artist_builds);
            heap_caps_free(album_builds);
            return ESP_ERR_INVALID_SIZE;
        }
        artist_ref_count += metadata->artist_count;
        lyrics_ref_count += metadata->lyrics_count;

        for (uint16_t a = 0; a < metadata->artist_count; ++a) {
            uint32_t ignored = MEDIA_CATALOG_INVALID_ID_V2;
            const esp_err_t add_ret = ensure_artist_build(
                &artist_builds, &artist_count, &artist_capacity, metadata->artists[a], &ignored
            );
            if (add_ret != ESP_OK) {
                heap_caps_free(artist_builds);
                heap_caps_free(album_builds);
                return add_ret;
            }
        }

        if (metadata->album != nullptr && metadata->album[0] != '\0') {
            const bool has_explicit_album_artist =
                metadata->album_artist != nullptr && metadata->album_artist[0] != '\0';
            const char *album_artist = has_explicit_album_artist
                ? metadata->album_artist
                : (metadata->display_artist != nullptr ? metadata->display_artist : "");
            uint32_t album_artist_id = MEDIA_CATALOG_INVALID_ID_V2;
            if (has_explicit_album_artist) {
                const esp_err_t artist_ret = ensure_artist_build(
                    &artist_builds, &artist_count, &artist_capacity, album_artist, &album_artist_id
                );
                if (artist_ret != ESP_OK) {
                    heap_caps_free(artist_builds);
                    heap_caps_free(album_builds);
                    return artist_ret;
                }
            }
            uint32_t album_id = find_album_build(album_builds, album_count, metadata->album, album_artist);
            if (album_id == MEDIA_CATALOG_INVALID_ID_V2) {
                if (album_count == album_capacity) {
                    uint32_t next = album_capacity == 0 ? 16U : album_capacity * 2U;
                    void *grown = heap_caps_realloc(
                        album_builds, static_cast<size_t>(next) * sizeof(CatalogAlbumBuildV2),
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                    );
                    if (grown == nullptr) {
                        heap_caps_free(artist_builds);
                        heap_caps_free(album_builds);
                        return ESP_ERR_NO_MEM;
                    }
                    album_builds = static_cast<CatalogAlbumBuildV2 *>(grown);
                    album_capacity = next;
                }
                album_id = album_count++;
                album_builds[album_id] = {};
                album_builds[album_id].title = metadata->album;
                album_builds[album_id].display_artist = album_artist;
                album_builds[album_id].album_artist_id = album_artist_id;
            }
            CatalogAlbumBuildV2 &album = album_builds[album_id];
            if (album.artwork_track_id == MEDIA_CATALOG_INVALID_ID_V2 &&
                artwork != nullptr && artwork->source != MediaArtworkSourceV2::None) {
                album.artwork_track_id = static_cast<uint32_t>(i);
            }
            merge_album_year(metadata->metadata_flags, MEDIA_TRACK_META_HAS_RELEASE_YEAR_V2,
                metadata->release_year, MEDIA_ALBUM_FLAG_HAS_RELEASE_YEAR_V2,
                MEDIA_ALBUM_FLAG_MIXED_RELEASE_YEARS_V2, &album.flags, &album.release_year);
            merge_album_year(metadata->metadata_flags, MEDIA_TRACK_META_HAS_ORIGINAL_YEAR_V2,
                metadata->original_year, MEDIA_ALBUM_FLAG_HAS_ORIGINAL_YEAR_V2,
                MEDIA_ALBUM_FLAG_MIXED_ORIGINAL_YEARS_V2, &album.flags, &album.original_year);
        }
    }

    size_t string_bytes = 1U; // offset 0 固定为空字符串。
    for (uint32_t i = 0; i < artist_count; ++i) {
        if (!checked_add_size(&string_bytes, strlen(artist_builds[i].name) + 1U)) {
            heap_caps_free(artist_builds);
            heap_caps_free(album_builds);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    for (uint32_t i = 0; i < album_count; ++i) {
        if (!checked_add_size(&string_bytes, strlen(album_builds[i].title) + 1U) ||
            (album_builds[i].display_artist != nullptr && album_builds[i].display_artist[0] != '\0' &&
             !checked_add_size(&string_bytes, strlen(album_builds[i].display_artist) + 1U))) {
            heap_caps_free(artist_builds);
            heap_caps_free(album_builds);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    for (size_t i = 0; i < record_count; ++i) {
        const char *path = path_pool + records[i].path_offset;
        const MediaMetadataBuildV2 *metadata = records[i].metadata_build;
        const char *title = metadata != nullptr && metadata->title != nullptr && metadata->title[0] != '\0'
            ? metadata->title : nullptr;
        const size_t fallback_title_len = title == nullptr ? title_length_without_extension(path) : 0U;
        if (!checked_add_size(&string_bytes, strlen(path) + 1U) ||
            !checked_add_size(&string_bytes, title != nullptr ? strlen(title) + 1U : fallback_title_len + 1U) ||
            (metadata != nullptr && metadata->display_artist != nullptr && metadata->display_artist[0] != '\0' &&
             !checked_add_size(&string_bytes, strlen(metadata->display_artist) + 1U))) {
            heap_caps_free(artist_builds);
            heap_caps_free(album_builds);
            return ESP_ERR_INVALID_SIZE;
        }
        const MediaArtworkBuildV2 *artwork = records[i].artwork_build;
        if (artwork != nullptr && artwork->source == MediaArtworkSourceV2::ExternalFile &&
            artwork->external_path != nullptr && artwork->external_path[0] != '\0' &&
            !checked_add_size(&string_bytes, strlen(artwork->external_path) + 1U)) {
            heap_caps_free(artist_builds);
            heap_caps_free(album_builds);
            return ESP_ERR_INVALID_SIZE;
        }
        if (metadata != nullptr) {
            for (uint16_t l = 0; l < metadata->lyrics_count; ++l) {
                const MediaMetadataLyricsBuildV2 &lyrics = metadata->lyrics[l];
                if ((lyrics.path != nullptr && lyrics.path[0] != '\0' &&
                     !checked_add_size(&string_bytes, strlen(lyrics.path) + 1U)) ||
                    (lyrics.language != nullptr && lyrics.language[0] != '\0' &&
                     !checked_add_size(&string_bytes, strlen(lyrics.language) + 1U))) {
                    heap_caps_free(artist_builds);
                    heap_caps_free(album_builds);
                    return ESP_ERR_INVALID_SIZE;
                }
            }
        }
    }

    char *strings = static_cast<char *>(catalog_psram_alloc(string_bytes));
    TrackRowV2 *tracks = record_count > 0
        ? static_cast<TrackRowV2 *>(catalog_psram_alloc(record_count * sizeof(TrackRowV2))) : nullptr;
    ArtistRowV2 *artists = artist_count > 0
        ? static_cast<ArtistRowV2 *>(catalog_psram_alloc(static_cast<size_t>(artist_count) * sizeof(ArtistRowV2))) : nullptr;
    AlbumRowV2 *albums = album_count > 0
        ? static_cast<AlbumRowV2 *>(catalog_psram_alloc(static_cast<size_t>(album_count) * sizeof(AlbumRowV2))) : nullptr;
    TrackArtistRefV2 *artist_refs = artist_ref_count > 0
        ? static_cast<TrackArtistRefV2 *>(catalog_psram_alloc(static_cast<size_t>(artist_ref_count) * sizeof(TrackArtistRefV2))) : nullptr;
    LyricsRefV2 *lyrics_refs = lyrics_ref_count > 0
        ? static_cast<LyricsRefV2 *>(catalog_psram_alloc(static_cast<size_t>(lyrics_ref_count) * sizeof(LyricsRefV2))) : nullptr;
    ArtworkRefV2 *artwork_refs = artwork_ref_count > 0
        ? static_cast<ArtworkRefV2 *>(catalog_psram_alloc(static_cast<size_t>(artwork_ref_count) * sizeof(ArtworkRefV2))) : nullptr;
    if (strings == nullptr || (record_count > 0 && tracks == nullptr) ||
        (artist_count > 0 && artists == nullptr) || (album_count > 0 && albums == nullptr) ||
        (artist_ref_count > 0 && artist_refs == nullptr) || (lyrics_ref_count > 0 && lyrics_refs == nullptr) ||
        (artwork_ref_count > 0 && artwork_refs == nullptr)) {
        heap_caps_free(strings);
        heap_caps_free(tracks);
        heap_caps_free(artists);
        heap_caps_free(albums);
        heap_caps_free(artist_refs);
        heap_caps_free(lyrics_refs);
        heap_caps_free(artwork_refs);
        heap_caps_free(artist_builds);
        heap_caps_free(album_builds);
        return ESP_ERR_NO_MEM;
    }
    memset(strings, 0, string_bytes);
    for (size_t i = 0; i < record_count; ++i) tracks[i] = {};
    for (uint32_t i = 0; i < artist_count; ++i) artists[i] = {};
    for (uint32_t i = 0; i < album_count; ++i) albums[i] = {};
    for (uint32_t i = 0; i < artist_ref_count; ++i) artist_refs[i] = {};
    for (uint32_t i = 0; i < lyrics_ref_count; ++i) lyrics_refs[i] = {};
    for (uint32_t i = 0; i < artwork_ref_count; ++i) artwork_refs[i] = {};

    size_t next_string = 1U;
    for (uint32_t i = 0; i < artist_count; ++i) {
        artists[i].name_off = append_pool_string(strings, string_bytes, &next_string, artist_builds[i].name);
        artists[i].flags = 0U;
    }
    for (uint32_t i = 0; i < album_count; ++i) {
        albums[i].title_off = append_pool_string(strings, string_bytes, &next_string, album_builds[i].title);
        albums[i].display_artist_off = append_pool_string(strings, string_bytes, &next_string, album_builds[i].display_artist);
        albums[i].album_artist_id = album_builds[i].album_artist_id;
        albums[i].artwork_track_id = album_builds[i].artwork_track_id;
        albums[i].flags = album_builds[i].flags;
        albums[i].release_year = album_builds[i].release_year;
        albums[i].original_year = album_builds[i].original_year;
    }

    uint32_t next_artist_ref = 0;
    uint32_t next_lyrics_ref = 0;
    uint32_t next_artwork_ref = 0;
    for (size_t i = 0; i < record_count; ++i) {
        const MediaIndexRecord &source = records[i];
        const MediaMetadataBuildV2 *metadata = source.metadata_build;
        const MediaArtworkBuildV2 *artwork = source.artwork_build;
        const char *path = path_pool + source.path_offset;
        const char *filename = filename_from_path(path);
        TrackRowV2 &track = tracks[i];
        track.path_off = append_pool_string(strings, string_bytes, &next_string, path);
        if (metadata != nullptr && metadata->title != nullptr && metadata->title[0] != '\0') {
            track.title_off = append_pool_string(strings, string_bytes, &next_string, metadata->title);
        } else {
            const size_t title_len = title_length_without_extension(path);
            if (next_string + title_len + 1U > string_bytes) {
                heap_caps_free(artist_builds);
                heap_caps_free(album_builds);
                MusicCatalogV2 failed = {};
                failed.pool = {strings, static_cast<uint32_t>(string_bytes)};
                failed.tracks = tracks; failed.artists = artists; failed.albums = albums;
                failed.track_artist_refs = artist_refs; failed.lyrics_refs = lyrics_refs; failed.artwork_refs = artwork_refs;
                failed.track_count = static_cast<uint32_t>(record_count); failed.artist_count = artist_count;
                failed.album_count = album_count; failed.track_artist_ref_count = artist_ref_count;
                failed.lyrics_ref_count = lyrics_ref_count; failed.artwork_ref_count = artwork_ref_count;
                media_catalog_v2_release(&failed);
                return ESP_ERR_INVALID_SIZE;
            }
            track.title_off = static_cast<uint32_t>(next_string);
            memcpy(strings + next_string, filename, title_len);
            strings[next_string + title_len] = '\0';
            next_string += title_len + 1U;
        }
        track.display_artist_off = metadata != nullptr
            ? append_pool_string(strings, string_bytes, &next_string, metadata->display_artist) : 0U;

        track.artist_ref_start = next_artist_ref;
        track.artist_ref_count = metadata != nullptr ? metadata->artist_count : 0U;
        if (metadata != nullptr) {
            for (uint16_t a = 0; a < metadata->artist_count; ++a) {
                const uint32_t artist_id = find_artist_build(artist_builds, artist_count, metadata->artists[a]);
                if (artist_id == MEDIA_CATALOG_INVALID_ID_V2) {
                    heap_caps_free(artist_builds);
                    heap_caps_free(album_builds);
                    MusicCatalogV2 failed = {};
                    failed.pool = {strings, static_cast<uint32_t>(string_bytes)};
                    failed.tracks = tracks; failed.artists = artists; failed.albums = albums;
                    failed.track_artist_refs = artist_refs; failed.lyrics_refs = lyrics_refs; failed.artwork_refs = artwork_refs;
                    failed.track_count = static_cast<uint32_t>(record_count); failed.artist_count = artist_count;
                    failed.album_count = album_count; failed.track_artist_ref_count = artist_ref_count;
                    failed.lyrics_ref_count = lyrics_ref_count; failed.artwork_ref_count = artwork_ref_count;
                    media_catalog_v2_release(&failed);
                    return ESP_ERR_INVALID_RESPONSE;
                }
                artist_refs[next_artist_ref++] = {artist_id, MEDIA_TRACK_ARTIST_REF_NONE_V2};
            }
        }

        track.lyrics_ref_start = next_lyrics_ref;
        track.lyrics_ref_count = metadata != nullptr ? metadata->lyrics_count : 0U;
        if (metadata != nullptr) {
            for (uint16_t l = 0; l < metadata->lyrics_count; ++l) {
                const MediaMetadataLyricsBuildV2 &source_lyrics = metadata->lyrics[l];
                LyricsRefV2 &dest = lyrics_refs[next_lyrics_ref++];
                dest.data_offset = source_lyrics.data_offset;
                dest.data_size = source_lyrics.data_size;
                dest.path_off = append_pool_string(strings, string_bytes, &next_string, source_lyrics.path);
                dest.language_off = append_pool_string(strings, string_bytes, &next_string, source_lyrics.language);
                dest.flags = source_lyrics.flags;
                dest.source = source_lyrics.source;
                dest.kind = source_lyrics.kind;
                dest.encoding = source_lyrics.encoding;
            }
        }

        track.album_id = MEDIA_CATALOG_INVALID_ID_V2;
        if (metadata != nullptr && metadata->album != nullptr && metadata->album[0] != '\0') {
            const char *album_artist = metadata->album_artist != nullptr && metadata->album_artist[0] != '\0'
                ? metadata->album_artist : (metadata->display_artist != nullptr ? metadata->display_artist : "");
            track.album_id = find_album_build(album_builds, album_count, metadata->album, album_artist);
        }
        track.artwork_ref_id = MEDIA_CATALOG_INVALID_ID_V2;
        if (artwork != nullptr && artwork->source != MediaArtworkSourceV2::None) {
            ArtworkRefV2 &dest = artwork_refs[next_artwork_ref];
            dest.data_offset = artwork->data_offset;
            dest.data_size = artwork->data_size;
            dest.source_modified_time = artwork->source_modified_time;
            dest.path_off = artwork->source == MediaArtworkSourceV2::ExternalFile
                ? append_pool_string(strings, string_bytes, &next_string, artwork->external_path) : 0U;
            dest.flags = artwork->flags;
            dest.width = artwork->width;
            dest.height = artwork->height;
            dest.source = artwork->source;
            dest.format = artwork->format;
            dest.picture_type = artwork->picture_type;
            track.artwork_ref_id = next_artwork_ref++;
        }
        track.metadata_flags = metadata != nullptr ? metadata->metadata_flags : MEDIA_TRACK_META_NONE_V2;
        track.track_number = metadata != nullptr ? metadata->track_number : 0U;
        track.track_total = metadata != nullptr ? metadata->track_total : 0U;
        track.disc_number = metadata != nullptr ? metadata->disc_number : 0U;
        track.disc_total = metadata != nullptr ? metadata->disc_total : 0U;
        track.release_year = metadata != nullptr ? metadata->release_year : 0U;
        track.original_year = metadata != nullptr ? metadata->original_year : 0U;
        track.file_size_bytes = source.file_size_bytes;
        track.format = source.format;
        track.technical = source.technical;
    }

    heap_caps_free(artist_builds);
    heap_caps_free(album_builds);
    if (next_string != string_bytes || next_artist_ref != artist_ref_count || next_lyrics_ref != lyrics_ref_count ||
        next_artwork_ref != artwork_ref_count) {
        MusicCatalogV2 failed = {};
        failed.pool = {strings, static_cast<uint32_t>(string_bytes)};
        failed.tracks = tracks; failed.artists = artists; failed.albums = albums;
        failed.track_artist_refs = artist_refs; failed.lyrics_refs = lyrics_refs; failed.artwork_refs = artwork_refs;
        failed.track_count = static_cast<uint32_t>(record_count); failed.artist_count = artist_count;
        failed.album_count = album_count; failed.track_artist_ref_count = artist_ref_count;
        failed.lyrics_ref_count = lyrics_ref_count; failed.artwork_ref_count = artwork_ref_count;
        media_catalog_v2_release(&failed);
        return ESP_ERR_INVALID_SIZE;
    }

    out_catalog->pool.data = strings;
    out_catalog->pool.size = static_cast<uint32_t>(string_bytes);
    out_catalog->tracks = tracks;
    out_catalog->artists = artists;
    out_catalog->albums = albums;
    out_catalog->track_artist_refs = artist_refs;
    out_catalog->lyrics_refs = lyrics_refs;
    out_catalog->artwork_refs = artwork_refs;
    out_catalog->track_count = static_cast<uint32_t>(record_count);
    out_catalog->artist_count = artist_count;
    out_catalog->album_count = album_count;
    out_catalog->track_artist_ref_count = artist_ref_count;
    out_catalog->lyrics_ref_count = lyrics_ref_count;
    out_catalog->artwork_ref_count = artwork_ref_count;

    const esp_err_t validate_ret = media_catalog_v2_validate(out_catalog);
    if (validate_ret != ESP_OK) {
        media_catalog_v2_release(out_catalog);
        return validate_ret;
    }
    return ESP_OK;
}

esp_err_t media_catalog_v2_publish(MusicCatalogV2 *catalog, uint32_t source_crc32)
{
    if (catalog == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        // 当前运行时大量 View 直接引用 Catalog 内存；在没有 lease/refcount 之前，
        // 运行期替换会让旧指针立即失效。现阶段明确保持“每次启动只发布一次”。
        ESP_LOGE(TAG, "拒绝运行期替换 Catalog：当前 generation=%lu",
            static_cast<unsigned long>(s_catalog.generation));
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t validate_ret = media_catalog_v2_validate(catalog);
    if (validate_ret != ESP_OK) {
        return validate_ret;
    }
    const esp_err_t groups_ret = media_groups_v2_build(catalog);
    if (groups_ret != ESP_OK) {
        ESP_LOGE(TAG, "构建运行时分组失败：%s", esp_err_to_name(groups_ret));
        return groups_ret;
    }

    media_catalog_v2_release(&s_catalog);
    s_catalog = *catalog;
    *catalog = {};
    s_catalog.source_crc32 = source_crc32;
    s_catalog.generation = ++s_generation_seq;
    if (s_catalog.generation == 0) {
        s_catalog.generation = ++s_generation_seq;
    }
    s_ready = true;

    CATALOG_BOOT_LOGI("Catalog 已发布：generation=%lu tracks=%lu artists=%lu albums=%lu artist_refs=%lu lyrics_refs=%lu artwork_refs=%lu groups(A/AL/D)=%lu/%lu/%lu strings=%luB crc=0x%08lX",
        static_cast<unsigned long>(s_catalog.generation),
        static_cast<unsigned long>(s_catalog.track_count),
        static_cast<unsigned long>(s_catalog.artist_count),
        static_cast<unsigned long>(s_catalog.album_count),
        static_cast<unsigned long>(s_catalog.track_artist_ref_count),
        static_cast<unsigned long>(s_catalog.lyrics_ref_count),
        static_cast<unsigned long>(s_catalog.artwork_ref_count),
        static_cast<unsigned long>(s_catalog.artist_group_count),
        static_cast<unsigned long>(s_catalog.album_group_count),
        static_cast<unsigned long>(s_catalog.decade_group_count),
        static_cast<unsigned long>(s_catalog.pool.size),
        static_cast<unsigned long>(s_catalog.source_crc32));
    return ESP_OK;
}

bool media_catalog_v2_ready()
{
    return s_ready;
}

const MusicCatalogV2 *media_catalog_v2_current()
{
    return s_ready ? &s_catalog : nullptr;
}

uint32_t media_catalog_v2_generation()
{
    return s_ready ? s_catalog.generation : 0;
}

bool media_catalog_v2_get_track_view(size_t index, MediaTrackViewV2 *out_view)
{
    if (!s_ready || out_view == nullptr || index >= s_catalog.track_count) {
        return false;
    }
    const TrackRowV2 &track = s_catalog.tracks[index];
    MediaTrackViewV2 view = {};
    view.generation = s_catalog.generation;
    view.track_index = static_cast<uint32_t>(index);
    view.row = &track;
    view.path = media_catalog_v2_pool_str(&s_catalog, track.path_off);
    view.title = media_catalog_v2_pool_str(&s_catalog, track.title_off);
    view.artist = media_catalog_v2_pool_str(&s_catalog, track.display_artist_off);
    view.album = track.album_id == MEDIA_CATALOG_INVALID_ID_V2
        ? media_catalog_v2_pool_str(&s_catalog, 0)
        : media_catalog_v2_pool_str(&s_catalog, s_catalog.albums[track.album_id].title_off);
    if (view.path == nullptr || view.title == nullptr || view.artist == nullptr || view.album == nullptr) {
        return false;
    }
    *out_view = view;
    return true;
}

bool media_catalog_v2_copy_technical(size_t index, MediaTechnicalInfo *out_info)
{
    if (!s_ready || out_info == nullptr || index >= s_catalog.track_count) {
        return false;
    }
    *out_info = s_catalog.tracks[index].technical;
    return true;
}


bool media_catalog_v2_get_artwork_view(size_t index, MediaArtworkViewV2 *out_view)
{
    if (!s_ready || out_view == nullptr || index >= s_catalog.track_count) {
        return false;
    }
    const TrackRowV2 &track = s_catalog.tracks[index];
    if (track.artwork_ref_id == MEDIA_CATALOG_INVALID_ID_V2 ||
        track.artwork_ref_id >= s_catalog.artwork_ref_count) {
        return false;
    }
    const ArtworkRefV2 &ref = s_catalog.artwork_refs[track.artwork_ref_id];
    MediaArtworkViewV2 view = {};
    view.generation = s_catalog.generation;
    view.track_index = static_cast<uint32_t>(index);
    view.ref = &ref;
    if (ref.source == MediaArtworkSourceV2::ExternalFile) {
        view.external_path = media_catalog_v2_pool_str(&s_catalog, ref.path_off);
        if (view.external_path == nullptr || view.external_path[0] == '\0') {
            return false;
        }
    }
    *out_view = view;
    return true;
}
