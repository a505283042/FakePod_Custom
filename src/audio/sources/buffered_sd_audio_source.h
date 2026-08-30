#pragma once

#include <stdint.h>

#include "audio_source.h"

// MP3/WAV 进入连续播放阶段后使用的 SD 顺序预读 Source。
// 打开、格式解析和 Seek 仍使用同步 SD Source；这些操作完成后再切换到本 Source，
// 使 AudioTask 的正常播放热路径只从 PSRAM 环形缓冲取数据，不直接等待 FATFS。
//
// 从指定绝对文件偏移开始建立顺序预读。
// 本 Source 只支持连续 READ/SIZE/EOF；需要 Seek 时应关闭播放管线并重新建立 Source。
esp_err_t buffered_sd_audio_source_open(
    AudioSource *out_source,
    const char *path,
    uint64_t start_offset
);
