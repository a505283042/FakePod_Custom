#pragma once

#include <stdint.h>

#include "esp_err.h"

enum class NsfRemoteCommand : uint8_t
{
    TogglePlayPause = 0,
    Next,
    Previous,
};

struct NsfRemoteSnapshot
{
    bool active = false;
    bool playing = false;
    bool paused = false;
    uint32_t track_index = UINT32_MAX;
    uint32_t position_ms = 0U;
    uint32_t duration_ms = 0U;
    uint32_t playback_revision = 0U;
    uint32_t metadata_revision = 0U;
    char title[128] = {};
    char artist[96] = {};
};

// BLE/外部遥控只提交意图；真正的 NSF 状态切换仍由电子音流 LVGL timer 串行执行。
bool visual_music_app_remote_control_active();
bool visual_music_app_remote_submit(NsfRemoteCommand command);
bool visual_music_app_get_remote_snapshot(NsfRemoteSnapshot *out_snapshot);

// 注册第2个 Launcher 槽位的“电子音流” APP。
// 电子音流沿用 Video/Ebook 生命周期与文件浏览；当前接入 NSF 纯瀑布与 6502/2A03 音频合成。
esp_err_t visual_music_app_register();
