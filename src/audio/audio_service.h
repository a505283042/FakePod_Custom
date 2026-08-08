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

// 当前播放世代。播放新曲或停止时递增，用于后续取消过期异步操作。
uint32_t audio_service_playback_revision();
