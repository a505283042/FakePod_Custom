#pragma once

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "media_types.h"

struct MusicCatalogV2;

// Stage 12.0 扫描期封面定位结构。只保存“在哪里”，不保存图片正文。
// external_path 仅 ExternalFile 使用，由本结构持有并在 release 时释放。
struct MediaArtworkBuildV2
{
    char *external_path = nullptr;
    uint64_t data_offset = 0;
    uint32_t data_size = 0;
    int64_t source_modified_time = 0;
    uint32_t flags = MEDIA_ARTWORK_REF_NONE_V2;
    uint16_t width = 0;
    uint16_t height = 0;
    MediaArtworkSourceV2 source = MediaArtworkSourceV2::None;
    MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
    uint8_t picture_type = 0;
};

void media_artwork_build_release_v2(MediaArtworkBuildV2 *artwork);

// 每个目录只调用一次。按 cover -> folder -> front，jpg/jpeg -> png 的顺序寻找有效外置图片。
// 返回完整 ExternalFile locator；没有 fallback 也返回 ESP_OK + source=None。
esp_err_t media_artwork_find_directory_fallback_v2(
    const char *directory,
    MediaArtworkBuildV2 *out_fallback
);

// 解析一首歌的封面 locator：
// 1) MP3 APIC / PIC 或 FLAC PICTURE，优先 picture type=3 Front Cover；
// 2) 没有有效内嵌封面时使用 directory_fallback_path；
// 3) 不读取整张图片，仅读取最小头部用于 JPEG/PNG magic 与尺寸识别。
esp_err_t media_artwork_scan_file_v2(
    const char *path,
    MediaFormat format,
    uint64_t file_size,
    const MediaArtworkBuildV2 *directory_fallback,
    MediaArtworkBuildV2 *out_artwork
);

// 复用上层已打开的音频文件，避免首次建库重复打开同一路径。
// 调用方负责 TF 锁与 FILE* 生命周期；函数只读取嵌入封面并复制目录 fallback locator。
esp_err_t media_artwork_scan_open_file_v2(
    FILE *file,
    const char *path,
    MediaFormat format,
    uint64_t file_size,
    const MediaArtworkBuildV2 *directory_fallback,
    MediaArtworkBuildV2 *out_artwork
);

// 增量扫描时从当前 Catalog 克隆 locator。
// Embedded 在音频签名未变时直接复用；External/None 会与当前目录 fallback 比较并按需刷新。
// out_unchanged=true 表示 locator 完全未变，可参与整库“全量命中”快速路径。
// 启动增量快扫只需要判断旧 locator 是否仍与当前目录 fallback 一致，
// 不创建扫描期临时对象，避免无变化曲库为每首歌重复申请/复制 artwork 数据。
esp_err_t media_artwork_catalog_reuse_unchanged_v2(
    const MusicCatalogV2 *catalog,
    uint32_t track_index,
    const MediaArtworkBuildV2 *current_directory_fallback,
    bool *out_unchanged
);

// 只有确认本轮确实需要重建 Catalog 时，才把旧 locator 精确克隆到扫描期对象。
// 这里不重新比较目录 fallback；调用方已经在扫描阶段完成一致性判断。
esp_err_t media_artwork_clone_exact_from_catalog_v2(
    const MusicCatalogV2 *catalog,
    uint32_t track_index,
    MediaArtworkBuildV2 *out_artwork
);

esp_err_t media_artwork_clone_from_catalog_v2(
    const MusicCatalogV2 *catalog,
    uint32_t track_index,
    const MediaArtworkBuildV2 *current_directory_fallback,
    MediaArtworkBuildV2 *out_artwork,
    bool *out_unchanged
);
