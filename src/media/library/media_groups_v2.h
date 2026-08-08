#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

struct MusicCatalogV2;

// Artist / Album 分组都使用连续 track-index pool。group 本身只保存实体 ID 与 pool span。
struct MediaEntityTrackGroupV2
{
    uint32_t entity_id = UINT32_MAX;
    uint32_t track_index_start = 0;
    uint32_t track_count = 0;
};

enum MediaDecadeGroupFlagsV2 : uint32_t
{
    MEDIA_DECADE_GROUP_NONE_V2 = 0,
    MEDIA_DECADE_GROUP_UNKNOWN_V2 = 1U << 0,
};

// decade_start=2010 表示 2010-2019；未知年代使用 flag 标记且 decade_start=0。
struct MediaDecadeTrackGroupV2
{
    uint32_t track_index_start = 0;
    uint32_t track_count = 0;
    uint32_t flags = MEDIA_DECADE_GROUP_NONE_V2;
    uint16_t decade_start = 0;
    uint16_t reserved0 = 0;
};

struct MediaArtistGroupViewV2
{
    uint32_t generation = 0;
    uint32_t artist_id = UINT32_MAX;
    const char *name = nullptr;
    const uint32_t *track_indices = nullptr;
    uint32_t track_count = 0;
};

struct MediaAlbumGroupViewV2
{
    uint32_t generation = 0;
    uint32_t album_id = UINT32_MAX;
    const char *title = nullptr;
    const char *artist = nullptr;
    const uint32_t *track_indices = nullptr;
    uint32_t track_count = 0;
    uint32_t flags = 0;
    uint16_t release_year = 0;
    uint16_t original_year = 0;
};

struct MediaDecadeGroupViewV2
{
    uint32_t generation = 0;
    uint16_t decade_start = 0;
    bool unknown = false;
    const uint32_t *track_indices = nullptr;
    uint32_t track_count = 0;
};

// Stage 10.4：根据已校验 Catalog Row 构建纯运行时分组；不写入 V2 磁盘文件。
esp_err_t media_groups_v2_build(MusicCatalogV2 *catalog);
void media_groups_v2_release(MusicCatalogV2 *catalog);
esp_err_t media_groups_v2_validate(const MusicCatalogV2 *catalog);
size_t media_groups_v2_psram_bytes(const MusicCatalogV2 *catalog);

size_t media_groups_v2_artist_count();
size_t media_groups_v2_album_count();
size_t media_groups_v2_decade_count();

bool media_groups_v2_get_artist(size_t group_index, MediaArtistGroupViewV2 *out_view);
bool media_groups_v2_get_album(size_t group_index, MediaAlbumGroupViewV2 *out_view);
bool media_groups_v2_get_decade(size_t group_index, MediaDecadeGroupViewV2 *out_view);
