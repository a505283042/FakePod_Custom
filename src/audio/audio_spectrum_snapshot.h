#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "audio_types.h"

// 低优先级 SpectrumFFT 任务由频谱页按需创建；离开后进入约2秒 Grace 复用窗口，
// 快速返回频谱页直接复用，超时后任务自行退出并释放 Internal 栈。
// start() 保留给 set_enabled(true) 使用；失败不会改变 AudioTask ownership。
esp_err_t audio_spectrum_snapshot_start();
bool audio_spectrum_snapshot_is_ready();

// 频谱页显示时创建/复用任务并启用捕获；隐藏时立即关闭旁路并进入 Grace，
// UI 不等待 Task 回收；不控制 decoder/I2S。
void audio_spectrum_snapshot_set_enabled(bool enabled);

// 仅 AudioTask 调用 reset/publish；其它任务只能通过 audio_service_get_spectrum_snapshot() 读取。
void audio_spectrum_snapshot_reset(
    uint32_t playback_revision,
    uint32_t track_index,
    uint32_t sample_rate_hz);

// PCM 必须是 AudioTask 已经成功提交给 I2S 的 32bit stereo block。
// 本地普通采样率约24Hz抽取、>=96kHz降到12Hz；NAS_HTTP >=96kHz由Hi-Res Guard直接禁用FFT。
// AudioTask 只负责抽取/降采样并填充小型 256 点 mono 窗，不在实时热路径执行 FFT。
void audio_spectrum_snapshot_publish_pcm(
    const int32_t *interleaved_stereo,
    size_t frames,
    AudioPlaybackSource source,
    uint32_t playback_revision,
    uint32_t track_index,
    uint32_t sample_rate_hz,
    uint64_t submitted_frames);

bool audio_spectrum_snapshot_get(AudioSpectrumSnapshot *out_snapshot);
