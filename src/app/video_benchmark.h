#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace VideoBenchmark
{

static constexpr uint16_t kWidth = 460U;   // CO5300 canvas / maximum video width
static constexpr uint16_t kHeight = 460U;  // CO5300 canvas / maximum video height
static constexpr size_t kRgb565Bytes = static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 2U;

enum class State : uint8_t
{
    Idle = 0,
    Starting,
    Running,
    Eof,
    Stopped,
    Failed,
};

struct Snapshot
{
    State state = State::Idle;
    uint32_t generation = 0U;
    esp_err_t result = ESP_OK;
    uint32_t duration_ms = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint16_t fps_hint = 0U;

    // R.40.4.1 AVI MP3 Audio Pipeline V1：AUDIO 压缩帧送入 PSRAM Bridge，由 AudioTask 解码并输出 PCM。
    uint16_t audio_streams = 0U;
    uint32_t audio_format = 0U;
    uint32_t audio_duration_ms = 0U;
    uint32_t audio_bitrate = 0U;
    uint32_t audio_sample_rate = 0U;
    uint8_t audio_channels = 0U;
    uint8_t audio_bits_per_sample = 0U;
    uint32_t audio_frames_read = 0U;
    uint64_t audio_compressed_bytes = 0ULL;
    uint32_t audio_compressed_bytes_max = 0U;
    uint32_t audio_first_pts_ms = 0U;
    uint32_t audio_last_pts_ms = 0U;
    uint64_t audio_extract_us_total = 0ULL;
    uint32_t audio_extract_us_max = 0U;
    uint64_t audio_storage_us_total = 0ULL;
    uint32_t audio_storage_us_max = 0U;

    uint32_t frames_read = 0U;
    uint32_t frames_decoded = 0U;
    uint32_t frames_pool_skipped = 0U;
    uint32_t decode_failures = 0U;
    uint64_t compressed_bytes = 0ULL;
    uint32_t compressed_bytes_max = 0U;

    uint64_t extract_us_total = 0ULL;
    uint32_t extract_us_max = 0U;
    uint64_t storage_us_total = 0ULL;
    uint32_t storage_us_max = 0U;
    uint64_t decode_us_total = 0ULL;
    uint32_t decode_us_max = 0U;
    uint64_t storage_gate_wait_us = 0ULL;
    uint32_t storage_gate_wait_count = 0U;

    uint32_t first_pts_ms = 0U;
    uint32_t last_pts_ms = 0U;
};

struct FrameView
{
    const uint8_t *rgb565_be = nullptr;
    size_t bytes = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint32_t pts_ms = 0U;
    uint32_t compressed_bytes = 0U;
    uint32_t extract_us = 0U;
    uint32_t storage_us = 0U;
    uint32_t decode_us = 0U;
    uint8_t slot = 0xFFU;
};

// 启动 AVI A/V pipeline：VIDEO 进入 MJPEG pipeline；MP3 AUDIO 经 PSRAM Bridge 送 AudioTask 解码/输出。
// RGB565 输出固定为 CO5300 wire-order (big-endian)，供 BoundedSPI 直接提交。
// allow_fullcanvas_over20=false 时，460x460 且 >20FPS 只做风险探测并返回 ESP_ERR_NOT_SUPPORTED；
// UI 提示用户后可用 true 再启动一次。运行期不做 24->20 自动降帧。
esp_err_t start(const char *path, bool allow_fullcanvas_over20 = false);
void stop();

bool get_snapshot(Snapshot *out_snapshot);
// UI 在取得第一张帧时发布唯一 Presentation Clock；DecodeTask 只消费该时钟，不再自行锚定。
bool set_presentation_clock(uint32_t first_pts_ms, int64_t clock_base_us);
bool take_frame(FrameView *out_frame);
// Presenter 专用阻塞式取帧；直接等待 RGB ready queue，避免 LVGL timer 轮询引入几十毫秒空洞。
bool wait_frame(FrameView *out_frame, uint32_t timeout_ms);
void release_frame(uint8_t slot);

// 仅在 task 已退出且没有 UI 持有 frame 时释放双 RGB565 PSRAM/queue。
esp_err_t cleanup();
const char *state_name(State state);

} // namespace VideoBenchmark
