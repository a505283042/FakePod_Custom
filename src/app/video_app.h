#pragma once

#include "esp_err.h"

// R.40.2 Video：/sdcard/VIDEO AVI Virtual Browser + 460x460 MJPEG Decode/CO5300 Benchmark。
// 当前阶段只提取/解码 VIDEO 轨；AVI 内 MP3 暂不播放，AudioTask 不改。
esp_err_t video_app_register();
