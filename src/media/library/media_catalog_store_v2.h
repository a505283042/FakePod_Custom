#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "media_catalog_v2.h"

struct MediaIndexRecord;

enum class MediaCatalogLoadSourceV2 : uint8_t
{
    None = 0,
    Temp,
    Final,
    Backup,
};

struct MediaManifestRecordV2
{
    uint32_t track_index = 0;
    MediaFormat format = MediaFormat::Unknown;
    uint64_t file_size_bytes = 0;
    int64_t modified_time = 0;
};

struct MediaCatalogSnapshotV2
{
    MusicCatalogV2 catalog = {};
    MediaManifestRecordV2 *manifest = nullptr;
    uint32_t manifest_count = 0;
    uint32_t index_crc32 = 0;
    uint32_t manifest_crc32 = 0;
    MediaCatalogLoadSourceV2 source = MediaCatalogLoadSourceV2::None;
};

// 按 tmp -> final -> bak 顺序尝试加载成对的 V2 Catalog/Manifest；任一候选都必须通过
// header/section/CRC/semantic/manifest linkage 全部校验后才能返回。
esp_err_t media_catalog_store_v2_load(MediaCatalogSnapshotV2 *snapshot);
// R46.0.70：流式校验任意 V2 Catalog/Manifest 文件对，不展开 MusicCatalogV2，
// 因而大 NAS 索引同步时只需要一个 4KB PSRAM scratch，不会临时复制整份 Catalog。
esp_err_t media_catalog_store_v2_validate_pair_files(
    const char *index_path,
    const char *manifest_path,
    uint32_t *out_track_count,
    uint32_t *out_index_crc32,
    uint32_t *out_manifest_crc32);

// R46.0.71：NAS 浏览只需 Catalog，Manifest 只用于链接校验，不常驻 PSRAM。
esp_err_t media_catalog_store_v2_load_catalog_only(
    const char *index_path,
    const char *manifest_path,
    MusicCatalogV2 *out_catalog,
    uint32_t *out_index_crc32);
// R46.0.74：NAS 后台浏览加载专用。索引读取每约4KB主动释放一次全局SD锁并阻塞1 tick，
// 让高优先级音频预取任务可以穿插补充 ring；只完整校验 Catalog，本次浏览不展开 Manifest。
esp_err_t media_catalog_store_v2_load_catalog_only_cooperative(
    const char *index_path,
    const char *manifest_path,
    MusicCatalogV2 *out_catalog,
    uint32_t *out_index_crc32);
void media_catalog_store_v2_release(MediaCatalogSnapshotV2 *snapshot);

// V2 Track 按完整 UTF-8 路径排序，可二分查找并取到对应 Manifest 签名。
bool media_catalog_store_v2_find(
    const MediaCatalogSnapshotV2 *snapshot,
    const char *path,
    const TrackRowV2 **out_track,
    const MediaManifestRecordV2 **out_manifest
);

// 原子提交 /System/library 下的 V2 Catalog + Manifest。内容未变化且来源为 final 时跳过写盘。
esp_err_t media_catalog_store_v2_commit(
    const MusicCatalogV2 *catalog,
    const MediaIndexRecord *source_records,
    size_t source_record_count,
    const MediaCatalogSnapshotV2 *previous_snapshot,
    uint32_t *out_index_crc32
);
