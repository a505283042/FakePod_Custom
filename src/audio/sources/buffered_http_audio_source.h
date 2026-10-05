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

enum class BufferedHttpAudioProfile : uint8_t
{
    Mp3 = 0,
};

// R46.0.85：NAS HTTP Source 改为统一字节流 Worker。
// Worker 只负责 connect/retry/read -> PSRAM ring，不解析 MP3/FLAC；Codec 继续只消费 AudioSource。
// 当前只启用 MP3 profile，后续 NAS FLAC 复用同一个 Worker，不再创建第二套 HTTP task。
esp_err_t buffered_http_audio_source_open(
    AudioSource *out_source,
    BufferedHttpAudioProfile profile,
    const char *url,
    const char *username,
    const char *password
);

bool buffered_http_audio_source_get_stats(
    const AudioSource *source,
    BufferedHttpAudioSourceStats *out_stats
);

// 仅供 AudioTask 在 Deep Suspend 关闭 Source 前复制恢复端点；返回指针只在 Source 打开期间有效。
bool buffered_http_audio_source_get_endpoint_view(
    const AudioSource *source,
    const char **out_url,
    const char **out_username,
    const char **out_password
);
