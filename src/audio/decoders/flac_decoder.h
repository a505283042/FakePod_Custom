#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#include "audio_diag_config.h"

// FLAC 解码器只负责“压缩数据 -> PCM”，不拥有 I2S、DAC 或播放状态。
// 当前已进入 44.1~192kHz 受控实机验证；高采样率是否长期保留由实时预算和缓存水位决定。
struct FlacDecoder
{
    FILE *file = nullptr;
    void *simple_handle = nullptr;
    // 压缩流预取层只拥有 SD 读取和 PSRAM 环形缓冲，不拥有 FLAC 解码器状态。
    void *prefetch_context = nullptr;

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
    uint64_t flac_offset_bytes = 0;
    uint16_t max_block_size = 0;
    uint32_t min_frame_size = 0;
    uint32_t max_frame_size = 0;

    uint32_t sample_rate_hz = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint64_t total_frames = 0;
    uint64_t frames_read = 0;

#if APP_DIAG_FLAC_PERFORMANCE
    // 仅保存少量累计计数，用于定位 SD 读取或 FLAC process 是否超过 I2S DMA 安全窗口。
    uint64_t perf_decode_total_us = 0;
    uint64_t perf_refill_total_us = 0;
    uint64_t perf_last_report_us = 0;
    uint32_t perf_decode_calls = 0;
    uint32_t perf_refill_calls = 0;
    uint32_t perf_decode_max_us = 0;
    uint32_t perf_refill_max_us = 0;
    uint32_t perf_decode_over_20ms = 0;
    uint32_t perf_decode_over_40ms = 0;
    uint32_t perf_refill_over_20ms = 0;
    uint32_t perf_refill_over_30ms = 0;
    uint32_t perf_refill_over_block_budget = 0;
    uint32_t perf_refill_worst_over_budget_us = 0;
    uint64_t perf_refill_process_total = 0;
    uint32_t perf_refill_process_max = 0;
    uint32_t perf_refill_multi_process = 0;
    uint32_t perf_refill_input_fill_max = 0;
    uint32_t perf_refill_multi_fill = 0;
    // 输入窗口搬运统计只用于验证高采样率下的低拷贝策略，不参与播放控制。
    uint32_t perf_input_compact_calls = 0;
    uint64_t perf_input_compact_bytes = 0;
    uint64_t perf_input_topup_bytes = 0;
    uint32_t perf_input_topup_calls = 0;
    uint32_t perf_input_topup_max_bytes = 0;
    uint64_t perf_prefetch_wait_total_us = 0;
    uint32_t perf_prefetch_wait_calls = 0;
    uint32_t perf_prefetch_wait_max_us = 0;
    uint32_t perf_prefetch_wait_over_2ms = 0;
    uint32_t perf_prefetch_starve_count = 0;
    size_t perf_prefetch_min_buffered_bytes = 0;

#endif

    // 首块 PCM 在 I2S 启动前预解码，此阶段没有实时播放截止时间；
    // 若 64KB 起播预充被较大的 FLAC 元数据耗尽，可允许等待后台预取继续补充。
    bool startup_predecode_active = false;
    bool runtime_info_verified = false;
    bool eof = false;
};

#if APP_DIAG_FLAC_PERFORMANCE
// FLAC 性能统计通过只读快照跨任务发布。AudioTask 只更新 POD 数值，
// 长日志由系统 loopTask 输出，避免串口格式化阻塞实时解码路径。
struct FlacPerfSnapshot
{
    uint32_t sequence = 0;
    bool active = false;
    int32_t prefetch_core_id = -1;
    uint32_t sample_rate_hz = 0;

    uint32_t read_avg_us = 0;
    uint32_t read_max_us = 0;
    uint32_t read_over_10ms = 0;
    uint32_t read_calls = 0;

    uint32_t ring_buffered_bytes = 0;
    uint32_t ring_capacity_bytes = 0;
    uint32_t ring_min_buffered_bytes = 0;
    uint32_t prefetch_copy_avg_us = 0;
    uint32_t prefetch_copy_max_us = 0;
    uint32_t prefetch_copy_over_2ms = 0;
    uint32_t prefetch_starve_count = 0;
    uint32_t prefetch_stack_hwm = 0;

    uint32_t decode_avg_us = 0;
    uint32_t decode_max_us = 0;
    uint32_t decode_over_20ms = 0;
    uint32_t decode_over_40ms = 0;
    uint32_t decode_calls = 0;

    uint32_t refill_avg_us = 0;
    uint32_t refill_max_us = 0;
    uint32_t refill_over_20ms = 0;
    uint32_t refill_over_30ms = 0;
    uint32_t refill_over_block_budget = 0;
    uint32_t refill_worst_over_budget_us = 0;
    int32_t refill_peak_margin_us = 0;
    uint32_t refill_calls = 0;
    uint32_t process_per_refill_x100 = 0;
    uint32_t process_per_refill_max = 0;
    uint32_t multi_process_refills = 0;
    uint32_t input_fills_per_refill_max = 0;
    uint32_t multi_fill_refills = 0;
    uint32_t input_compact_calls = 0;
    uint32_t input_compact_total_bytes = 0;
    uint32_t input_topup_avg_bytes = 0;
    uint32_t input_topup_max_bytes = 0;

    uint32_t block_budget_us = 0;
    uint32_t refill_avg_load_percent = 0;
    uint32_t refill_peak_load_percent = 0;
};

// 获取最近一次由 AudioTask 发布的性能快照。
bool flac_decoder_get_perf_snapshot(FlacPerfSnapshot *out_snapshot);

#endif

// AudioTask 启动时调用一次，只注册 FLAC 后端，不注册无关编解码器。
esp_err_t flac_decoder_register_backend();

// 打开 FLAC 并读取 STREAMINFO。支持标准 fLaC，也兼容前置 ID3v2 标签。
// 当前 PCM sink 支持：44.1/48/88.2/96/176.4/192kHz、单/双声道、16/24/32bit。
esp_err_t flac_decoder_open(FlacDecoder *decoder, const char *path);

// 按需流式解码并统一转换成 32bit I2S 立体声容器。
// 单声道会复制到左右声道；16/24bit 会左对齐到 32bit。
esp_err_t flac_decoder_read_pcm32(
    FlacDecoder *decoder,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames
);

void flac_decoder_close(FlacDecoder *decoder);
bool flac_decoder_is_open(const FlacDecoder *decoder);
bool flac_decoder_is_eof(const FlacDecoder *decoder);
