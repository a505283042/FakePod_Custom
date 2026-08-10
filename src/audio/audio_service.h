#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "audio_types.h"

// 启动唯一的 AudioTask。运行期所有 DAC/I2S 控制都必须由该任务执行。
esp_err_t audio_service_start();

// 判断 AudioTask 是否已经完成硬件初始化。
bool audio_service_is_ready();

// 获取 AudioTask 发布的只读状态快照。
bool audio_service_get_snapshot(AudioStateSnapshot *out_snapshot);

// P1.5.2R.3：获取低优先级 SpectrumFFT 任务发布的 16 路真实频率 band。
// UI 只能读取快照，不能直接访问 PCM/decoder/I2S。
bool audio_service_get_spectrum_snapshot(AudioSpectrumSnapshot *out_snapshot);

// 频谱页面显示/隐藏时启停旁路分析。这里只控制观察者，不改变音频 pipeline。
void audio_service_set_spectrum_enabled(bool enabled);

// 请求播放指定曲目。路径和技术索引都会复制到请求对象，避免跨任务悬空指针。
// Stage 10.1 只传递/校验索引快照，decoder 仍以实际文件解析结果作为播放真值。
bool audio_service_play_track(
    uint32_t track_index,
    const char *path,
    MediaFormat format,
    const MediaTechnicalInfo *technical_info,
    bool wait = false
);

// 停止、暂停和恢复均通过 AudioTask 串行执行。
bool audio_service_stop(bool wait = true);
bool audio_service_pause(bool wait = true);
bool audio_service_resume(bool wait = true);

// Stage 11.x：对当前选中 Track 发起 Seek。Play/Seek 共用 latest-intent 单槽；
// 路径/技术索引仍复制进请求，AudioTask 会核对 track + playback_revision，并丢弃被更新意图覆盖的旧请求。
bool audio_service_seek_track(
    uint32_t track_index,
    const char *path,
    MediaFormat format,
    const MediaTechnicalInfo *technical_info,
    uint64_t target_ms,
    bool wait = false
);

// 用户音量/静音同样只能通过 AudioTask 命令队列改变。
// percent 范围 0~100；默认 80 保持 Stage 9.x 已实机验证的 -20dB 起始音量。
bool audio_service_set_volume(uint8_t percent, bool wait = false);
bool audio_service_set_mute(bool mute, bool wait = false);

// 当前播放世代。播放新曲或停止时递增，用于后续取消过期异步操作。
uint32_t audio_service_playback_revision();
