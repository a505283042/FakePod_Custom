#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 电子音流 MIDI V2：由 UI 解析器生成的紧凑音符事件。
// AudioTask 接管一份 PSRAM 副本，避免跨任务持有 UI Timeline 指针。
struct MidiSynthNoteEvent
{
    uint32_t start_ms = 0U;
    uint32_t end_ms = 0U;
    uint8_t note = 0U;
    uint8_t velocity = 0U;
    uint8_t channel = 0U;
    uint8_t program = 0U;
};
static_assert(sizeof(MidiSynthNoteEvent) == 12U, "MidiSynthNoteEvent must remain compact");

struct MidiSynthVoice
{
    bool active = false;
    bool releasing = false;
    uint8_t note = 0U;
    uint8_t velocity = 0U;
    uint8_t channel = 0U;
    uint8_t program = 0U;
    uint8_t stage = 0U;
    uint32_t phase = 0U;
    uint32_t phase_increment = 0U;
    uint32_t noise_state = 1U;
    uint8_t wavetable_band = 0U;
    uint64_t end_frame = 0ULL;
    uint64_t started_frame = 0ULL;
    float envelope = 0.0f;
    float attack_step = 1.0f;
    float decay_step = 0.0f;
    float sustain_level = 0.75f;
    float release_step = 1.0f;
    float velocity_gain = 1.0f;
    float tone_gain = 1.0f;
    float filter_alpha = 1.0f;
    float filter_state = 0.0f;
    float pan_left = 0.70710678f;
    float pan_right = 0.70710678f;
    float noise_prev = 0.0f;
};

struct MidiSynth
{
    MidiSynthNoteEvent *notes = nullptr; // AudioTask 独占并负责释放的 PSRAM
    size_t note_count = 0U;
    size_t next_note_index = 0U;
    uint32_t duration_ms = 0U;
    uint32_t sample_rate_hz = 0U;
    uint64_t position_frames = 0ULL;
    uint64_t total_frames = 0ULL;
    uint32_t phase_increment[128] = {};
    MidiSynthVoice voices[32] = {};
    bool open = false;
    bool eof = false;
};

// owned_notes 所有权在成功后转移给 MidiSynth；失败时调用方仍负责释放。
esp_err_t midi_synth_open_owned(
    MidiSynth *synth,
    MidiSynthNoteEvent *owned_notes,
    size_t note_count,
    uint32_t duration_ms,
    uint32_t sample_rate_hz);

void midi_synth_close(MidiSynth *synth);
void midi_synth_restart(MidiSynth *synth);

esp_err_t midi_synth_render_pcm32(
    MidiSynth *synth,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames);

bool midi_synth_is_open(const MidiSynth *synth);
bool midi_synth_is_eof(const MidiSynth *synth);
uint64_t midi_synth_position_frames(const MidiSynth *synth);
uint64_t midi_synth_total_frames(const MidiSynth *synth);
