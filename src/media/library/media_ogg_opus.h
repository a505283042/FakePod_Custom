#pragma once

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "media_types.h"

// Ogg Opus 的文本 comment 回调。comment 为 UTF-8 的完整 KEY=VALUE，
// 仅对长度不超过扫描文本上限的 comment 调用；大型封面字段会直接跳过，不占用 PSRAM。
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
// METADATA_BLOCK_PICTURE 等大型字段本轮只跳过；内嵌封面留到后续独立功能实现。
esp_err_t media_ogg_opus_visit_text_comments(
    FILE *file,
    uint64_t file_size,
    MediaOggOpusCommentCallback callback,
    void *context
);
