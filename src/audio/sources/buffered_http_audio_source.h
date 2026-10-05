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

using BufferedHttpAudioAbortFn = bool (*)(const void *context);

enum class BufferedHttpAudioProfile : uint8_t
{
    Mp3 = 0,
    Flac,
};

// R46.0.85：NAS HTTP Source 改为统一字节流 Worker。
// Worker 只负责 connect/retry/read -> PSRAM ring，不解析 MP3/FLAC；Codec 继续只消费 AudioSource。
// R46.0.98：MP3/FLAC 共用同一个 NasStream task；MP3保持128KB/96KB，NAS FLAC固定384KB/256KB，
// 不再依赖建链前不可用/不可靠的采样率信息，不增加第二个网络任务。
esp_err_t buffered_http_audio_source_open(
    AudioSource *out_source,
    BufferedHttpAudioProfile profile,
    const char *url,
    const char *username,
    const char *password,
    BufferedHttpAudioAbortFn abort_fn = nullptr,
    const void *abort_context = nullptr
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
