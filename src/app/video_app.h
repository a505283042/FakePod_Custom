#pragma once

#include <stdint.h>

#include "esp_err.h"

enum class VideoRemoteCommand : uint8_t
{
    TogglePlayPause = 0,
    Next,
    Previous,
};

struct VideoRemoteSnapshot
{
    bool active = false;
    bool playing = false;
    bool paused = false;
    uint32_t item_index = UINT32_MAX;
    uint32_t position_ms = 0U;
    uint32_t duration_ms = 0U;
    uint32_t playback_revision = 0U;
    uint32_t metadata_revision = 0U;
    char title[128] = {};
};

// BLE/外部遥控只提交意图；真正的Video状态切换仍由Video LVGL timer串行执行。
bool video_app_remote_control_active();
bool video_app_remote_submit(VideoRemoteCommand command);
bool video_app_get_remote_snapshot(VideoRemoteSnapshot *out_snapshot);

// R.40.2 Video：/sdcard/video AVI Virtual Browser + 460x460 MJPEG Decode/CO5300 Benchmark。
esp_err_t video_app_register();
