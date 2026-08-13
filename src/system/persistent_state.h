#pragma once

#include <stdint.h>

#include "esp_err.h"

// NVS V1 只保存稳定身份：音量、播放模式、当前歌曲路径与播放列表身份。
// 不保存播放进度、亮度、播放/暂停状态，也不保存任何运行时 index/generation。

enum PersistentDirtyBits : uint32_t
{
    PERSISTENT_DIRTY_NONE = 0U,
    PERSISTENT_DIRTY_AUDIO = 1U << 0,
    PERSISTENT_DIRTY_RESUME = 1U << 1,
};

struct PersistentStateStatus
{
    bool ready = false;
    bool schema_valid = false;
    bool has_volume = false;
    bool has_loop_mode = false;
    bool has_resume = false;
    uint32_t dirty_bits = PERSISTENT_DIRTY_NONE;
};

// 初始化默认 NVS 分区并加载 V1 快照。不会主动擦除 NVS，也不会写 Flash。
esp_err_t persistent_state_init();

// AudioTask 启动后应用已保存音量。没有有效保存值时保持 AudioTask 默认值。
bool persistent_state_restore_audio();

// Catalog/Player 建立后恢复播放模式、歌曲与稳定 Playlist Context。
// 只恢复选择状态，不自动播放；歌曲始终从 00:00 开始。
bool persistent_state_restore_player();

// READY 后观察当前运行态，只更新 RAM 快照并标 dirty；绝不调用 nvs_set/nvs_commit。
void persistent_state_observe_runtime();

// 显式写入当前 dirty 快照。PowerKey V1 在关机长按确认后调用；其余自动写入策略仍未启用。
esp_err_t persistent_state_flush();

bool persistent_state_get_status(PersistentStateStatus *out_status);
