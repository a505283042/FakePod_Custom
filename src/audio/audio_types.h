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
    Paused,
    Finished,
    Stopped,
    Error
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

    // 共享 decoder workspace 只发布容量快照，便于确认 MP3/FLAC 没有重复持有大块 PSRAM。
    uint32_t decode_workspace_input_bytes = 0;
    uint32_t decode_workspace_pcm_bytes = 0;

    // 用户音量由 AudioTask 持有并发布；0~100 与 DAC 实际寄存器映射解耦。
    uint8_t volume_percent = 80;
    bool user_muted = false;
};

const char *audio_playback_state_name_cn(AudioPlaybackState state);
