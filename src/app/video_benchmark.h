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

// 启动一个一次性 MJPEG benchmark task。只抽取 VIDEO track，不读取/解码 AVI 内 MP3。
// RGB565 输出固定为 CO5300 wire-order (big-endian)，供 BoundedSPI 直接提交。
esp_err_t start(const char *path);
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
