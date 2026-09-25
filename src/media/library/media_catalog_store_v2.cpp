#include "media_catalog_store_v2.h"
#include "storage_io.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "media_index_store.h"
#include "system_paths.h"
#include "app_diag_config.h"

static const char *TAG = "曲库索引V2";

#if APP_DIAG_BOOT_VERBOSE
#define CATALOG_STORE_BOOT_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define CATALOG_STORE_BOOT_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif
// R25：新增 OpusPicture locator；旧 version=4 自动失效并重建，确保既有 .opus 重新扫描内嵌封面。
static constexpr uint16_t CATALOG_VERSION_V2 = 5;
static constexpr uint16_t MANIFEST_VERSION_V2 = 2;
static constexpr uint32_t SIGNATURE_MODE_FAST_V2 = 1;
static constexpr uint32_t MAX_TRACKS_V2 = 100000;
static constexpr uint32_t MAX_ARTISTS_V2 = 100000;
static constexpr uint32_t MAX_ALBUMS_V2 = 100000;
static constexpr uint32_t MAX_TRACK_ARTIST_REFS_V2 = 1000000;
static constexpr uint32_t MAX_LYRICS_REFS_V2 = 400000;
static constexpr uint32_t MAX_ARTWORK_REFS_V2 = 100000;
static constexpr uint32_t MAX_STRING_POOL_V2 = 32U * 1024U * 1024U;
static constexpr uint16_t SECTION_COUNT_V2 = 7;

enum CatalogSectionTypeV2 : uint32_t
{
    SEC_V2_STR_POOL = 1,
    SEC_V2_ARTISTS = 2,
    SEC_V2_ALBUMS = 3,
    SEC_V2_TRACK_ARTIST_REFS = 4,
    SEC_V2_LYRICS_REFS = 5,
    SEC_V2_ARTWORK_REFS = 6,
    SEC_V2_TRACKS = 7,
};

#pragma pack(push, 1)
struct CatalogFileHeaderV2
{
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint16_t section_entry_size;
    uint16_t section_count;
    uint32_t file_size;
    uint32_t payload_crc32;
    uint32_t signature_mode;
    uint32_t track_count;
    uint32_t artist_count;
    uint32_t album_count;
    uint32_t track_artist_ref_count;
    uint32_t lyrics_ref_count;
    uint32_t artwork_ref_count;
};

struct CatalogSectionV2
{
    uint32_t type;
    uint32_t offset;
    uint32_t size;
    uint32_t count;
    uint16_t row_size;
    uint16_t flags;
    uint32_t crc32;
};

struct ArtistDiskRowV2
{
    uint32_t name_off;
    uint32_t flags;
};

struct AlbumDiskRowV2
{
    uint32_t title_off;
    uint32_t display_artist_off;
    uint32_t album_artist_id;
    uint32_t artwork_track_id;
    uint32_t flags;
    uint16_t release_year;
    uint16_t original_year;
};

struct TrackArtistDiskRefV2
{
    uint32_t artist_id;
    uint32_t flags;
};

struct LyricsDiskRefV2
{
    uint64_t data_offset;
    uint32_t data_size;
    uint32_t path_off;
    uint32_t language_off;
    uint32_t flags;
    uint8_t source;
    uint8_t kind;
    uint8_t encoding;
    uint8_t reserved0;
};

struct ArtworkDiskRefV2
{
    uint64_t data_offset;
    uint32_t data_size;
    uint32_t path_off;
    int64_t source_modified_time;
    uint32_t flags;
    uint16_t width;
    uint16_t height;
    uint8_t source;
    uint8_t format;
    uint8_t picture_type;
    uint8_t reserved0;
};

struct TrackDiskRowV2
{
    uint32_t path_off;
    uint32_t title_off;
    uint32_t display_artist_off;
    uint32_t artist_ref_start;
    uint16_t artist_ref_count;
    uint16_t reserved0;
    uint32_t lyrics_ref_start;
    uint16_t lyrics_ref_count;
    uint16_t reserved1;
    uint32_t album_id;
    uint32_t artwork_ref_id;
    uint32_t metadata_flags;
    uint16_t track_number;
    uint16_t track_total;
    uint16_t disc_number;
    uint16_t disc_total;
    uint16_t release_year;
    uint16_t original_year;
    uint8_t format;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t reserved2;
    uint32_t technical_flags;
    uint64_t file_size_bytes;
    uint32_t sample_rate_hz;
    uint32_t bitrate_kbps;
    uint32_t duration_ms;
    uint64_t total_frames;
    uint64_t audio_data_offset;
    uint64_t metadata_end_offset;
    uint64_t artwork_offset;
    uint32_t artwork_size;
    uint32_t max_frame_size;
    uint16_t max_block_size;
    uint16_t samples_per_frame;
};

struct ManifestFileHeaderV2
{
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint16_t record_size;
    uint16_t reserved0;
    uint32_t record_count;
    uint32_t index_payload_crc32;
    uint32_t payload_crc32;
    uint32_t signature_mode;
};

struct ManifestDiskRowV2
{
    uint32_t track_index;
    uint8_t format;
    uint8_t reserved[3];
    uint64_t file_size_bytes;
    int64_t modified_time;
};
#pragma pack(pop)

static_assert(sizeof(CatalogFileHeaderV2) == 52, "CatalogFileHeaderV2 layout changed");
static_assert(sizeof(CatalogSectionV2) == 24, "CatalogSectionV2 layout changed");
static_assert(sizeof(ArtistDiskRowV2) == 8, "ArtistDiskRowV2 layout changed");
static_assert(sizeof(AlbumDiskRowV2) == 24, "AlbumDiskRowV2 layout changed");
static_assert(sizeof(TrackArtistDiskRefV2) == 8, "TrackArtistDiskRefV2 layout changed");
static_assert(sizeof(LyricsDiskRefV2) == 28, "LyricsDiskRefV2 layout changed");
static_assert(sizeof(ArtworkDiskRefV2) == 36, "ArtworkDiskRefV2 layout changed");
static_assert(sizeof(TrackDiskRowV2) == 124, "TrackDiskRowV2 layout changed");
static_assert(sizeof(ManifestFileHeaderV2) == 32, "ManifestFileHeaderV2 layout changed");
static_assert(sizeof(ManifestDiskRowV2) == 24, "ManifestDiskRowV2 layout changed");

// Stage 12.0.1：Catalog 事务会在 main task 的曲库扫描调用栈内执行。
// 读回校验涉及 header/section/stat/row scratch；这些是事务工作区，不应长期占用任务栈。
union CatalogDiskRowScratchV2
{
    ArtistDiskRowV2 artist;
    AlbumDiskRowV2 album;
    TrackArtistDiskRefV2 track_artist;
    LyricsDiskRefV2 lyrics;
    ArtworkDiskRefV2 artwork;
    TrackDiskRowV2 track;
};

struct CatalogLoadScratchV2
{
    CatalogFileHeaderV2 header = {};
    CatalogSectionV2 sections[SECTION_COUNT_V2] = {};
    struct stat file_info = {};
    MusicCatalogV2 loaded = {};
    CatalogDiskRowScratchV2 row = {};
};

struct CatalogCommitScratchV2
{
    CatalogSectionV2 sections[SECTION_COUNT_V2] = {};
    MediaCatalogSnapshotV2 verify = {};
};

static uint32_t crc32_begin()
{
    return 0xFFFFFFFFU;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

static uint32_t crc32_end(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFU;
}

static uint32_t crc32_buffer(const void *data, size_t size)
{
    return crc32_end(crc32_update(crc32_begin(), data, size));
}

static void *store_psram_alloc(size_t bytes)
{
    if (bytes == 0) {
        return nullptr;
    }
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool flush_file(FILE *file)
{
    if (file == nullptr || fflush(file) != 0) {
        return false;
    }
    const int descriptor = fileno(file);
    return descriptor < 0 || fsync(descriptor) == 0;
}

static bool ensure_directory(const char *path)
{
    struct stat info = {};
    if (stat(path, &info) == 0) {
        return S_ISDIR(info.st_mode);
    }
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return true;
    }
    ESP_LOGE(TAG, "创建目录失败：%s errno=%d", path, errno);
    return false;
}

static bool ensure_library_directory()
{
    return ensure_directory(SystemPaths::kSystemDirectory) &&
        ensure_directory(SystemPaths::kLibraryDirectory);
}

static AlbumDiskRowV2 to_disk_album(const AlbumRowV2 &source)
{
    AlbumDiskRowV2 row = {};
    row.title_off = source.title_off;
    row.display_artist_off = source.display_artist_off;
    row.album_artist_id = source.album_artist_id;
    row.artwork_track_id = source.artwork_track_id;
    row.flags = source.flags;
    row.release_year = source.release_year;
    row.original_year = source.original_year;
    return row;
}

static AlbumRowV2 from_disk_album(const AlbumDiskRowV2 &source)
{
    AlbumRowV2 row = {};
    row.title_off = source.title_off;
    row.display_artist_off = source.display_artist_off;
    row.album_artist_id = source.album_artist_id;
    row.artwork_track_id = source.artwork_track_id;
    row.flags = source.flags;
    row.release_year = source.release_year;
    row.original_year = source.original_year;
    return row;
}

static TrackArtistDiskRefV2 to_disk_track_artist_ref(const TrackArtistRefV2 &source)
{
    TrackArtistDiskRefV2 row = {};
    row.artist_id = source.artist_id;
    row.flags = source.flags;
    return row;
}

static TrackArtistRefV2 from_disk_track_artist_ref(const TrackArtistDiskRefV2 &source)
{
    TrackArtistRefV2 row = {};
    row.artist_id = source.artist_id;
    row.flags = source.flags;
    return row;
}

static LyricsDiskRefV2 to_disk_lyrics_ref(const LyricsRefV2 &source)
{
    LyricsDiskRefV2 row = {};
    row.data_offset = source.data_offset;
    row.data_size = source.data_size;
    row.path_off = source.path_off;
    row.language_off = source.language_off;
    row.flags = source.flags;
    row.source = static_cast<uint8_t>(source.source);
    row.kind = static_cast<uint8_t>(source.kind);
    row.encoding = static_cast<uint8_t>(source.encoding);
    row.reserved0 = source.reserved0;
    return row;
}

static LyricsRefV2 from_disk_lyrics_ref(const LyricsDiskRefV2 &source)
{
    LyricsRefV2 row = {};
    row.data_offset = source.data_offset;
    row.data_size = source.data_size;
    row.path_off = source.path_off;
    row.language_off = source.language_off;
    row.flags = source.flags;
    row.source = static_cast<MediaLyricsSourceV2>(source.source);
    row.kind = static_cast<MediaLyricsKindV2>(source.kind);
    row.encoding = static_cast<MediaLyricsEncodingV2>(source.encoding);
    row.reserved0 = source.reserved0;
    return row;
}

static ArtworkDiskRefV2 to_disk_artwork_ref(const ArtworkRefV2 &source)
{
    ArtworkDiskRefV2 row = {};
    row.data_offset = source.data_offset;
    row.data_size = source.data_size;
    row.path_off = source.path_off;
    row.source_modified_time = source.source_modified_time;
    row.flags = source.flags;
    row.width = source.width;
    row.height = source.height;
    row.source = static_cast<uint8_t>(source.source);
    row.format = static_cast<uint8_t>(source.format);
    row.picture_type = source.picture_type;
    row.reserved0 = source.reserved0;
    return row;
}

static ArtworkRefV2 from_disk_artwork_ref(const ArtworkDiskRefV2 &source)
{
    ArtworkRefV2 row = {};
    row.data_offset = source.data_offset;
    row.data_size = source.data_size;
    row.path_off = source.path_off;
    row.source_modified_time = source.source_modified_time;
    row.flags = source.flags;
    row.width = source.width;
    row.height = source.height;
    row.source = static_cast<MediaArtworkSourceV2>(source.source);
    row.format = static_cast<MediaArtworkFormatV2>(source.format);
    row.picture_type = source.picture_type;
    row.reserved0 = source.reserved0;
    return row;
}

static TrackDiskRowV2 to_disk_track(const TrackRowV2 &source)
{
    TrackDiskRowV2 row = {};
    row.path_off = source.path_off;
    row.title_off = source.title_off;
    row.display_artist_off = source.display_artist_off;
    row.artist_ref_start = source.artist_ref_start;
    row.artist_ref_count = source.artist_ref_count;
    row.reserved0 = source.reserved0;
    row.lyrics_ref_start = source.lyrics_ref_start;
    row.lyrics_ref_count = source.lyrics_ref_count;
    row.reserved1 = source.reserved1;
    row.album_id = source.album_id;
    row.artwork_ref_id = source.artwork_ref_id;
    row.metadata_flags = source.metadata_flags;
    row.track_number = source.track_number;
    row.track_total = source.track_total;
    row.disc_number = source.disc_number;
    row.disc_total = source.disc_total;
    row.release_year = source.release_year;
    row.original_year = source.original_year;
    row.format = static_cast<uint8_t>(source.format);
    row.channels = source.technical.channels;
    row.bits_per_sample = source.technical.bits_per_sample;
    row.technical_flags = source.technical.flags;
    row.file_size_bytes = source.file_size_bytes;
    row.sample_rate_hz = source.technical.sample_rate_hz;
    row.bitrate_kbps = source.technical.bitrate_kbps;
    row.duration_ms = source.technical.duration_ms;
    row.total_frames = source.technical.total_frames;
    row.audio_data_offset = source.technical.audio_data_offset;
    row.metadata_end_offset = source.technical.metadata_end_offset;
    row.artwork_offset = source.technical.artwork_offset;
    row.artwork_size = source.technical.artwork_size;
    row.max_frame_size = source.technical.max_frame_size;
    row.max_block_size = source.technical.max_block_size;
    row.samples_per_frame = source.technical.samples_per_frame;
    return row;
}

static TrackRowV2 from_disk_track(const TrackDiskRowV2 &source)
{
    TrackRowV2 row = {};
    row.path_off = source.path_off;
    row.title_off = source.title_off;
    row.display_artist_off = source.display_artist_off;
    row.artist_ref_start = source.artist_ref_start;
    row.artist_ref_count = source.artist_ref_count;
    row.reserved0 = source.reserved0;
    row.lyrics_ref_start = source.lyrics_ref_start;
    row.lyrics_ref_count = source.lyrics_ref_count;
    row.reserved1 = source.reserved1;
    row.album_id = source.album_id;
    row.artwork_ref_id = source.artwork_ref_id;
    row.metadata_flags = source.metadata_flags;
    row.track_number = source.track_number;
    row.track_total = source.track_total;
    row.disc_number = source.disc_number;
    row.disc_total = source.disc_total;
    row.release_year = source.release_year;
    row.original_year = source.original_year;
    row.file_size_bytes = source.file_size_bytes;
    row.format = static_cast<MediaFormat>(source.format);
    row.technical.flags = source.technical_flags;
    row.technical.sample_rate_hz = source.sample_rate_hz;
    row.technical.bitrate_kbps = source.bitrate_kbps;
    row.technical.duration_ms = source.duration_ms;
    row.technical.total_frames = source.total_frames;
    row.technical.audio_data_offset = source.audio_data_offset;
    row.technical.metadata_end_offset = source.metadata_end_offset;
    row.technical.artwork_offset = source.artwork_offset;
    row.technical.artwork_size = source.artwork_size;
    row.technical.max_frame_size = source.max_frame_size;
    row.technical.max_block_size = source.max_block_size;
    row.technical.samples_per_frame = source.samples_per_frame;
    row.technical.channels = source.channels;
    row.technical.bits_per_sample = source.bits_per_sample;
    return row;
}

static bool checked_u32_mul(uint32_t a, uint32_t b, uint32_t *out)
{
    if (out == nullptr || (a != 0 && b > UINT32_MAX / a)) {
        return false;
    }
    *out = a * b;
    return true;
}

static esp_err_t build_section_table(
    const MusicCatalogV2 *catalog,
    CatalogSectionV2 sections[SECTION_COUNT_V2],
    uint32_t *out_file_size,
    uint32_t *out_payload_crc
)
{
    if (catalog == nullptr || sections == nullptr || out_file_size == nullptr || out_payload_crc == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t artists_size = 0;
    uint32_t albums_size = 0;
    uint32_t track_artist_refs_size = 0;
    uint32_t lyrics_refs_size = 0;
    uint32_t artwork_refs_size = 0;
    uint32_t tracks_size = 0;
    if (!checked_u32_mul(catalog->artist_count, sizeof(ArtistDiskRowV2), &artists_size) ||
        !checked_u32_mul(catalog->album_count, sizeof(AlbumDiskRowV2), &albums_size) ||
        !checked_u32_mul(catalog->track_artist_ref_count, sizeof(TrackArtistDiskRefV2), &track_artist_refs_size) ||
        !checked_u32_mul(catalog->lyrics_ref_count, sizeof(LyricsDiskRefV2), &lyrics_refs_size) ||
        !checked_u32_mul(catalog->artwork_ref_count, sizeof(ArtworkDiskRefV2), &artwork_refs_size) ||
        !checked_u32_mul(catalog->track_count, sizeof(TrackDiskRowV2), &tracks_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t offset = sizeof(CatalogFileHeaderV2) + SECTION_COUNT_V2 * sizeof(CatalogSectionV2);
    sections[0] = {SEC_V2_STR_POOL, offset, catalog->pool.size, catalog->pool.size, 1, 0,
        crc32_buffer(catalog->pool.data, catalog->pool.size)};
    offset += catalog->pool.size;

    uint32_t artists_crc = crc32_begin();
    for (uint32_t i = 0; i < catalog->artist_count; ++i) {
        const ArtistDiskRowV2 row = {catalog->artists[i].name_off, catalog->artists[i].flags};
        artists_crc = crc32_update(artists_crc, &row, sizeof(row));
    }
    sections[1] = {SEC_V2_ARTISTS, offset, artists_size, catalog->artist_count,
        sizeof(ArtistDiskRowV2), 0, crc32_end(artists_crc)};
    offset += artists_size;

    uint32_t albums_crc = crc32_begin();
    for (uint32_t i = 0; i < catalog->album_count; ++i) {
        const AlbumDiskRowV2 row = to_disk_album(catalog->albums[i]);
        albums_crc = crc32_update(albums_crc, &row, sizeof(row));
    }
    sections[2] = {SEC_V2_ALBUMS, offset, albums_size, catalog->album_count,
        sizeof(AlbumDiskRowV2), 0, crc32_end(albums_crc)};
    offset += albums_size;

    uint32_t artist_refs_crc = crc32_begin();
    for (uint32_t i = 0; i < catalog->track_artist_ref_count; ++i) {
        const TrackArtistDiskRefV2 row = to_disk_track_artist_ref(catalog->track_artist_refs[i]);
        artist_refs_crc = crc32_update(artist_refs_crc, &row, sizeof(row));
    }
    sections[3] = {SEC_V2_TRACK_ARTIST_REFS, offset, track_artist_refs_size, catalog->track_artist_ref_count,
        sizeof(TrackArtistDiskRefV2), 0, crc32_end(artist_refs_crc)};
    offset += track_artist_refs_size;

    uint32_t lyrics_refs_crc = crc32_begin();
    for (uint32_t i = 0; i < catalog->lyrics_ref_count; ++i) {
        const LyricsDiskRefV2 row = to_disk_lyrics_ref(catalog->lyrics_refs[i]);
        lyrics_refs_crc = crc32_update(lyrics_refs_crc, &row, sizeof(row));
    }
    sections[4] = {SEC_V2_LYRICS_REFS, offset, lyrics_refs_size, catalog->lyrics_ref_count,
        sizeof(LyricsDiskRefV2), 0, crc32_end(lyrics_refs_crc)};
    offset += lyrics_refs_size;

    uint32_t artwork_refs_crc = crc32_begin();
    for (uint32_t i = 0; i < catalog->artwork_ref_count; ++i) {
        const ArtworkDiskRefV2 row = to_disk_artwork_ref(catalog->artwork_refs[i]);
        artwork_refs_crc = crc32_update(artwork_refs_crc, &row, sizeof(row));
    }
    sections[5] = {SEC_V2_ARTWORK_REFS, offset, artwork_refs_size, catalog->artwork_ref_count,
        sizeof(ArtworkDiskRefV2), 0, crc32_end(artwork_refs_crc)};
    offset += artwork_refs_size;

    uint32_t tracks_crc = crc32_begin();
    for (uint32_t i = 0; i < catalog->track_count; ++i) {
        const TrackDiskRowV2 row = to_disk_track(catalog->tracks[i]);
        tracks_crc = crc32_update(tracks_crc, &row, sizeof(row));
    }
    sections[6] = {SEC_V2_TRACKS, offset, tracks_size, catalog->track_count,
        sizeof(TrackDiskRowV2), 0, crc32_end(tracks_crc)};
    offset += tracks_size;

    uint32_t payload_crc = crc32_begin();
    payload_crc = crc32_update(payload_crc, catalog->pool.data, catalog->pool.size);
    for (uint32_t i = 0; i < catalog->artist_count; ++i) {
        const ArtistDiskRowV2 row = {catalog->artists[i].name_off, catalog->artists[i].flags};
        payload_crc = crc32_update(payload_crc, &row, sizeof(row));
    }
    for (uint32_t i = 0; i < catalog->album_count; ++i) {
        const AlbumDiskRowV2 row = to_disk_album(catalog->albums[i]);
        payload_crc = crc32_update(payload_crc, &row, sizeof(row));
    }
    for (uint32_t i = 0; i < catalog->track_artist_ref_count; ++i) {
        const TrackArtistDiskRefV2 row = to_disk_track_artist_ref(catalog->track_artist_refs[i]);
        payload_crc = crc32_update(payload_crc, &row, sizeof(row));
    }
    for (uint32_t i = 0; i < catalog->lyrics_ref_count; ++i) {
        const LyricsDiskRefV2 row = to_disk_lyrics_ref(catalog->lyrics_refs[i]);
        payload_crc = crc32_update(payload_crc, &row, sizeof(row));
    }
    for (uint32_t i = 0; i < catalog->artwork_ref_count; ++i) {
        const ArtworkDiskRefV2 row = to_disk_artwork_ref(catalog->artwork_refs[i]);
        payload_crc = crc32_update(payload_crc, &row, sizeof(row));
    }
    for (uint32_t i = 0; i < catalog->track_count; ++i) {
        const TrackDiskRowV2 row = to_disk_track(catalog->tracks[i]);
        payload_crc = crc32_update(payload_crc, &row, sizeof(row));
    }

    *out_file_size = offset;
    *out_payload_crc = crc32_end(payload_crc);
    return ESP_OK;
}

static esp_err_t write_catalog_file(
    const char *path,
    const MusicCatalogV2 *catalog,
    const CatalogSectionV2 *sections,
    uint32_t file_size,
    uint32_t payload_crc
)
{
    if (path == nullptr || catalog == nullptr || sections == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        return ESP_FAIL;
    }
    CatalogFileHeaderV2 header = {};
    memcpy(header.magic, "FPCATV2", 7);
    header.version = CATALOG_VERSION_V2;
    header.header_size = sizeof(header);
    header.section_entry_size = sizeof(CatalogSectionV2);
    header.section_count = SECTION_COUNT_V2;
    header.file_size = file_size;
    header.payload_crc32 = payload_crc;
    header.signature_mode = SIGNATURE_MODE_FAST_V2;
    header.track_count = catalog->track_count;
    header.artist_count = catalog->artist_count;
    header.album_count = catalog->album_count;
    header.track_artist_ref_count = catalog->track_artist_ref_count;
    header.lyrics_ref_count = catalog->lyrics_ref_count;
    header.artwork_ref_count = catalog->artwork_ref_count;

    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
        fwrite(sections, 1, sizeof(CatalogSectionV2) * SECTION_COUNT_V2, file) ==
            sizeof(CatalogSectionV2) * SECTION_COUNT_V2 &&
        fwrite(catalog->pool.data, 1, catalog->pool.size, file) == catalog->pool.size;

    for (uint32_t i = 0; ok && i < catalog->artist_count; ++i) {
        const ArtistDiskRowV2 row = {catalog->artists[i].name_off, catalog->artists[i].flags};
        ok = fwrite(&row, 1, sizeof(row), file) == sizeof(row);
    }
    for (uint32_t i = 0; ok && i < catalog->album_count; ++i) {
        const AlbumDiskRowV2 row = to_disk_album(catalog->albums[i]);
        ok = fwrite(&row, 1, sizeof(row), file) == sizeof(row);
    }
    for (uint32_t i = 0; ok && i < catalog->track_artist_ref_count; ++i) {
        const TrackArtistDiskRefV2 row = to_disk_track_artist_ref(catalog->track_artist_refs[i]);
        ok = fwrite(&row, 1, sizeof(row), file) == sizeof(row);
    }
    for (uint32_t i = 0; ok && i < catalog->lyrics_ref_count; ++i) {
        const LyricsDiskRefV2 row = to_disk_lyrics_ref(catalog->lyrics_refs[i]);
        ok = fwrite(&row, 1, sizeof(row), file) == sizeof(row);
    }
    for (uint32_t i = 0; ok && i < catalog->artwork_ref_count; ++i) {
        const ArtworkDiskRefV2 row = to_disk_artwork_ref(catalog->artwork_refs[i]);
        ok = fwrite(&row, 1, sizeof(row), file) == sizeof(row);
    }
    for (uint32_t i = 0; ok && i < catalog->track_count; ++i) {
        const TrackDiskRowV2 row = to_disk_track(catalog->tracks[i]);
        ok = fwrite(&row, 1, sizeof(row), file) == sizeof(row);
    }
    if (ok) {
        ok = flush_file(file);
    }
    fclose(file);
    return ok ? ESP_OK : ESP_FAIL;
}

static uint32_t calculate_manifest_crc(const MediaIndexRecord *records, size_t count)
{
    uint32_t crc = crc32_begin();
    for (size_t i = 0; i < count; ++i) {
        ManifestDiskRowV2 row = {};
        row.track_index = static_cast<uint32_t>(i);
        row.format = static_cast<uint8_t>(records[i].format);
        row.file_size_bytes = records[i].file_size_bytes;
        row.modified_time = records[i].modified_time;
        crc = crc32_update(crc, &row, sizeof(row));
    }
    return crc32_end(crc);
}

static esp_err_t write_manifest_file(
    const char *path,
    const MediaIndexRecord *records,
    size_t count,
    uint32_t index_crc,
    uint32_t *out_crc
)
{
    if (count > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t payload_crc = calculate_manifest_crc(records, count);
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        return ESP_FAIL;
    }
    ManifestFileHeaderV2 header = {};
    memcpy(header.magic, "FPMNFV2", 7);
    header.version = MANIFEST_VERSION_V2;
    header.header_size = sizeof(header);
    header.record_size = sizeof(ManifestDiskRowV2);
    header.record_count = static_cast<uint32_t>(count);
    header.index_payload_crc32 = index_crc;
    header.payload_crc32 = payload_crc;
    header.signature_mode = SIGNATURE_MODE_FAST_V2;

    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    for (size_t i = 0; ok && i < count; ++i) {
        ManifestDiskRowV2 row = {};
        row.track_index = static_cast<uint32_t>(i);
        row.format = static_cast<uint8_t>(records[i].format);
        row.file_size_bytes = records[i].file_size_bytes;
        row.modified_time = records[i].modified_time;
        ok = fwrite(&row, 1, sizeof(row), file) == sizeof(row);
    }
    if (ok) {
        ok = flush_file(file);
    }
    fclose(file);
    if (ok && out_crc != nullptr) {
        *out_crc = payload_crc;
    }
    return ok ? ESP_OK : ESP_FAIL;
}

static bool section_table_valid(const CatalogFileHeaderV2 &header, const CatalogSectionV2 sections[SECTION_COUNT_V2])
{
    const uint32_t payload_start = sizeof(CatalogFileHeaderV2) + SECTION_COUNT_V2 * sizeof(CatalogSectionV2);
    const uint32_t expected_types[SECTION_COUNT_V2] = {
        SEC_V2_STR_POOL, SEC_V2_ARTISTS, SEC_V2_ALBUMS,
        SEC_V2_TRACK_ARTIST_REFS, SEC_V2_LYRICS_REFS, SEC_V2_ARTWORK_REFS, SEC_V2_TRACKS,
    };
    uint32_t previous_end = payload_start;
    for (uint32_t i = 0; i < SECTION_COUNT_V2; ++i) {
        const CatalogSectionV2 &section = sections[i];
        if (section.type != expected_types[i] || section.offset < payload_start ||
            section.offset != previous_end || section.offset > header.file_size ||
            section.size > header.file_size - section.offset) {
            return false;
        }
        if (section.type == SEC_V2_STR_POOL) {
            if (section.row_size != 1 || section.count != section.size || section.size == 0 ||
                section.size > MAX_STRING_POOL_V2) {
                return false;
            }
        } else if ((section.row_size == 0) ||
                   (section.count > 0 && section.size / section.row_size != section.count) ||
                   section.size != section.count * section.row_size) {
            return false;
        }
        previous_end = section.offset + section.size;
    }
    return previous_end == header.file_size &&
        sections[1].count == header.artist_count &&
        sections[2].count == header.album_count &&
        sections[3].count == header.track_artist_ref_count &&
        sections[4].count == header.lyrics_ref_count &&
        sections[5].count == header.artwork_ref_count &&
        sections[6].count == header.track_count &&
        sections[1].row_size == sizeof(ArtistDiskRowV2) &&
        sections[2].row_size == sizeof(AlbumDiskRowV2) &&
        sections[3].row_size == sizeof(TrackArtistDiskRefV2) &&
        sections[4].row_size == sizeof(LyricsDiskRefV2) &&
        sections[5].row_size == sizeof(ArtworkDiskRefV2) &&
        sections[6].row_size == sizeof(TrackDiskRowV2);
}

static __attribute__((noinline)) esp_err_t load_catalog_file(const char *path, MusicCatalogV2 *catalog, uint32_t *out_crc)
{
    if (catalog == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    media_catalog_v2_release(catalog);
    CatalogLoadScratchV2 *scratch = static_cast<CatalogLoadScratchV2 *>(
        heap_caps_calloc(1, sizeof(CatalogLoadScratchV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (scratch == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    CatalogFileHeaderV2 &header = scratch->header;
    CatalogSectionV2 *sections = scratch->sections;
    struct stat &info = scratch->file_info;
    MusicCatalogV2 &loaded = scratch->loaded;

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        heap_caps_free(scratch);
        return ESP_ERR_NOT_FOUND;
    }

    const bool header_ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
        fread(sections, 1, sizeof(CatalogSectionV2) * SECTION_COUNT_V2, file) ==
            sizeof(CatalogSectionV2) * SECTION_COUNT_V2 &&
        memcmp(header.magic, "FPCATV2", 7) == 0 &&
        header.version == CATALOG_VERSION_V2 &&
        header.header_size == sizeof(CatalogFileHeaderV2) &&
        header.section_entry_size == sizeof(CatalogSectionV2) &&
        header.section_count == SECTION_COUNT_V2 &&
        header.signature_mode == SIGNATURE_MODE_FAST_V2 &&
        header.track_count <= MAX_TRACKS_V2 &&
        header.artist_count <= MAX_ARTISTS_V2 &&
        header.album_count <= MAX_ALBUMS_V2 &&
        header.track_artist_ref_count <= MAX_TRACK_ARTIST_REFS_V2 &&
        header.lyrics_ref_count <= MAX_LYRICS_REFS_V2 &&
        header.artwork_ref_count <= MAX_ARTWORK_REFS_V2 &&
        stat(path, &info) == 0 && info.st_size >= 0 &&
        static_cast<uint64_t>(info.st_size) == header.file_size &&
        section_table_valid(header, sections);
    if (!header_ok) {
        fclose(file);
        heap_caps_free(scratch);
        return ESP_ERR_INVALID_RESPONSE;
    }
    loaded.pool.size = sections[0].size;
    loaded.pool.data = static_cast<char *>(store_psram_alloc(loaded.pool.size));
    if (loaded.pool.data == nullptr) {
        fclose(file);
        heap_caps_free(scratch);
        return ESP_ERR_NO_MEM;
    }
    if (header.artist_count > 0) {
        loaded.artists = static_cast<ArtistRowV2 *>(store_psram_alloc(header.artist_count * sizeof(ArtistRowV2)));
    }
    if (header.album_count > 0) {
        loaded.albums = static_cast<AlbumRowV2 *>(store_psram_alloc(header.album_count * sizeof(AlbumRowV2)));
    }
    if (header.track_artist_ref_count > 0) {
        loaded.track_artist_refs = static_cast<TrackArtistRefV2 *>(
            store_psram_alloc(header.track_artist_ref_count * sizeof(TrackArtistRefV2))
        );
    }
    if (header.lyrics_ref_count > 0) {
        loaded.lyrics_refs = static_cast<LyricsRefV2 *>(
            store_psram_alloc(header.lyrics_ref_count * sizeof(LyricsRefV2))
        );
    }
    if (header.artwork_ref_count > 0) {
        loaded.artwork_refs = static_cast<ArtworkRefV2 *>(
            store_psram_alloc(header.artwork_ref_count * sizeof(ArtworkRefV2))
        );
    }
    if (header.track_count > 0) {
        loaded.tracks = static_cast<TrackRowV2 *>(store_psram_alloc(header.track_count * sizeof(TrackRowV2)));
    }
    if ((header.artist_count > 0 && loaded.artists == nullptr) ||
        (header.album_count > 0 && loaded.albums == nullptr) ||
        (header.track_artist_ref_count > 0 && loaded.track_artist_refs == nullptr) ||
        (header.lyrics_ref_count > 0 && loaded.lyrics_refs == nullptr) ||
        (header.artwork_ref_count > 0 && loaded.artwork_refs == nullptr) ||
        (header.track_count > 0 && loaded.tracks == nullptr)) {
        fclose(file);
        media_catalog_v2_release(&loaded);
        heap_caps_free(scratch);
        return ESP_ERR_NO_MEM;
    }
    loaded.artist_count = header.artist_count;
    loaded.album_count = header.album_count;
    loaded.track_artist_ref_count = header.track_artist_ref_count;
    loaded.lyrics_ref_count = header.lyrics_ref_count;
    loaded.artwork_ref_count = header.artwork_ref_count;
    loaded.track_count = header.track_count;

    uint32_t payload_crc = crc32_begin();
    bool ok = fseek(file, sections[0].offset, SEEK_SET) == 0 &&
        fread(loaded.pool.data, 1, loaded.pool.size, file) == loaded.pool.size;
    if (ok) {
        const uint32_t section_crc = crc32_buffer(loaded.pool.data, loaded.pool.size);
        ok = section_crc == sections[0].crc32;
        payload_crc = crc32_update(payload_crc, loaded.pool.data, loaded.pool.size);
    }

    for (uint32_t i = 0; ok && i < header.artist_count; ++i) {
        ArtistDiskRowV2 &row = scratch->row.artist;
        row = {};
        ok = fread(&row, 1, sizeof(row), file) == sizeof(row);
        if (ok) {
            loaded.artists[i] = {row.name_off, row.flags};
            payload_crc = crc32_update(payload_crc, &row, sizeof(row));
        }
    }
    if (ok) {
        uint32_t crc = crc32_begin();
        for (uint32_t i = 0; i < header.artist_count; ++i) {
            const ArtistDiskRowV2 row = {loaded.artists[i].name_off, loaded.artists[i].flags};
            crc = crc32_update(crc, &row, sizeof(row));
        }
        ok = crc32_end(crc) == sections[1].crc32;
    }

    for (uint32_t i = 0; ok && i < header.album_count; ++i) {
        AlbumDiskRowV2 &row = scratch->row.album;
        row = {};
        ok = fread(&row, 1, sizeof(row), file) == sizeof(row);
        if (ok) {
            loaded.albums[i] = from_disk_album(row);
            payload_crc = crc32_update(payload_crc, &row, sizeof(row));
        }
    }
    if (ok) {
        uint32_t crc = crc32_begin();
        for (uint32_t i = 0; i < header.album_count; ++i) {
            const AlbumDiskRowV2 row = to_disk_album(loaded.albums[i]);
            crc = crc32_update(crc, &row, sizeof(row));
        }
        ok = crc32_end(crc) == sections[2].crc32;
    }

    for (uint32_t i = 0; ok && i < header.track_artist_ref_count; ++i) {
        TrackArtistDiskRefV2 &row = scratch->row.track_artist;
        row = {};
        ok = fread(&row, 1, sizeof(row), file) == sizeof(row);
        if (ok) {
            loaded.track_artist_refs[i] = from_disk_track_artist_ref(row);
            payload_crc = crc32_update(payload_crc, &row, sizeof(row));
        }
    }
    if (ok) {
        uint32_t crc = crc32_begin();
        for (uint32_t i = 0; i < header.track_artist_ref_count; ++i) {
            const TrackArtistDiskRefV2 row = to_disk_track_artist_ref(loaded.track_artist_refs[i]);
            crc = crc32_update(crc, &row, sizeof(row));
        }
        ok = crc32_end(crc) == sections[3].crc32;
    }

    for (uint32_t i = 0; ok && i < header.lyrics_ref_count; ++i) {
        LyricsDiskRefV2 &row = scratch->row.lyrics;
        row = {};
        ok = fread(&row, 1, sizeof(row), file) == sizeof(row);
        if (ok) {
            loaded.lyrics_refs[i] = from_disk_lyrics_ref(row);
            payload_crc = crc32_update(payload_crc, &row, sizeof(row));
        }
    }
    if (ok) {
        uint32_t crc = crc32_begin();
        for (uint32_t i = 0; i < header.lyrics_ref_count; ++i) {
            const LyricsDiskRefV2 row = to_disk_lyrics_ref(loaded.lyrics_refs[i]);
            crc = crc32_update(crc, &row, sizeof(row));
        }
        ok = crc32_end(crc) == sections[4].crc32;
    }

    for (uint32_t i = 0; ok && i < header.artwork_ref_count; ++i) {
        ArtworkDiskRefV2 &row = scratch->row.artwork;
        row = {};
        ok = fread(&row, 1, sizeof(row), file) == sizeof(row);
        if (ok) {
            loaded.artwork_refs[i] = from_disk_artwork_ref(row);
            payload_crc = crc32_update(payload_crc, &row, sizeof(row));
        }
    }
    if (ok) {
        uint32_t crc = crc32_begin();
        for (uint32_t i = 0; i < header.artwork_ref_count; ++i) {
            const ArtworkDiskRefV2 row = to_disk_artwork_ref(loaded.artwork_refs[i]);
            crc = crc32_update(crc, &row, sizeof(row));
        }
        ok = crc32_end(crc) == sections[5].crc32;
    }

    for (uint32_t i = 0; ok && i < header.track_count; ++i) {
        TrackDiskRowV2 &row = scratch->row.track;
        row = {};
        ok = fread(&row, 1, sizeof(row), file) == sizeof(row);
        if (ok) {
            ok = row.reserved0 == 0U && row.reserved1 == 0U && row.reserved2 == 0U;
            if (ok) {
                loaded.tracks[i] = from_disk_track(row);
                payload_crc = crc32_update(payload_crc, &row, sizeof(row));
            }
        }
    }
    if (ok) {
        uint32_t crc = crc32_begin();
        for (uint32_t i = 0; i < header.track_count; ++i) {
            const TrackDiskRowV2 row = to_disk_track(loaded.tracks[i]);
            crc = crc32_update(crc, &row, sizeof(row));
        }
        ok = crc32_end(crc) == sections[6].crc32;
    }
    fclose(file);

    if (!ok || crc32_end(payload_crc) != header.payload_crc32) {
        media_catalog_v2_release(&loaded);
        heap_caps_free(scratch);
        return ESP_ERR_INVALID_CRC;
    }
    const esp_err_t semantic_ret = media_catalog_v2_validate(&loaded);
    if (semantic_ret != ESP_OK) {
        media_catalog_v2_release(&loaded);
        heap_caps_free(scratch);
        return semantic_ret;
    }
    loaded.source_crc32 = header.payload_crc32;
    *catalog = loaded;
    loaded = {};
    if (out_crc != nullptr) {
        *out_crc = header.payload_crc32;
    }
    heap_caps_free(scratch);
    return ESP_OK;
}

static __attribute__((noinline)) esp_err_t load_manifest_file(
    const char *path,
    const MusicCatalogV2 *catalog,
    uint32_t expected_index_crc,
    MediaManifestRecordV2 **out_records,
    uint32_t *out_count,
    uint32_t *out_crc
)
{
    if (catalog == nullptr || out_records == nullptr || out_count == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_records = nullptr;
    *out_count = 0;
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    ManifestFileHeaderV2 header = {};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header.magic, "FPMNFV2", 7) != 0 ||
        header.version != MANIFEST_VERSION_V2 ||
        header.header_size != sizeof(ManifestFileHeaderV2) ||
        header.record_size != sizeof(ManifestDiskRowV2) ||
        header.record_count != catalog->track_count ||
        header.record_count > MAX_TRACKS_V2 ||
        header.index_payload_crc32 != expected_index_crc ||
        header.signature_mode != SIGNATURE_MODE_FAST_V2) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    MediaManifestRecordV2 *records = nullptr;
    if (header.record_count > 0) {
        records = static_cast<MediaManifestRecordV2 *>(
            store_psram_alloc(header.record_count * sizeof(MediaManifestRecordV2))
        );
        if (records == nullptr) {
            fclose(file);
            return ESP_ERR_NO_MEM;
        }
    }

    uint32_t crc = crc32_begin();
    bool ok = true;
    for (uint32_t i = 0; i < header.record_count; ++i) {
        ManifestDiskRowV2 row = {};
        if (fread(&row, 1, sizeof(row), file) != sizeof(row)) {
            ok = false;
            break;
        }
        crc = crc32_update(crc, &row, sizeof(row));
        if (row.track_index != i || row.format != static_cast<uint8_t>(catalog->tracks[i].format) ||
            row.file_size_bytes != catalog->tracks[i].file_size_bytes) {
            ok = false;
            break;
        }
        records[i].track_index = row.track_index;
        records[i].format = static_cast<MediaFormat>(row.format);
        records[i].file_size_bytes = row.file_size_bytes;
        records[i].modified_time = row.modified_time;
    }
    fclose(file);
    crc = crc32_end(crc);
    if (!ok || crc != header.payload_crc32) {
        heap_caps_free(records);
        return ESP_ERR_INVALID_CRC;
    }
    *out_records = records;
    *out_count = header.record_count;
    if (out_crc != nullptr) {
        *out_crc = header.payload_crc32;
    }
    return ESP_OK;
}

static __attribute__((noinline)) esp_err_t load_pair(
    const char *index_path,
    const char *manifest_path,
    MediaCatalogLoadSourceV2 source,
    MediaCatalogSnapshotV2 *snapshot
)
{
    MusicCatalogV2 catalog = {};
    uint32_t index_crc = 0;
    esp_err_t ret = load_catalog_file(index_path, &catalog, &index_crc);
    if (ret != ESP_OK) {
        return ret;
    }
    MediaManifestRecordV2 *manifest = nullptr;
    uint32_t manifest_count = 0;
    uint32_t manifest_crc = 0;
    ret = load_manifest_file(
        manifest_path, &catalog, index_crc, &manifest, &manifest_count, &manifest_crc
    );
    if (ret != ESP_OK) {
        media_catalog_v2_release(&catalog);
        return ret;
    }

    snapshot->catalog = catalog;
    snapshot->manifest = manifest;
    snapshot->manifest_count = manifest_count;
    snapshot->index_crc32 = index_crc;
    snapshot->manifest_crc32 = manifest_crc;
    snapshot->source = source;
    return ESP_OK;
}

void media_catalog_store_v2_release(MediaCatalogSnapshotV2 *snapshot)
{
    if (snapshot == nullptr) {
        return;
    }
    media_catalog_v2_release(&snapshot->catalog);
    heap_caps_free(snapshot->manifest);
    *snapshot = {};
}

esp_err_t media_catalog_store_v2_load(MediaCatalogSnapshotV2 *snapshot)
{
    if (snapshot == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    media_catalog_store_v2_release(snapshot);

    StorageSdLockGuard sd_lock;
    if (!sd_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }

    struct Candidate
    {
        const char *index_path;
        const char *manifest_path;
        MediaCatalogLoadSourceV2 source;
        const char *name;
    };
    const Candidate candidates[] = {
        {SystemPaths::kMusicIndexV2Temp, SystemPaths::kMusicManifestV2Temp, MediaCatalogLoadSourceV2::Temp, "tmp"},
        {SystemPaths::kMusicIndexV2, SystemPaths::kMusicManifestV2, MediaCatalogLoadSourceV2::Final, "final"},
        {SystemPaths::kMusicIndexV2Backup, SystemPaths::kMusicManifestV2Backup, MediaCatalogLoadSourceV2::Backup, "bak"},
    };

    esp_err_t last_error = ESP_ERR_NOT_FOUND;
    for (const Candidate &candidate : candidates) {
        MediaCatalogSnapshotV2 loaded = {};
        const esp_err_t ret = load_pair(
            candidate.index_path, candidate.manifest_path, candidate.source, &loaded
        );
        if (ret == ESP_OK) {
            *snapshot = loaded;
            CATALOG_STORE_BOOT_LOGI("已加载 V2 Catalog：来源=%s tracks=%lu artists=%lu albums=%lu artist_refs=%lu lyrics_refs=%lu artwork_refs=%lu strings=%luB CRC=0x%08lX",
                candidate.name,
                static_cast<unsigned long>(snapshot->catalog.track_count),
                static_cast<unsigned long>(snapshot->catalog.artist_count),
                static_cast<unsigned long>(snapshot->catalog.album_count),
                static_cast<unsigned long>(snapshot->catalog.track_artist_ref_count),
                static_cast<unsigned long>(snapshot->catalog.lyrics_ref_count),
                static_cast<unsigned long>(snapshot->catalog.artwork_ref_count),
                static_cast<unsigned long>(snapshot->catalog.pool.size),
                static_cast<unsigned long>(snapshot->index_crc32));
            return ESP_OK;
        }
        if (ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "V2 候选无效：%s index=%s ret=%s",
                candidate.name, candidate.index_path, esp_err_to_name(ret));
            if (candidate.source == MediaCatalogLoadSourceV2::Temp) {
                // 无效 tmp 只是上次中断事务的残留，清掉避免每次启动重复尝试/告警。
                remove(candidate.index_path);
                remove(candidate.manifest_path);
            }
            last_error = ret;
        }
    }
    return last_error == ESP_ERR_NOT_FOUND ? ESP_ERR_NOT_FOUND : last_error;
}

bool media_catalog_store_v2_find(
    const MediaCatalogSnapshotV2 *snapshot,
    const char *path,
    const TrackRowV2 **out_track,
    const MediaManifestRecordV2 **out_manifest
)
{
    if (out_track != nullptr) {
        *out_track = nullptr;
    }
    if (out_manifest != nullptr) {
        *out_manifest = nullptr;
    }
    if (snapshot == nullptr || path == nullptr || snapshot->catalog.track_count == 0 ||
        snapshot->catalog.tracks == nullptr || snapshot->manifest == nullptr ||
        snapshot->manifest_count != snapshot->catalog.track_count) {
        return false;
    }

    uint32_t low = 0;
    uint32_t high = snapshot->catalog.track_count;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2;
        const TrackRowV2 &track = snapshot->catalog.tracks[mid];
        const char *candidate = media_catalog_v2_pool_str(&snapshot->catalog, track.path_off);
        if (candidate == nullptr) {
            return false;
        }
        const int comparison = strcasecmp(path, candidate);
        if (comparison == 0) {
            if (out_track != nullptr) {
                *out_track = &track;
            }
            if (out_manifest != nullptr) {
                *out_manifest = &snapshot->manifest[mid];
            }
            return true;
        }
        if (comparison < 0) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return false;
}

static esp_err_t rotate_atomic_file(const char *temp_path, const char *final_path, const char *backup_path)
{
    remove(backup_path);
    if (rename(final_path, backup_path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "旧文件转 bak 失败：%s errno=%d", final_path, errno);
    }
    if (rename(temp_path, final_path) != 0) {
        ESP_LOGE(TAG, "tmp 提升 final 失败：%s -> %s errno=%d", temp_path, final_path, errno);
        rename(backup_path, final_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t media_catalog_store_v2_commit(
    const MusicCatalogV2 *catalog,
    const MediaIndexRecord *source_records,
    size_t source_record_count,
    const MediaCatalogSnapshotV2 *previous_snapshot,
    uint32_t *out_index_crc32
)
{
    if (catalog == nullptr || source_record_count != catalog->track_count ||
        (source_record_count > 0 && source_records == nullptr)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t semantic_ret = media_catalog_v2_validate(catalog);
    if (semantic_ret != ESP_OK) {
        return semantic_ret;
    }

    CatalogCommitScratchV2 *scratch = static_cast<CatalogCommitScratchV2 *>(
        heap_caps_calloc(1, sizeof(CatalogCommitScratchV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (scratch == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t file_size = 0;
    uint32_t index_crc = 0;
    esp_err_t ret = build_section_table(catalog, scratch->sections, &file_size, &index_crc);
    if (ret != ESP_OK) {
        heap_caps_free(scratch);
        return ret;
    }
    const uint32_t manifest_crc = calculate_manifest_crc(source_records, source_record_count);
    if (out_index_crc32 != nullptr) {
        *out_index_crc32 = index_crc;
    }

    // Catalog build/CRC 全部在锁外完成；从目录检查开始才进入全局 SD 事务。
    StorageSdLockGuard sd_lock;
    if (!sd_lock.locked()) {
        heap_caps_free(scratch);
        return ESP_ERR_TIMEOUT;
    }
    if (!ensure_library_directory()) {
        heap_caps_free(scratch);
        return ESP_FAIL;
    }

    if (previous_snapshot != nullptr &&
        previous_snapshot->source == MediaCatalogLoadSourceV2::Final &&
        previous_snapshot->index_crc32 == index_crc &&
        previous_snapshot->manifest_crc32 == manifest_crc) {
        ESP_LOGI(TAG, "V2 Catalog 内容未变化，跳过写盘：CRC=0x%08lX",
            static_cast<unsigned long>(index_crc));
        heap_caps_free(scratch);
        return ESP_OK;
    }

    remove(SystemPaths::kMusicIndexV2Temp);
    remove(SystemPaths::kMusicManifestV2Temp);
    ret = write_catalog_file(
        SystemPaths::kMusicIndexV2Temp, catalog, scratch->sections, file_size, index_crc
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "写入 V2 Catalog tmp 失败：%s", esp_err_to_name(ret));
        heap_caps_free(scratch);
        return ret;
    }
    uint32_t written_manifest_crc = 0;
    ret = write_manifest_file(
        SystemPaths::kMusicManifestV2Temp,
        source_records,
        source_record_count,
        index_crc,
        &written_manifest_crc
    );
    if (ret != ESP_OK || written_manifest_crc != manifest_crc) {
        remove(SystemPaths::kMusicIndexV2Temp);
        remove(SystemPaths::kMusicManifestV2Temp);
        heap_caps_free(scratch);
        return ret == ESP_OK ? ESP_FAIL : ret;
    }

    ret = load_pair(
        SystemPaths::kMusicIndexV2Temp,
        SystemPaths::kMusicManifestV2Temp,
        MediaCatalogLoadSourceV2::Temp,
        &scratch->verify
    );
    media_catalog_store_v2_release(&scratch->verify);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "V2 tmp 成对校验失败：%s", esp_err_to_name(ret));
        remove(SystemPaths::kMusicIndexV2Temp);
        remove(SystemPaths::kMusicManifestV2Temp);
        heap_caps_free(scratch);
        return ret;
    }

    ret = rotate_atomic_file(
        SystemPaths::kMusicIndexV2Temp,
        SystemPaths::kMusicIndexV2,
        SystemPaths::kMusicIndexV2Backup
    );
    if (ret != ESP_OK) {
        remove(SystemPaths::kMusicManifestV2Temp);
        heap_caps_free(scratch);
        return ret;
    }
    ret = rotate_atomic_file(
        SystemPaths::kMusicManifestV2Temp,
        SystemPaths::kMusicManifestV2,
        SystemPaths::kMusicManifestV2Backup
    );
    if (ret != ESP_OK) {
        remove(SystemPaths::kMusicIndexV2);
        rename(SystemPaths::kMusicIndexV2Backup, SystemPaths::kMusicIndexV2);
        heap_caps_free(scratch);
        return ret;
    }

    ret = load_pair(
        SystemPaths::kMusicIndexV2,
        SystemPaths::kMusicManifestV2,
        MediaCatalogLoadSourceV2::Final,
        &scratch->verify
    );
    if (ret != ESP_OK || scratch->verify.index_crc32 != index_crc ||
        scratch->verify.manifest_crc32 != manifest_crc) {
        ESP_LOGE(TAG, "V2 final 二次校验失败，回滚 bak：%s", esp_err_to_name(ret));
        media_catalog_store_v2_release(&scratch->verify);
        remove(SystemPaths::kMusicIndexV2);
        remove(SystemPaths::kMusicManifestV2);
        rename(SystemPaths::kMusicIndexV2Backup, SystemPaths::kMusicIndexV2);
        rename(SystemPaths::kMusicManifestV2Backup, SystemPaths::kMusicManifestV2);
        heap_caps_free(scratch);
        return ESP_FAIL;
    }
    media_catalog_store_v2_release(&scratch->verify);

    heap_caps_free(scratch);

    ESP_LOGI(TAG, "V2 Catalog 事务提交完成：tracks=%lu artists=%lu albums=%lu artist_refs=%lu lyrics_refs=%lu artwork_refs=%lu strings=%luB index_crc=0x%08lX manifest_crc=0x%08lX",
        static_cast<unsigned long>(catalog->track_count),
        static_cast<unsigned long>(catalog->artist_count),
        static_cast<unsigned long>(catalog->album_count),
        static_cast<unsigned long>(catalog->track_artist_ref_count),
        static_cast<unsigned long>(catalog->lyrics_ref_count),
        static_cast<unsigned long>(catalog->artwork_ref_count),
        static_cast<unsigned long>(catalog->pool.size),
        static_cast<unsigned long>(index_crc),
        static_cast<unsigned long>(manifest_crc));
    return ESP_OK;
}
