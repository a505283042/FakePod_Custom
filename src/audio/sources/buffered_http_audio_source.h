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
    uint64_t range_offset_bytes = 0ULL;
    uint64_t file_total_bytes = 0ULL;
    bool range_response_verified = false;
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
    FlacSeekProbe, // NAS FLAC no-SEEKTABLE Range probe: 384KB ring, 128KB initial prime.
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

// R145 网络层预备：从指定绝对文件偏移开始的新连接；非零位置必须收到
// 206 + 与偏移匹配的 Content-Range。当前不改变原有播放和 SEEK 路径。
esp_err_t buffered_http_audio_source_open_range(
    AudioSource *out_source,
    BufferedHttpAudioProfile profile,
    const char *url,
    const char *username,
    const char *password,
    uint64_t start_offset,
    BufferedHttpAudioAbortFn abort_fn = nullptr,
    const void *abort_context = nullptr
);

// R145.2: 仅 AudioTask 串行调用，且不得在 decoder 正在 read 时调用。
// 先建立并验证新 Range 流；连接失败时保留旧源及其缓冲。
// 成功后替换整个 AudioSource，旧 Ring/Worker 被安全关闭；不混用两代字节。
esp_err_t buffered_http_audio_source_reopen_range(
    AudioSource *source,
    uint64_t start_offset,
    BufferedHttpAudioAbortFn abort_fn = nullptr,
    const void *abort_context = nullptr
);

// Install an already-read byte suffix ahead of the same HTTP ring. Ownership of the
// PSRAM buffer transfers on success; reads then continue from the same Range connection.
esp_err_t buffered_http_audio_source_attach_replay(
    AudioSource *source, uint8_t *owned_buffer, size_t replay_bytes, uint64_t absolute_offset);

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

// R145.4: 在单个 Range 窗口里验证相邻 MP3 帧头，返回新打开、对齐的流。
// 不估算播放时钟，也不会替换旧 Source；调用者负责原流生命周期。
esp_err_t buffered_http_audio_source_open_mp3_aligned_range(
    AudioSource *out_source, const char *url, const char *username, const char *password,
    uint64_t estimate_offset, uint32_t expected_sample_rate_hz, uint64_t *out_frame_offset,
    BufferedHttpAudioAbortFn abort_fn = nullptr, const void *abort_context = nullptr);
