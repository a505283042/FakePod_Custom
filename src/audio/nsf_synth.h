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
    bool enable_visual_capture = false; // 实时播放实例采当前音符；独立预读实例采未来约4秒音符
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

// owned_prg 所有权在成功后转移给 NsfSynth；失败时调用方仍负责释放。
esp_err_t nsf_synth_open_owned(
    NsfSynth *synth,
    uint8_t *owned_prg,
    size_t prg_size,
    const NsfSynthConfig *config,
    uint32_t sample_rate_hz);

void nsf_synth_close(NsfSynth *synth);
esp_err_t nsf_synth_set_track(NsfSynth *synth, uint8_t track);

// 为后台分析复制当前 NSF 源数据和配置；返回的 PRG 由调用方负责释放或转交给 NsfSynth。
esp_err_t nsf_synth_copy_source(
    const NsfSynth *synth,
    uint8_t **out_prg,
    size_t *out_prg_size,
    NsfSynthConfig *out_config);

esp_err_t nsf_synth_render_pcm32(
    NsfSynth *synth,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames);

// R4 后台快速分析：每步直接执行 NSF PLAY，并只推进 Envelope/Length/DMC 等控制状态。
// 不生成 PCM；虚拟 position_frames 仍按 sample_rate_hz 时间基准累计。
esp_err_t nsf_synth_analyze_play_calls(
    NsfSynth *synth,
    size_t max_calls,
    size_t *out_calls);

bool nsf_synth_is_open(const NsfSynth *synth);
bool nsf_synth_has_failed(const NsfSynth *synth);
uint64_t nsf_synth_position_frames(const NsfSynth *synth);
uint8_t nsf_synth_track(const NsfSynth *synth);
bool nsf_synth_get_loop_info(const NsfSynth *synth, NsfSynthLoopInfo *out_info);
bool nsf_synth_get_activity_info(const NsfSynth *synth, NsfSynthActivityInfo *out_info);
size_t nsf_synth_take_visual_ticks(
    NsfSynth *synth,
    NsfSynthVisualTick *out_ticks,
    size_t capacity);
