#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "media_catalog_v2.h"

// Stage 10.3 格式无关的扫描期 metadata 中间结构。
// 这里允许使用按需 PSRAM 字符串/数组；Catalog Builder 完成后立即释放，不进入 AudioTask 热路径。
struct MediaMetadataLyricsBuildV2
{
    char *path = nullptr;
    char *language = nullptr;
    uint64_t data_offset = 0;
    uint32_t data_size = 0;
    uint32_t flags = MEDIA_LYRICS_REF_NONE_V2;
    MediaLyricsSourceV2 source = MediaLyricsSourceV2::None;
    MediaLyricsKindV2 kind = MediaLyricsKindV2::Unknown;
    MediaLyricsEncodingV2 encoding = MediaLyricsEncodingV2::Unknown;
};

struct MediaMetadataBuildV2
{
    char *title = nullptr;
    char *display_artist = nullptr;
    char *album = nullptr;
    char *album_artist = nullptr;

    char **artists = nullptr;
    uint16_t artist_count = 0;
    uint16_t artist_capacity = 0;

    MediaMetadataLyricsBuildV2 *lyrics = nullptr;
    uint16_t lyrics_count = 0;
    uint16_t lyrics_capacity = 0;

    uint32_t metadata_flags = MEDIA_TRACK_META_NONE_V2;
    uint16_t track_number = 0;
    uint16_t track_total = 0;
    uint16_t disc_number = 0;
    uint16_t disc_total = 0;
    uint16_t release_year = 0;
    uint16_t original_year = 0;
};

void media_metadata_build_release(MediaMetadataBuildV2 *metadata);

// 扫描当前已接入的压缩格式 metadata：
// - FLAC: Vorbis Comment + 外置同名 .lrc
// - MP3: ID3v2.2/v2.3/v2.4（并兼容 ID3v1 fallback）+ USLT/SYLT + 外置同名 .lrc
// 不读取歌词正文；内嵌歌词仅记录文件 offset/size/编码/语言。
esp_err_t media_metadata_scan_file_v2(
    const char *path,
    MediaFormat format,
    uint64_t file_size,
    MediaMetadataBuildV2 *out_metadata
);

// 复用上层已打开的音频文件，避免首次建库对同一路径重复 fopen。
// 调用方负责 TF 锁与 FILE* 生命周期；外置 .lrc 仍按 audio_path 探测。
esp_err_t media_metadata_scan_open_file_v2(
    FILE *file,
    const char *audio_path,
    MediaFormat format,
    uint64_t file_size,
    MediaMetadataBuildV2 *out_metadata
);

// 首次建库已经按目录解析出同名歌词时使用。external_lrc_path=nullptr 表示已确认无外置歌词，
// 因此不会再对每首歌执行 stat("同名.lrc")。
esp_err_t media_metadata_scan_open_file_indexed_v2(
    FILE *file,
    const char *audio_path,
    const char *external_lrc_path,
    MediaFormat format,
    uint64_t file_size,
    MediaMetadataBuildV2 *out_metadata
);

// 增量重建时，从已校验的旧 Catalog 克隆一首歌的小型 metadata 到扫描期中间结构。
// 歌词正文不会复制，只复制 LyricsRef 的定位信息。
esp_err_t media_metadata_clone_from_catalog_v2(
    const MusicCatalogV2 *catalog,
    uint32_t track_index,
    MediaMetadataBuildV2 *out_metadata
);
