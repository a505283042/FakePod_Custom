#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "media_types.h"

struct MediaMetadataBuildV2;
struct MediaArtworkBuildV2;

// Stage 10.0 V1 兼容记录。Stage 10.2 起只用于从旧索引迁移技术信息，正式运行索引改用 MusicCatalogV2。
// 路径字符串单独放入 path pool，避免每条记录固定占用大数组。
struct MediaIndexRecord
{
    uint32_t path_offset = 0;
    MediaFormat format = MediaFormat::Unknown;
    uint64_t file_size_bytes = 0;
    int64_t modified_time = 0;
    MediaTechnicalInfo technical = {};

    // 仅扫描期使用，不参与 V1/V2 磁盘序列化。排序时随记录一起移动，Catalog 构建完成后释放。
    MediaMetadataBuildV2 *metadata_build = nullptr;
    MediaArtworkBuildV2 *artwork_build = nullptr;
};

struct MediaIndexSnapshot
{
    MediaIndexRecord *records = nullptr;
    size_t record_count = 0;
    char *path_pool = nullptr;
    size_t path_pool_size = 0;
    uint32_t payload_crc32 = 0;
    bool loaded_from_backup = false;
};

// 读取并校验上一次索引。主文件损坏/缺失时会尝试 .bak。
esp_err_t media_index_store_load(MediaIndexSnapshot *snapshot);
void media_index_store_release(MediaIndexSnapshot *snapshot);

// 索引按 UTF-8 完整路径排序，因此可二分查找旧记录用于增量复用。
const MediaIndexRecord *media_index_store_find(
    const MediaIndexSnapshot *snapshot,
    const char *path
);

// 将当前内存音乐库写入 /sdcard/System。写入采用 tmp -> 校验 -> bak -> final。
esp_err_t media_index_store_commit(
    const MediaIndexRecord *records,
    size_t record_count,
    const char *path_pool,
    size_t path_pool_size,
    const MediaIndexSnapshot *previous_snapshot
);
