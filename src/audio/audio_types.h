#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "media_types.h"

// 音频任务对外发布的播放状态。
enum class AudioPlaybackState : uint8_t
{
    Starting = 0,
    Ready,
    Preparing,
    Prepared,
    Playing,
    Seeking,
    Paused,
    Finished,
    Stopped,
    Error
};

// Error 只区分“当前曲目自身不可播放”和“系统音频链故障”。
// Player 仅允许自动跳过 Track；System 必须停住，避免 DAC/I2S/内存故障时连续扫完整个队列。
enum class AudioFailureScope : uint8_t
{
    None = 0,
    Track,
    System,
};

// 只读快照只包含 POD 数据，可安全跨任务复制。
// 高频进度由 AudioTask 每约250ms发布一次，UI不直接访问解码器/文件/I2S。
struct AudioStateSnapshot
{
    bool ready = false;
    AudioPlaybackState state = AudioPlaybackState::Starting;
    uint32_t state_revision = 0;
    uint32_t playback_revision = 1;
    uint32_t last_request_id = 0;
    uint32_t track_index = UINT32_MAX;
    MediaFormat format = MediaFormat::Unknown;
    esp_err_t last_error = ESP_OK;
    AudioFailureScope failure_scope = AudioFailureScope::None;
    uint32_t queue_depth = 0;

    uint32_t sample_rate_hz = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    // position_frames 的正式语义是“已成功提交给 I2S DMA 的真实 PCM 帧”，
    // 不包含首块预解码、启动/暂停/EOF 的静音帧。decoder_position_frames 仅用于诊断前后级差异。
    uint64_t position_frames = 0;
    uint64_t decoder_position_frames = 0;
    uint64_t position_ms = 0;
    uint64_t total_frames = 0;

    // Seek 只由 AudioTask 串行执行。seek_supported 表示当前文件已开放任意位置 seek；
    // FLAC 只有存在有效 SEEKTABLE 时才为 true。seek_revision 每次成功定位递增。
    bool seek_supported = false;
    uint32_t seek_revision = 0;
    uint32_t last_seek_request_id = 0;
    uint64_t last_seek_target_ms = 0;

    // 共享 decoder workspace 只发布容量快照，便于确认 MP3/FLAC 没有重复持有大块 PSRAM。
    uint32_t decode_workspace_input_bytes = 0;
    uint32_t decode_workspace_pcm_bytes = 0;

    // 用户音量由 AudioTask 持有并发布；0~100 与 DAC 实际寄存器映射解耦。
    uint8_t volume_percent = 50;
    bool user_muted = false;
};

// R.36.2.2：偶发音频停止的持久 RAM 快照。
// 仅在 AudioTask 真实进入错误状态前写入一次；不依赖高频诊断开关。
enum class AudioFaultStage : uint8_t
{
    None = 0,
    ReadPcm,
    PcmNoProgress,
    FirstPcmUnmute,
    I2sWrite,
    PauseMute,
    ResumePlayback,
    EofDrain,
    Unknown,
};

struct AudioFaultSnapshot
{
    bool valid = false;
    uint32_t sequence = 0;
    uint32_t fault_count = 0;
    AudioFaultStage stage = AudioFaultStage::None;
    esp_err_t error = ESP_OK;

    uint32_t playback_revision = 0;
    uint32_t track_index = UINT32_MAX;
    MediaFormat format = MediaFormat::Unknown;
    uint32_t sample_rate_hz = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint64_t position_frames = 0;
    uint64_t decoder_position_frames = 0;

    // FLAC 专用预取现场；非 FLAC 故障时 flac_prefetch_active=false。
    bool flac_prefetch_active = false;
    bool flac_prefetch_io_error = false;
    bool flac_prefetch_eof = false;
    bool flac_prefetch_pressure = false;
    uint8_t flac_qos_level = 2;  // 0=Emergency,1=Recovery,2=Normal,3=Plenty
    uint32_t flac_ring_buffered_bytes = 0;
    uint32_t flac_ring_capacity_bytes = 0;
    uint32_t flac_ring_min_buffered_bytes = 0;
    uint32_t flac_emergency_entries = 0;
    uint32_t flac_recovered_count = 0;
    uint32_t flac_max_consecutive_reads = 0;

    // 故障发生瞬间的 heap 现场。
    uint32_t internal_free_bytes = 0;
    uint32_t internal_min_bytes = 0;
    uint32_t internal_largest_bytes = 0;
    uint32_t dma_free_bytes = 0;
    uint32_t psram_free_bytes = 0;
};

// P1.5.2R.3：AudioTask 只旁路抽取真实 PCM；低优先级 SpectrumFFT 任务
// 计算 256 点 FFT 并发布 16 个低频→高频 band。UI 只消费 POD 快照，
// 不接触 decoder、PCM 工作区或 I2S。
static constexpr uint8_t AUDIO_SPECTRUM_BAND_COUNT = 16U;

struct AudioSpectrumSnapshot
{
    bool valid = false;
    uint32_t revision = 0;
    uint32_t playback_revision = 0;
    uint32_t track_index = UINT32_MAX;
    uint32_t sample_rate_hz = 0;
    uint32_t analysis_sample_rate_hz = 0;
    uint16_t fft_size = 0;
    uint64_t position_frames = 0;
    uint8_t levels[AUDIO_SPECTRUM_BAND_COUNT] = {};
};

const char *audio_playback_state_name_cn(AudioPlaybackState state);
