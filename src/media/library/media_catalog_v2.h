#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "media_types.h"

static constexpr uint32_t MEDIA_CATALOG_INVALID_ID_V2 = UINT32_MAX;

struct MediaEntityTrackGroupV2;
struct MediaDecadeTrackGroupV2;

// Track metadata 有效位。数值为 0 只表示默认存储值，是否真的存在该标签必须看对应 flag。
enum MediaTrackMetadataFlagsV2 : uint32_t
{
    MEDIA_TRACK_META_NONE_V2 = 0,
    MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2 = 1U << 0,
    MEDIA_TRACK_META_HAS_TRACK_TOTAL_V2 = 1U << 1,
    MEDIA_TRACK_META_HAS_DISC_NUMBER_V2 = 1U << 2,
    MEDIA_TRACK_META_HAS_DISC_TOTAL_V2 = 1U << 3,
    MEDIA_TRACK_META_HAS_RELEASE_YEAR_V2 = 1U << 4,
    MEDIA_TRACK_META_HAS_ORIGINAL_YEAR_V2 = 1U << 5,
    // Stage 10.3：该文件已由当前 Metadata Scanner 完整处理；即使没有任何标签也会置位。
    MEDIA_TRACK_META_SCANNED_V2 = 1U << 6,
    MEDIA_TRACK_META_HAS_TITLE_TAG_V2 = 1U << 7,
    MEDIA_TRACK_META_HAS_ARTIST_TAG_V2 = 1U << 8,
    MEDIA_TRACK_META_HAS_ALBUM_TAG_V2 = 1U << 9,
    MEDIA_TRACK_META_HAS_ALBUM_ARTIST_TAG_V2 = 1U << 10,
};

// Album 年份由后续 metadata/group 构建阶段归纳。mixed 与单一有效年份互斥。
enum MediaAlbumFlagsV2 : uint32_t
{
    MEDIA_ALBUM_FLAG_NONE_V2 = 0,
    MEDIA_ALBUM_FLAG_HAS_RELEASE_YEAR_V2 = 1U << 0,
    MEDIA_ALBUM_FLAG_HAS_ORIGINAL_YEAR_V2 = 1U << 1,
    MEDIA_ALBUM_FLAG_MIXED_RELEASE_YEARS_V2 = 1U << 2,
    MEDIA_ALBUM_FLAG_MIXED_ORIGINAL_YEARS_V2 = 1U << 3,
};

// 多歌手关系独立放在连续关系池中。当前阶段只冻结 flags 空间，10.3 不根据字符串猜角色。
enum MediaTrackArtistRefFlagsV2 : uint32_t
{
    MEDIA_TRACK_ARTIST_REF_NONE_V2 = 0,
};

enum MediaLyricsRefFlagsV2 : uint32_t
{
    MEDIA_LYRICS_REF_NONE_V2 = 0,
    // ID3v2 某些标签启用 unsynchronisation；offset 仍指向原始文件，读取时需要反转义。
    MEDIA_LYRICS_REF_NEEDS_ID3_UNSYNC_V2 = 1U << 0,
    // data_offset/data_size 指向完整 USLT/SYLT frame payload（从 encoding byte 开始），
    // LyricsTask 读取后再解析 description/timestamp payload，避免扫描期复制歌词正文。
    MEDIA_LYRICS_REF_ID3_FRAME_PAYLOAD_V2 = 1U << 1,
};

enum class MediaLyricsSourceV2 : uint8_t
{
    None = 0,
    ExternalFile = 1,
    EmbeddedTag = 2,
};

enum class MediaLyricsKindV2 : uint8_t
{
    Unknown = 0,
    Unsynced = 1,
    Synced = 2,
};

enum class MediaLyricsEncodingV2 : uint8_t
{
    Unknown = 0,
    Utf8 = 1,
    Utf16Le = 2,
    Utf16Be = 3,
    Latin1 = 4,
};

// Stage 10.2 运行时 Catalog：大数据区全部优先放 PSRAM，Row 只保存偏移/ID/POD 技术字段。
struct StringPoolV2
{
    char *data = nullptr;
    uint32_t size = 0;
};

struct ArtistRowV2
{
    uint32_t name_off = 0;
    uint32_t flags = 0;
};

// Track 与 Artist 是多对多关系；数组中的物理顺序就是标签中的展示顺序。
struct TrackArtistRefV2
{
    uint32_t artist_id = MEDIA_CATALOG_INVALID_ID_V2;
    uint32_t flags = MEDIA_TRACK_ARTIST_REF_NONE_V2;
};

// 歌词也采用关系池：一首歌可以同时保留外置 LRC、USLT/SYLT 或多语言歌词。
// ExternalFile 使用 path_off；EmbeddedTag 使用 data_offset/data_size。language_off 为可选语言字符串。
struct LyricsRefV2
{
    uint64_t data_offset = 0;
    uint32_t data_size = 0;
    uint32_t path_off = 0;
    uint32_t language_off = 0;
    uint32_t flags = MEDIA_LYRICS_REF_NONE_V2;
    MediaLyricsSourceV2 source = MediaLyricsSourceV2::None;
    MediaLyricsKindV2 kind = MediaLyricsKindV2::Unknown;
    MediaLyricsEncodingV2 encoding = MediaLyricsEncodingV2::Unknown;
    uint8_t reserved0 = 0;
};

// 每首歌最多选择一个首选封面 locator。
// MP3/FLAC Embedded 使用音频文件 + 物理 data_offset/data_size；OpusPicture 的 data_offset
// 保存 OpusTags comment 序号、data_size 保存解码后的 JPEG/PNG 字节数；ExternalFile 使用 path_off。
struct ArtworkRefV2
{
    uint64_t data_offset = 0;
    uint32_t data_size = 0;
    uint32_t path_off = 0;
    int64_t source_modified_time = 0;
    uint32_t flags = MEDIA_ARTWORK_REF_NONE_V2;
    uint16_t width = 0;
    uint16_t height = 0;
    MediaArtworkSourceV2 source = MediaArtworkSourceV2::None;
    MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
    uint8_t picture_type = 0;
    uint8_t reserved0 = 0;
};

struct AlbumRowV2
{
    uint32_t title_off = 0;
    // Album Artist 与 Track Artist 分离，避免 OST/Various Artists 被拆成多个同名专辑。
    uint32_t display_artist_off = 0;
    uint32_t album_artist_id = MEDIA_CATALOG_INVALID_ID_V2;
    uint32_t artwork_track_id = MEDIA_CATALOG_INVALID_ID_V2;
    uint32_t flags = MEDIA_ALBUM_FLAG_NONE_V2;
    uint16_t release_year = 0;
    uint16_t original_year = 0;
};

struct TrackRowV2
{
    uint32_t path_off = 0;
    uint32_t title_off = 0;
    // 原始显示字符串与实体关系分离：UI 可原样显示“A & B feat. C”，分组使用 artist_refs。
    uint32_t display_artist_off = 0;
    uint32_t artist_ref_start = 0;
    uint16_t artist_ref_count = 0;
    uint16_t reserved0 = 0;

    // 同一首歌可有多个歌词来源，关系池顺序由后续策略决定优先级。
    uint32_t lyrics_ref_start = 0;
    uint16_t lyrics_ref_count = 0;
    uint16_t reserved1 = 0;

    uint32_t album_id = MEDIA_CATALOG_INVALID_ID_V2;
    uint32_t artwork_ref_id = MEDIA_CATALOG_INVALID_ID_V2;

    // Stage 10.2.2 冻结专辑排序/年代播放所需的基础 metadata schema。
    uint32_t metadata_flags = MEDIA_TRACK_META_NONE_V2;
    uint16_t track_number = 0;
    uint16_t track_total = 0;
    uint16_t disc_number = 0;
    uint16_t disc_total = 0;
    uint16_t release_year = 0;
    uint16_t original_year = 0;

    uint64_t file_size_bytes = 0;
    MediaFormat format = MediaFormat::Unknown;
    MediaTechnicalInfo technical = {};
};

struct MusicCatalogV2
{
    StringPoolV2 pool = {};
    TrackRowV2 *tracks = nullptr;
    ArtistRowV2 *artists = nullptr;
    AlbumRowV2 *albums = nullptr;
    TrackArtistRefV2 *track_artist_refs = nullptr;
    LyricsRefV2 *lyrics_refs = nullptr;
    ArtworkRefV2 *artwork_refs = nullptr;
    uint32_t track_count = 0;
    uint32_t artist_count = 0;
    uint32_t album_count = 0;
    uint32_t track_artist_ref_count = 0;
    uint32_t lyrics_ref_count = 0;
    uint32_t artwork_ref_count = 0;
    uint32_t generation = 0;
    uint32_t source_crc32 = 0;

    // Stage 10.4：以下全部是运行时派生索引，不进入 V2 磁盘格式。
    MediaEntityTrackGroupV2 *artist_groups = nullptr;
    MediaEntityTrackGroupV2 *album_groups = nullptr;
    MediaDecadeTrackGroupV2 *decade_groups = nullptr;
    uint32_t *artist_group_track_pool = nullptr;
    uint32_t *album_group_track_pool = nullptr;
    uint32_t *decade_group_track_pool = nullptr;
    uint32_t artist_group_count = 0;
    uint32_t album_group_count = 0;
    uint32_t decade_group_count = 0;
    uint32_t artist_group_track_count = 0;
    uint32_t album_group_track_count = 0;
    uint32_t decade_group_track_count = 0;
};

struct MediaTrackViewV2
{
    uint32_t generation = 0;
    uint32_t track_index = MEDIA_CATALOG_INVALID_ID_V2;
    const TrackRowV2 *row = nullptr;
    const char *path = nullptr;
    const char *title = nullptr;
    const char *artist = nullptr; // display_artist_off 的原始显示字符串。
    const char *album = nullptr;
};

struct MediaArtworkViewV2
{
    uint32_t generation = 0;
    uint32_t track_index = MEDIA_CATALOG_INVALID_ID_V2;
    const ArtworkRefV2 *ref = nullptr;
    const char *external_path = nullptr;
};

struct MediaIndexRecord;

// 从扫描阶段的临时记录构建精确尺寸的 PSRAM Catalog。Stage 10.2 先使用文件名作为 title fallback；
// Artist/Album Row 的正式 metadata 填充留给下一阶段，但磁盘/运行时结构从本阶段固定下来。
esp_err_t media_catalog_v2_build_from_index_records(
    const MediaIndexRecord *records,
    size_t record_count,
    const char *path_pool,
    size_t path_pool_size,
    MusicCatalogV2 *out_catalog
);

// 完整释放一个尚未发布或加载出来的 Catalog。
void media_catalog_v2_release(MusicCatalogV2 *catalog);

// 做运行时语义校验：字符串偏移、ID、技术 offset、路径排序等都必须合法。
esp_err_t media_catalog_v2_validate(const MusicCatalogV2 *catalog);

// 将构建/加载好的 Catalog 发布为本次启动周期的运行时只读目录，并生成 generation。
// 启动路径仍只允许首次发布；运行期替换必须走 quiesced replace API。
esp_err_t media_catalog_v2_publish(MusicCatalogV2 *catalog, uint32_t source_crc32);

// 在所有 Catalog 裸指针消费者已停用/隔离时，事务式替换当前运行时 Catalog。
// 成功后旧 Catalog 的所有权移动到 out_retired，调用方必须在完成 Playlist/UI generation
// 重绑定后调用 media_catalog_v2_release(out_retired)。失败时当前 Catalog 完全不变。
esp_err_t media_catalog_v2_replace_quiesced(
    MusicCatalogV2 *catalog,
    uint32_t source_crc32,
    MusicCatalogV2 *out_retired);

bool media_catalog_v2_ready();
const MusicCatalogV2 *media_catalog_v2_current();
uint32_t media_catalog_v2_generation();

// 获取只读 Track View。返回的指针只在对应 generation 未变化期间有效。
bool media_catalog_v2_get_track_view(size_t index, MediaTrackViewV2 *out_view);

// 安全访问字符串池。offset 非法时返回 nullptr。
const char *media_catalog_v2_pool_str(const MusicCatalogV2 *catalog, uint32_t offset);

// 从 TrackRow 复制稳定 POD 技术快照，供 Player/AudioTask 使用。
bool media_catalog_v2_copy_technical(size_t index, MediaTechnicalInfo *out_info);

// 获取当前首选封面 locator。没有封面返回 false；返回指针仅在 generation 未变化期间有效。
bool media_catalog_v2_get_artwork_view(size_t index, MediaArtworkViewV2 *out_view);
