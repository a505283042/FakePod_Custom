#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// P1.4.1 先消费 Catalog 中已扫描出的 External Synced LRC。
// 歌词正文由低优先级 LyricsTask 从 TF 卡增量读取并解析；LVGL 只读取轻量窗口快照。
enum class LyricsLoadState : uint8_t
{
    Idle = 0,
    Loading,
    Ready,
    NoLyrics,
    Unsupported,
    Failed,
};

static constexpr size_t LYRICS_VIEW_WINDOW_LINES = 5U;
static constexpr size_t LYRICS_VIEW_TEXT_BYTES = 192U;

struct LyricsWindowLine
{
    bool valid = false;
    bool current = false;
    uint32_t time_ms = 0;
    char text[LYRICS_VIEW_TEXT_BYTES] = {};
};

struct LyricsWindowSnapshot
{
    LyricsLoadState state = LyricsLoadState::Idle;
    esp_err_t result = ESP_OK;
    uint32_t revision = 0;
    uint32_t catalog_generation = 0;
    uint32_t track_index = UINT32_MAX;
    uint32_t line_count = 0;
    uint32_t current_line_index = UINT32_MAX;
    LyricsWindowLine lines[LYRICS_VIEW_WINDOW_LINES] = {};
};

// 启动低优先级 LyricsTask。任务平时阻塞等待请求，不参与 AudioTask 热路径。
esp_err_t lyrics_service_start();
bool lyrics_service_is_ready();

// USB MSC 热切换前阻止新歌词读取，并等待已经打开的 LRC 文件安全 fclose。
bool lyrics_service_prepare_storage_handoff(TickType_t timeout_ticks);
void lyrics_service_resume_storage_after_handoff();

// 提交当前 Track 的歌词加载请求。只保留最新请求；迟到结果不会覆盖新 Track。
bool lyrics_service_request_track(uint32_t track_index);

// 根据 AudioStateSnapshot.position_ms 获取当前行前后各两行的只读副本。
bool lyrics_service_get_window(uint32_t track_index, uint64_t position_ms, LyricsWindowSnapshot *out_snapshot);

const char *lyrics_load_state_name_cn(LyricsLoadState state);
