#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_diag_config.h"
#include "../audio_decode_workspace.h"
#include "../sources/audio_source.h"
#include "media_types.h"


enum class Mp3SeekMethod : uint8_t
{
    None = 0,
    XingToc,
    Vbri,
    CbrLinear,
    VbrLinearFallback,
};

// MP3 解码器只负责“压缩数据 -> PCM”，不拥有 I2S、DAC 或播放状态。
// 当前先开放常见本地 MP3：44.1/48kHz、单/双声道，输出统一转换为 32bit 立体声容器。
struct Mp3Decoder
{
    AudioSource *source = nullptr;
    AudioDecodeWorkspace *workspace = nullptr;
    void *simple_handle = nullptr;

    uint8_t *input_buffer = nullptr;
    size_t input_capacity = 0;
    size_t input_offset = 0;
    size_t input_size = 0;
    bool input_chunk_eos = false;

    uint8_t *decoded_buffer = nullptr;
    size_t decoded_capacity = 0;
    size_t decoded_offset = 0;
    size_t decoded_size = 0;

    uint64_t file_size_bytes = 0;
    uint32_t sample_rate_hz = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t bitrate = 0;
    uint64_t total_frames = 0;
    uint64_t frames_read = 0;

#if APP_DIAG_MP3_PERFORMANCE
    // MP3 专项核查期间才编译这些计时/统计字段；验证完成后可整体从固件移除。
    uint64_t perf_read_total_us = 0;
    uint32_t perf_read_calls = 0;
    uint32_t perf_read_max_us = 0;
    uint32_t perf_read_over_5ms = 0;
    uint32_t perf_read_over_10ms = 0;
    uint32_t perf_input_compact_calls = 0;
    uint64_t perf_input_compact_bytes = 0;
    uint64_t perf_input_topup_bytes = 0;
    uint32_t perf_input_topup_max_bytes = 0;

    uint64_t perf_decode_total_us = 0;
    uint32_t perf_decode_calls = 0;
    uint32_t perf_decode_max_us = 0;
    uint32_t perf_decode_over_5ms = 0;
    uint32_t perf_decode_over_10ms = 0;

    uint64_t perf_refill_total_us = 0;
    uint32_t perf_refill_calls = 0;
    uint32_t perf_refill_max_us = 0;
    uint32_t perf_refill_over_10ms = 0;
    uint32_t perf_refill_over_20ms = 0;
    uint64_t perf_refill_process_total = 0;
    uint32_t perf_refill_process_max = 0;
    uint32_t perf_refill_multi_process = 0;
    uint32_t perf_refill_input_fill_max = 0;
    uint32_t perf_refill_multi_fill = 0;

    uint64_t perf_output_frames_total = 0;
    uint32_t perf_output_frames_min = 0;
    uint32_t perf_output_frames_max = 0;
    uint64_t perf_audio_budget_total_us = 0;
    uint32_t perf_refill_over_budget = 0;
    uint32_t perf_refill_worst_over_budget_us = 0;
    int32_t perf_refill_min_margin_us = 0;
    uint32_t perf_refill_peak_load_percent = 0;
    uint64_t perf_last_report_us = 0;
#endif

    bool runtime_info_verified = false;
    bool eof = false;
};

#if APP_DIAG_MP3_PERFORMANCE
// MP3 性能统计通过 POD 快照跨任务发布；长日志由 loopTask 输出，不阻塞 AudioTask。
struct Mp3PerfSnapshot
{
    uint32_t sequence = 0;
    bool active = false;
    uint32_t sample_rate_hz = 0;
    uint32_t bitrate = 0;

    uint32_t read_avg_us = 0;
    uint32_t read_max_us = 0;
    uint32_t read_over_5ms = 0;
    uint32_t read_over_10ms = 0;
    uint32_t read_calls = 0;
    uint32_t compact_calls = 0;
    uint32_t compact_total_bytes = 0;
    uint32_t topup_avg_bytes = 0;
    uint32_t topup_max_bytes = 0;

    uint32_t decode_avg_us = 0;
    uint32_t decode_max_us = 0;
    uint32_t decode_over_5ms = 0;
    uint32_t decode_over_10ms = 0;
    uint32_t decode_calls = 0;

    uint32_t refill_avg_us = 0;
    uint32_t refill_max_us = 0;
    uint32_t refill_over_10ms = 0;
    uint32_t refill_over_20ms = 0;
    uint32_t refill_calls = 0;
    uint32_t process_per_refill_x100 = 0;
    uint32_t process_per_refill_max = 0;
    uint32_t multi_process_refills = 0;
    uint32_t input_fills_per_refill_max = 0;
    uint32_t multi_fill_refills = 0;

    uint32_t output_frames_avg = 0;
    uint32_t output_frames_min = 0;
    uint32_t output_frames_max = 0;
    uint32_t audio_budget_avg_us = 0;
    uint32_t refill_over_budget = 0;
    uint32_t refill_worst_over_budget_us = 0;
    int32_t refill_min_margin_us = 0;
    uint32_t refill_avg_load_percent = 0;
    uint32_t refill_peak_load_percent = 0;
};

bool mp3_decoder_get_perf_snapshot(Mp3PerfSnapshot *out_snapshot);
#endif

// AudioTask 启动时调用一次，只注册 MP3 解码后端。
esp_err_t mp3_decoder_register_backend();

// 从统一 AudioSource 打开 MP3，并预解码第一块 PCM，以便在启动 I2S/DAC 前确认真实输出格式。
esp_err_t mp3_decoder_open(Mp3Decoder *decoder, AudioSource *source, AudioDecodeWorkspace *workspace = nullptr);

// 流式解码并统一转换为 32bit I2S 立体声容器；单声道自动复制到左右声道。
esp_err_t mp3_decoder_read_pcm32(
    Mp3Decoder *decoder,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames
);

// 利用 Stage 10.x 技术索引定位目标帧；优先 Xing TOC / VBRI，缺失时线性估算，
// 最终总会在 Source 上重新同步到连续两个兼容 MPEG frame 再重建 Simple Decoder。
esp_err_t mp3_decoder_seek_frame(
    Mp3Decoder *decoder,
    uint64_t target_frame,
    const MediaTechnicalInfo *technical_info,
    uint64_t *out_frame,
    uint64_t *out_source_offset,
    Mp3SeekMethod *out_method
);

void mp3_decoder_close(Mp3Decoder *decoder);
bool mp3_decoder_is_open(const Mp3Decoder *decoder);
bool mp3_decoder_is_eof(const Mp3Decoder *decoder);
