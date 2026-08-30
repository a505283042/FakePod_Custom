#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "audio/midi_synth.h"

namespace VisualMusicMidi
{

enum class LoadState : uint8_t
{
    Idle = 0,
    Ready,
    Failed,
};

// 瀑布与 AudioTask Synth 共用同一紧凑事件类型，避免解析完成后再次转换/复制字段。
using NoteEvent = MidiSynthNoteEvent;

struct Timeline
{
    NoteEvent *notes = nullptr; // PSRAM，按 start_ms 排序
    size_t note_count = 0U;
    uint32_t duration_ms = 0U;
    uint32_t max_note_duration_ms = 0U;
    uint16_t division = 0U;
    uint16_t track_count = 0U;
    uint8_t min_note = 0U;
    uint8_t max_note = 0U;
    uint8_t format = 0U;
};

struct LoadResult
{
    LoadState state = LoadState::Idle;
    esp_err_t result = ESP_OK;
    uint32_t generation = 0U;
    Timeline timeline = {};
};

// 初始化一次结果队列。解析任务采用 generation 取消，不强删任务。
esp_err_t init();

// 异步加载并解析 SMF Type 0/1；SMPTE division 与 Type 2 暂不支持。
esp_err_t start(const char *path);
void cancel();

// UI 线程取得解析结果并接管 Timeline 所有权；没有结果时返回 false。
bool take_result(LoadResult *out_result);

void release_timeline(Timeline *timeline);

} // namespace VisualMusicMidi
