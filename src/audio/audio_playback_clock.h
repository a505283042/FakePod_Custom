#pragma once

#include <stddef.h>
#include <stdint.h>

// 播放时钟只统计“真实 PCM 已成功提交到 I2S DMA”的帧。
// decoder 可以提前准备/消费数据，但不会因此提前推进 UI/歌词/恢复使用的时间轴。
struct AudioPlaybackClock
{
    uint32_t sample_rate_hz = 0;
    uint64_t submitted_frames = 0;
    uint64_t decoder_frames = 0;
};

static inline void audio_playback_clock_reset(
    AudioPlaybackClock *clock,
    uint32_t sample_rate_hz = 0)
{
    if (clock == nullptr) {
        return;
    }
    clock->sample_rate_hz = sample_rate_hz;
    clock->submitted_frames = 0;
    clock->decoder_frames = 0;
}

static inline void audio_playback_clock_note_decoder(
    AudioPlaybackClock *clock,
    uint64_t decoder_frames)
{
    if (clock != nullptr) {
        clock->decoder_frames = decoder_frames;
    }
}

static inline void audio_playback_clock_commit_pcm(
    AudioPlaybackClock *clock,
    size_t frames)
{
    if (clock != nullptr) {
        clock->submitted_frames += static_cast<uint64_t>(frames);
    }
}

static inline uint64_t audio_playback_clock_position_ms(const AudioPlaybackClock *clock)
{
    if (clock == nullptr || clock->sample_rate_hz == 0) {
        return 0;
    }
    return (clock->submitted_frames * 1000ULL) / clock->sample_rate_hz;
}
