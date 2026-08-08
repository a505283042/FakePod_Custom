#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "media_library.h"

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
    uint64_t position_frames = 0;
    uint64_t total_frames = 0;
};

const char *audio_playback_state_name_cn(AudioPlaybackState state);
