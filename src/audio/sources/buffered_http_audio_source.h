#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_source.h"

struct BufferedHttpAudioSourceStats
{
    uint32_t ring_capacity_bytes = 0U;
    uint32_t ring_buffered_bytes = 0U;
    uint32_t ring_min_buffered_bytes = 0U;
    uint64_t network_bytes = 0ULL;
    uint32_t task_stack_hwm = 0U;
    bool eof = false;
    bool io_error = false;
    esp_err_t io_error_code = ESP_OK;
};

// R46.0.78：NAS MP3 第一阶段网络 Source。
// HTTP client 只由 Core1 预读任务持有；AudioTask 热路径只从 128KB PSRAM ring 取压缩字节。
// 当前仅支持顺序播放，不开放 Range/Seek。HTTPS 仍未启用。
esp_err_t buffered_http_audio_source_open(
    AudioSource *out_source,
    const char *url,
    const char *username,
    const char *password
);

bool buffered_http_audio_source_get_stats(
    const AudioSource *source,
    BufferedHttpAudioSourceStats *out_stats
);
