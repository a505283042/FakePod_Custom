#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "audio_types.h"

// P1.5.2R.3：启动低优先级 SpectrumFFT 任务。任务常态阻塞，只有频谱页启用且
// AudioTask 提交了新的 256 点分析窗时才被唤醒；失败不会改变 AudioTask ownership。
esp_err_t audio_spectrum_snapshot_start();
bool audio_spectrum_snapshot_is_ready();

// 频谱页显示时启用捕获；隐藏时关闭。这里只改变旁路分析开关，不控制 decoder/I2S。
void audio_spectrum_snapshot_set_enabled(bool enabled);

// 仅 AudioTask 调用 reset/publish；其它任务只能通过 audio_service_get_spectrum_snapshot() 读取。
void audio_spectrum_snapshot_reset(
    uint32_t playback_revision,
    uint32_t track_index,
    uint32_t sample_rate_hz);

// PCM 必须是 AudioTask 已经成功提交给 I2S 的 32bit stereo block。
// 所有格式统一约24Hz抽取，略高于20FPS频谱UI，保留调度余量并降低后台FFT负载。
// AudioTask 只负责抽取/降采样并填充小型 256 点 mono 窗，不在实时热路径执行 FFT。
void audio_spectrum_snapshot_publish_pcm(
    const int32_t *interleaved_stereo,
    size_t frames,
    uint32_t playback_revision,
    uint32_t track_index,
    uint32_t sample_rate_hz,
    uint64_t submitted_frames);

bool audio_spectrum_snapshot_get(AudioSpectrumSnapshot *out_snapshot);
