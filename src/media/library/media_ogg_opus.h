#pragma once

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "media_types.h"

// Ogg Opus 的文本 comment 回调。comment 为 UTF-8 的完整 KEY=VALUE，
// 仅对长度不超过扫描文本上限的 comment 调用；大型封面字段会直接跳过，不占用 PSRAM。

struct MediaOggOpusPictureInfo
{
    uint32_t comment_index = 0U;
    uint32_t data_size = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
    uint8_t picture_type = 0U;
};

// ArtworkTask 可提供分片 I/O 回调，让 OggTags/Base64 解码沿用现有 SD 抢占策略。
// callback 必须精确完成 bytes 个字节的顺序读取/跳过；传 nullptr 时使用普通 fread/fseek。
using MediaOggOpusIoCallback = esp_err_t (*)(
    FILE *file,
    void *buffer,
    uint32_t bytes,
    bool skip,
    void *context
);
using MediaOggOpusCommentCallback = esp_err_t (*)(
    const char *comment,
    uint32_t length,
    void *context
);

// 解析 OpusHead 与尾部 EOS granule，得到固定 48kHz 播放时钟下的总 PCM 帧数/时长。
// 当前只支持常见的单 logical stream Ogg Opus；不做 chained stream 合并。
esp_err_t media_ogg_opus_probe(
    FILE *file,
    uint64_t file_size,
    MediaTechnicalInfo *out_info
);

// 读取第二个 Ogg packet（OpusTags）中的文本 comment。
// 大型 METADATA_BLOCK_PICTURE 由独立流式接口处理，文本扫描仍不会分配大块 PSRAM。
esp_err_t media_ogg_opus_visit_text_comments(
    FILE *file,
    uint64_t file_size,
    MediaOggOpusCommentCallback callback,
    void *context
);

// 流式寻找 OpusTags 中的 METADATA_BLOCK_PICTURE。优先 Front Cover(type=3)，
// 否则使用首个有效 JPEG/PNG；不保存图片正文。
esp_err_t media_ogg_opus_find_picture(
    FILE *file,
    uint64_t file_size,
    MediaOggOpusPictureInfo *out_picture
);

// 按 comment_index 重新定位并流式 Base64 解码内嵌图片到调用方缓冲。
// 使用 io_callback 时要求 FILE* 当前位于文件起点（fopen 后的默认位置）。
esp_err_t media_ogg_opus_read_picture(
    FILE *file,
    uint64_t file_size,
    uint32_t comment_index,
    uint8_t *out_data,
    uint32_t out_capacity,
    MediaOggOpusPictureInfo *out_picture,
    MediaOggOpusIoCallback io_callback = nullptr,
    void *io_context = nullptr
);
