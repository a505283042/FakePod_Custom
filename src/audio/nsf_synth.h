#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// VM10：传统 NESM NSF 的 2A03 基础五通道播放配置。
// 第一阶段只选择 NTSC 时序；纯 PAL、扩展音源和 NSFE 由上层明确拒绝。
struct NsfSynthConfig
{
    uint16_t load_address = 0U;
    uint16_t init_address = 0U;
    uint16_t play_address = 0U;
    uint16_t ntsc_speed_us = 0U;
    uint8_t banks[8] = {};
    uint8_t version = 1U;
    uint8_t track_count = 0U;
    uint8_t track = 0U; // 0-based
    uint8_t pal_ntsc_bits = 0U;
    uint8_t expansion_chips = 0U;
    bool enable_loop_detection = false; // 仅后台分析实例开启，实时播放路径不承担循环搜索
    bool enable_visual_capture = false; // Sequencer 采集音符状态，供统一瀑布时间轴使用
    bool enable_event_capture = false; // Sequencer 记录 APU/Bank 写事件；真实 PCM Renderer 不再执行6502
};

struct NsfSynth
{
    void *impl = nullptr;
    uint32_t sample_rate_hz = 0U;
    uint64_t position_frames = 0ULL;
    uint8_t track = 0U;
    uint8_t track_count = 0U;
    bool open = false;
    bool failed = false;
};

struct NsfSynthLoopInfo
{
    bool detected = false;
    uint64_t start_frame = 0ULL;
    uint64_t length_frames = 0ULL;
    bool hint_available = false; // 两个完整结构周期一致时即可用于提前显示时长；不能直接驱动 EOF
    uint64_t hint_start_frame = 0ULL;
    uint64_t hint_length_frames = 0ULL;
    uint32_t start_play_tick = 0U;
    uint32_t length_play_ticks = 0U;
    uint32_t hint_start_play_tick = 0U;
    uint32_t hint_length_play_ticks = 0U;
};

struct NsfSynthActivityInfo
{
    bool seen_audible = false;
    uint64_t silent_frames = 0ULL; // 仅后台分析实例累计的连续无可听通道活动帧数
};

// 开启可视采集的实例在每次 NSF PLAY 后记录一帧紧凑可视状态。
// Pulse1/Pulse2/Triangle 使用 MIDI 音高编号；Noise/DMC 只使用 active/trigger/level。
struct NsfSynthVisualTick
{
    uint32_t frame = 0U;
    uint8_t pitch[3] = {};
    uint8_t level[5] = {};
    uint8_t active_mask = 0U;
    uint8_t trigger_mask = 0U;
};


// R6：统一 Sequencer 输出的紧凑事件。play_tick=0 表示 INIT/初始写，1 表示第一次 PLAY。
// target 0x00~0x17 -> $4000~$4017 APU；0x18~0x1F -> $5FF8~$5FFF Bank。
struct NsfSynthApuEvent
{
    uint16_t play_tick = 0U;
    uint8_t target = 0U;
    uint8_t value = 0U;
};
static_assert(sizeof(NsfSynthApuEvent) == 4U, "NsfSynthApuEvent must remain compact");

// 不含6502 CPU的轻量真实播放器：只消费 Sequencer 已生成的 APU/Bank Event Timeline。
struct NsfApuRenderer
{
    void *impl = nullptr;
    uint32_t sample_rate_hz = 0U;
    uint64_t position_frames = 0ULL;
    uint8_t track = 0U;
    uint8_t track_count = 0U;
    bool open = false;
    bool failed = false;
};

// owned_prg 所有权在成功后转移给 NsfSynth；失败时调用方仍负责释放。
esp_err_t nsf_synth_open_owned(
    NsfSynth *synth,
    uint8_t *owned_prg,
    size_t prg_size,
    const NsfSynthConfig *config,
    uint32_t sample_rate_hz);

void nsf_synth_close(NsfSynth *synth);

// R6 唯一 Sequencer 的 PLAY-driven 推进：每步执行一次 NSF PLAY，并只推进 Envelope/Length/DMC 控制状态。
// 不生成 PCM；同一次扫描同时产出 APU Event、瀑布事件和 Loop/时长结果。
esp_err_t nsf_synth_analyze_play_calls(
    NsfSynth *synth,
    size_t max_calls,
    size_t *out_calls);

bool nsf_synth_is_open(const NsfSynth *synth);
bool nsf_synth_has_failed(const NsfSynth *synth);
uint64_t nsf_synth_position_frames(const NsfSynth *synth);
uint8_t nsf_synth_track(const NsfSynth *synth);
uint16_t nsf_synth_play_tick(const NsfSynth *synth);
bool nsf_synth_get_loop_info(const NsfSynth *synth, NsfSynthLoopInfo *out_info);
bool nsf_synth_get_activity_info(const NsfSynth *synth, NsfSynthActivityInfo *out_info);
size_t nsf_synth_take_visual_ticks(
    NsfSynth *synth,
    NsfSynthVisualTick *out_ticks,
    size_t capacity);
size_t nsf_synth_take_apu_events(
    NsfSynth *synth,
    NsfSynthApuEvent *out_events,
    size_t capacity);

esp_err_t nsf_apu_renderer_open_owned(
    NsfApuRenderer *renderer,
    uint8_t *owned_prg,
    size_t prg_size,
    const NsfSynthConfig *config,
    uint32_t sample_rate_hz);
void nsf_apu_renderer_close(NsfApuRenderer *renderer);
esp_err_t nsf_apu_renderer_set_track(NsfApuRenderer *renderer, uint8_t track);
esp_err_t nsf_apu_renderer_copy_source(
    const NsfApuRenderer *renderer,
    uint8_t **out_prg,
    size_t *out_prg_size,
    NsfSynthConfig *out_config);
esp_err_t nsf_apu_renderer_render_pcm32(
    NsfApuRenderer *renderer,
    const NsfSynthApuEvent *events,
    size_t event_count,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames,
    size_t *out_events_consumed);
bool nsf_apu_renderer_is_open(const NsfApuRenderer *renderer);
uint64_t nsf_apu_renderer_position_frames(const NsfApuRenderer *renderer);
