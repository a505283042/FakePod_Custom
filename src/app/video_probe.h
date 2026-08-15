#pragma once

#include <stdint.h>

#include "esp_err.h"

namespace VideoProbe
{

enum class State : uint8_t
{
    Idle = 0,
    Running,
    Ready,
    Failed,
};

struct Snapshot
{
    State state = State::Idle;
    uint32_t generation = 0;
    esp_err_t result = ESP_OK;
    uint32_t file_size = 0;
    uint32_t duration_ms = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t fps = 0;
    uint32_t video_bitrate = 0;
    uint32_t audio_bitrate = 0;
    uint32_t audio_sample_rate = 0;
    uint8_t audio_channels = 0;
    bool has_video = false;
    bool has_audio = false;
    bool video_is_mjpeg = false;
    bool audio_is_mp3 = false;
    bool seek_probe_ok = false;
};

// Probe/注册保持 R.40.1 行为；R.40.2 Benchmark 复用同一 AVI extractor 与共享 Storage IO。
esp_err_t init();

// 异步解析 AVI 头与首路音/视频 metadata。Task 完成后自动退出。
// 当前阶段不读取/解码 MJPEG frame，也不把 MP3 送入 AudioTask。
esp_err_t start(const char *path);
void cancel();
bool get_snapshot(Snapshot *out_snapshot);
const char *state_name(State state);

} // namespace VideoProbe
