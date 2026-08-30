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

esp_err_t nsf_synth_render_pcm32(
    NsfSynth *synth,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames);

bool nsf_synth_is_open(const NsfSynth *synth);
bool nsf_synth_has_failed(const NsfSynth *synth);
uint64_t nsf_synth_position_frames(const NsfSynth *synth);
uint8_t nsf_synth_track(const NsfSynth *synth);
bool nsf_synth_get_loop_info(const NsfSynth *synth, NsfSynthLoopInfo *out_info);
