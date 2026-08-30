#include "midi_synth.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"

namespace
{

static constexpr size_t kVoiceCount = 32U;
static constexpr float kVoiceMasterGain = 0.060f;
static constexpr uint8_t kEnvelopeAttack = 0U;
static constexpr uint8_t kEnvelopeDecay = 1U;
static constexpr uint8_t kEnvelopeSustain = 2U;
static constexpr uint8_t kEnvelopeRelease = 3U;
static constexpr uint32_t kWaveTableSize = 256U;
static constexpr uint32_t kWaveBandCount = 11U;
static constexpr uint32_t kMaxTableHarmonics = 64U;
static constexpr float kTwoPi = 6.28318530718f;

static int16_t g_sine_table[kWaveTableSize] = {};
static int16_t g_saw_table[kWaveBandCount][kWaveTableSize] = {};
static int16_t g_square_table[kWaveBandCount][kWaveTableSize] = {};
static bool g_sine_ready = false;
static uint32_t g_bandlimited_table_rate_hz = 0U;

static uint64_t ms_to_frames(uint32_t ms, uint32_t sample_rate_hz)
{
    return (static_cast<uint64_t>(ms) * sample_rate_hz + 999ULL) / 1000ULL;
}

static float note_frequency_hz(uint8_t note)
{
    return 440.0f * powf(2.0f,
        static_cast<float>(static_cast<int32_t>(note) - 69) / 12.0f);
}

static void init_sine_table()
{
    if (g_sine_ready) return;
    for (uint32_t i = 0U; i < kWaveTableSize; ++i) {
        const float phase = (static_cast<float>(i) * kTwoPi) /
            static_cast<float>(kWaveTableSize);
        g_sine_table[i] = static_cast<int16_t>(sinf(phase) * 32767.0f);
    }
    g_sine_ready = true;
}

static float sine_sample(uint32_t index)
{
    return static_cast<float>(g_sine_table[index & (kWaveTableSize - 1U)]) / 32768.0f;
}

static void normalize_table(float *scratch, int16_t *dst)
{
    float peak = 0.000001f;
    for (uint32_t i = 0U; i < kWaveTableSize; ++i) {
        const float value = fabsf(scratch[i]);
        if (value > peak) peak = value;
    }
    const float scale = 0.94f / peak;
    for (uint32_t i = 0U; i < kWaveTableSize; ++i) {
        float value = scratch[i] * scale;
        if (value > 0.999f) value = 0.999f;
        if (value < -0.999f) value = -0.999f;
        dst[i] = static_cast<int16_t>(value * 32767.0f);
    }
}

static void build_bandlimited_table(
    int16_t *dst,
    uint32_t harmonic_limit,
    uint8_t waveform)
{
    float scratch[kWaveTableSize] = {};
    if (harmonic_limit == 0U) harmonic_limit = 1U;
    if (harmonic_limit > kMaxTableHarmonics) harmonic_limit = kMaxTableHarmonics;

    for (uint32_t harmonic = 1U; harmonic <= harmonic_limit; ++harmonic) {
        float weight = 0.0f;
        if (waveform == 0U) { // Saw
            weight = 1.0f / static_cast<float>(harmonic);
        } else { // Square
            if ((harmonic & 1U) == 0U) continue;
            weight = 1.0f / static_cast<float>(harmonic);
        }

        for (uint32_t i = 0U; i < kWaveTableSize; ++i) {
            scratch[i] += sine_sample(i * harmonic) * weight;
        }
    }
    normalize_table(scratch, dst);
}

static void init_bandlimited_tables(uint32_t sample_rate_hz)
{
    init_sine_table();
    if (g_bandlimited_table_rate_hz == sample_rate_hz) return;

    const float nyquist = static_cast<float>(sample_rate_hz) * 0.5f;
    for (uint32_t band = 0U; band < kWaveBandCount; ++band) {
        uint32_t high_note = band * 12U + 11U;
        if (high_note > 127U) high_note = 127U;
        const float high_frequency = note_frequency_hz(static_cast<uint8_t>(high_note));
        uint32_t harmonic_limit = high_frequency > 0.0f
            ? static_cast<uint32_t>(nyquist / high_frequency)
            : 1U;
        if (harmonic_limit == 0U) harmonic_limit = 1U;
        if (harmonic_limit > kMaxTableHarmonics) harmonic_limit = kMaxTableHarmonics;
        build_bandlimited_table(g_saw_table[band], harmonic_limit, 0U);
        build_bandlimited_table(g_square_table[band], harmonic_limit, 1U);
    }
    g_bandlimited_table_rate_hz = sample_rate_hz;
}

static float table_lookup(const int16_t *table, uint32_t phase)
{
    // AudioTask 48kHz 热路径使用直接波表读取；带限谐波比逐样本插值更值得保留，
    // 避免 32 voice 密集 MIDI 因额外浮点插值增加实时负载。
    const uint32_t index = phase >> 24U;
    return static_cast<float>(table[index]) / 32768.0f;
}

static float wave_sine(uint32_t phase)
{
    return table_lookup(g_sine_table, phase);
}

static float wave_saw(const MidiSynthVoice *voice)
{
    return table_lookup(g_saw_table[voice->wavetable_band], voice->phase);
}

static float wave_square(const MidiSynthVoice *voice)
{
    return table_lookup(g_square_table[voice->wavetable_band], voice->phase);
}

static float wave_triangle(const MidiSynthVoice *voice)
{
    const uint32_t p = voice->phase >> 16U;
    const int32_t folded = p < 32768U
        ? static_cast<int32_t>(p * 2U) - 32768
        : 98303 - static_cast<int32_t>(p * 2U);
    return static_cast<float>(folded) / 32768.0f;
}

static float wave_noise(MidiSynthVoice *voice)
{
    uint32_t x = voice->noise_state;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    if (x == 0U) x = 1U;
    voice->noise_state = x;
    return static_cast<float>(static_cast<int16_t>(x >> 16U)) / 32768.0f;
}

static uint32_t profile_attack_ms(uint8_t family)
{
    switch (family) {
        case 0: return 3U;     // Piano
        case 1: return 2U;     // Chromatic percussion
        case 2: return 6U;     // Organ
        case 3: return 2U;     // Guitar
        case 4: return 3U;     // Bass
        case 5: return 42U;    // Strings
        case 6: return 35U;    // Ensemble
        case 7: return 10U;    // Brass
        case 8: return 8U;     // Reed
        case 9: return 10U;    // Pipe
        case 10: return 3U;    // Synth lead
        case 11: return 95U;   // Synth pad
        case 12: return 45U;   // Synth FX
        case 13: return 4U;    // Ethnic
        case 14: return 1U;    // Percussive
        default: return 2U;    // Sound FX
    }
}

static uint32_t profile_decay_ms(uint8_t family)
{
    switch (family) {
        case 0: return 1800U;
        case 1: return 900U;
        case 2: return 90U;
        case 3: return 1100U;
        case 4: return 680U;
        case 5: return 520U;
        case 6: return 520U;
        case 7: return 300U;
        case 8: return 260U;
        case 9: return 220U;
        case 10: return 260U;
        case 11: return 650U;
        case 12: return 700U;
        case 13: return 850U;
        case 14: return 360U;
        default: return 420U;
    }
}

static float profile_sustain(uint8_t family)
{
    switch (family) {
        case 0: return 0.08f;
        case 1: return 0.02f;
        case 2: return 0.90f;
        case 3: return 0.06f;
        case 4: return 0.25f;
        case 5: return 0.76f;
        case 6: return 0.72f;
        case 7: return 0.68f;
        case 8: return 0.72f;
        case 9: return 0.78f;
        case 10: return 0.72f;
        case 11: return 0.70f;
        case 12: return 0.58f;
        case 13: return 0.20f;
        case 14: return 0.03f;
        default: return 0.15f;
    }
}

static uint32_t profile_release_ms(uint8_t family)
{
    switch (family) {
        case 0: return 280U;
        case 1: return 120U;
        case 2: return 130U;
        case 3: return 180U;
        case 4: return 120U;
        case 5: return 460U;
        case 6: return 420U;
        case 7: return 190U;
        case 8: return 180U;
        case 9: return 220U;
        case 10: return 160U;
        case 11: return 560U;
        case 12: return 420U;
        case 13: return 240U;
        case 14: return 80U;
        default: return 150U;
    }
}

static float profile_tone_gain(uint8_t family)
{
    switch (family) {
        case 0: return 0.96f;
        case 1: return 0.82f;
        case 2: return 0.72f;
        case 3: return 0.86f;
        case 4: return 1.04f;
        case 5: return 0.72f;
        case 6: return 0.70f;
        case 7: return 0.74f;
        case 8: return 0.76f;
        case 9: return 0.78f;
        case 10: return 0.72f;
        case 11: return 0.66f;
        case 12: return 0.64f;
        case 13: return 0.80f;
        case 14: return 0.82f;
        default: return 0.68f;
    }
}

static float profile_cutoff_hz(uint8_t family)
{
    switch (family) {
        case 0: return 7600.0f;
        case 1: return 9000.0f;
        case 2: return 6200.0f;
        case 3: return 6100.0f;
        case 4: return 3200.0f;
        case 5: return 7200.0f;
        case 6: return 6500.0f;
        case 7: return 6200.0f;
        case 8: return 5600.0f;
        case 9: return 8200.0f;
        case 10: return 9000.0f;
        case 11: return 5200.0f;
        case 12: return 9400.0f;
        case 13: return 6800.0f;
        case 14: return 8500.0f;
        default: return 9800.0f;
    }
}

static void enter_decay(MidiSynthVoice *voice, uint32_t sample_rate_hz)
{
    const uint8_t family = static_cast<uint8_t>(voice->program >> 3U);
    const uint64_t frames = ms_to_frames(profile_decay_ms(family), sample_rate_hz);
    voice->stage = kEnvelopeDecay;
    voice->sustain_level = profile_sustain(family);
    voice->decay_step = frames > 0ULL
        ? (1.0f - voice->sustain_level) / static_cast<float>(frames)
        : 1.0f;
}

static void begin_release(MidiSynthVoice *voice, uint32_t sample_rate_hz)
{
    if (voice == nullptr || !voice->active || voice->releasing) return;
    const uint8_t family = static_cast<uint8_t>(voice->program >> 3U);
    const uint64_t frames = ms_to_frames(profile_release_ms(family), sample_rate_hz);
    voice->releasing = true;
    voice->stage = kEnvelopeRelease;
    voice->release_step = frames > 0ULL
        ? voice->envelope / static_cast<float>(frames)
        : voice->envelope;
    if (voice->release_step <= 0.000001f) voice->release_step = 1.0f;
}

static void advance_envelope(MidiSynthVoice *voice, uint32_t sample_rate_hz)
{
    switch (voice->stage) {
        case kEnvelopeAttack:
            voice->envelope += voice->attack_step;
            if (voice->envelope >= 1.0f) {
                voice->envelope = 1.0f;
                enter_decay(voice, sample_rate_hz);
            }
            break;
        case kEnvelopeDecay:
            voice->envelope -= voice->decay_step;
            if (voice->envelope <= voice->sustain_level) {
                voice->envelope = voice->sustain_level;
                voice->stage = kEnvelopeSustain;
            }
            break;
        case kEnvelopeRelease:
            voice->envelope -= voice->release_step;
            if (voice->envelope <= 0.0001f) {
                voice->envelope = 0.0f;
                voice->active = false;
            }
            break;
        default:
            break;
    }
}

static MidiSynthVoice *allocate_voice(MidiSynth *synth)
{
    MidiSynthVoice *release_candidate = nullptr;
    MidiSynthVoice *oldest = &synth->voices[0];
    for (size_t i = 0U; i < kVoiceCount; ++i) {
        MidiSynthVoice &voice = synth->voices[i];
        if (!voice.active) return &voice;
        if (voice.releasing &&
            (release_candidate == nullptr || voice.envelope < release_candidate->envelope)) {
            release_candidate = &voice;
        }
        if (voice.started_frame < oldest->started_frame) oldest = &voice;
    }
    return release_candidate != nullptr ? release_candidate : oldest;
}

static float clamp_float(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void configure_voice_tone(MidiSynth *synth, MidiSynthVoice *voice)
{
    const float velocity = static_cast<float>(voice->velocity) / 127.0f;
    // MIDI velocity 是感知量，二次曲线可避免轻触音符和重击音符听起来几乎一样响。
    voice->velocity_gain = 0.18f * velocity + 0.82f * velocity * velocity;

    if (voice->channel == 9U) {
        voice->tone_gain = (voice->note == 35U || voice->note == 36U) ? 1.18f : 0.96f;
        float pan = 0.0f;
        if (voice->note == 42U || voice->note == 44U || voice->note == 46U) pan = 0.24f;
        else if (voice->note == 49U || voice->note == 51U || voice->note == 57U || voice->note == 59U) {
            pan = -0.22f;
        } else if (voice->note >= 41U && voice->note <= 50U) {
            pan = (static_cast<float>(voice->note) - 45.5f) * 0.045f;
        }
        voice->pan_left = sqrtf(0.5f * (1.0f - pan));
        voice->pan_right = sqrtf(0.5f * (1.0f + pan));
        voice->filter_alpha = 1.0f;
        return;
    }

    const uint8_t family = static_cast<uint8_t>(voice->program >> 3U);
    const float variant = static_cast<float>(voice->program & 0x07U) / 7.0f;
    voice->tone_gain = profile_tone_gain(family) * (0.94f + variant * 0.12f);

    // 低音略向左、高音略向右；Channel 只提供很小偏移，避免整首歌被硬拆成左右两边。
    const float pitch_pan = clamp_float(
        (static_cast<float>(voice->note) - 60.0f) / 60.0f * 0.34f,
        -0.34f,
        0.34f);
    const float channel_pan =
        (static_cast<float>(voice->channel & 0x03U) - 1.5f) * 0.055f;
    const float pan = clamp_float(pitch_pan + channel_pan, -0.52f, 0.52f);
    voice->pan_left = sqrtf(0.5f * (1.0f - pan));
    voice->pan_right = sqrtf(0.5f * (1.0f + pan));

    const float frequency = note_frequency_hz(voice->note);
    float cutoff = profile_cutoff_hz(family) *
        (0.88f + variant * 0.18f) *
        (0.78f + velocity * 0.30f);
    const float minimum_cutoff = frequency * 1.45f;
    if (cutoff < minimum_cutoff) cutoff = minimum_cutoff;
    const float maximum_cutoff = static_cast<float>(synth->sample_rate_hz) * 0.42f;
    if (cutoff > maximum_cutoff) cutoff = maximum_cutoff;
    voice->filter_alpha = 1.0f - expf(
        -kTwoPi * cutoff / static_cast<float>(synth->sample_rate_hz));
    voice->filter_alpha = clamp_float(voice->filter_alpha, 0.02f, 1.0f);
}

static void start_voice(MidiSynth *synth, const MidiSynthNoteEvent &note)
{
    MidiSynthVoice *voice = allocate_voice(synth);
    if (voice == nullptr) return;
    *voice = {};
    voice->active = true;
    voice->note = note.note;
    voice->velocity = note.velocity;
    voice->channel = note.channel;
    voice->program = note.program;
    voice->phase_increment = synth->phase_increment[note.note];
    voice->wavetable_band = static_cast<uint8_t>(note.note / 12U);
    if (voice->wavetable_band >= kWaveBandCount) voice->wavetable_band = static_cast<uint8_t>(kWaveBandCount - 1U);
    voice->started_frame = synth->position_frames;
    voice->noise_state = 0x9E3779B9UL ^
        (static_cast<uint32_t>(note.note) << 16U) ^
        (static_cast<uint32_t>(note.channel) << 8U) ^
        static_cast<uint32_t>(synth->next_note_index + 1U);
    configure_voice_tone(synth, voice);

    if (note.channel == 9U) {
        // GM Channel 10 为打击乐。忽略 MIDI NoteOff 长度，使用短包络避免鼓点被拉成长音。
        uint32_t drum_ms = 130U;
        if (note.note == 35U || note.note == 36U) drum_ms = 220U;
        else if (note.note == 42U || note.note == 44U || note.note == 46U) drum_ms = 80U;
        voice->end_frame = synth->position_frames + ms_to_frames(drum_ms, synth->sample_rate_hz);
        voice->stage = kEnvelopeDecay;
        voice->envelope = 1.0f;
        voice->sustain_level = 0.0f;
        const uint64_t decay_frames = ms_to_frames(drum_ms, synth->sample_rate_hz);
        voice->decay_step = decay_frames > 0ULL
            ? 1.0f / static_cast<float>(decay_frames)
            : 1.0f;
        return;
    }

    voice->end_frame = ms_to_frames(note.end_ms, synth->sample_rate_hz);
    const uint8_t family = static_cast<uint8_t>(note.program >> 3U);
    const uint64_t attack_frames = ms_to_frames(profile_attack_ms(family), synth->sample_rate_hz);
    voice->sustain_level = profile_sustain(family);
    if (attack_frames == 0ULL) {
        voice->envelope = 1.0f;
        enter_decay(voice, synth->sample_rate_hz);
    } else {
        voice->stage = kEnvelopeAttack;
        voice->envelope = 0.0f;
        voice->attack_step = 1.0f / static_cast<float>(attack_frames);
    }
}

static float melodic_wave(MidiSynthVoice *voice)
{
    const uint8_t family = static_cast<uint8_t>(voice->program >> 3U);
    const float variant = static_cast<float>(voice->program & 0x07U) / 7.0f;
    const float sine = wave_sine(voice->phase);

    // GM-Lite V2：锯齿/方波改用按音区限制谐波数量的波表；三角波保留低成本算法。
    switch (family) {
        case 0: { // Piano
            float value = wave_triangle(voice) * (0.56f - variant * 0.06f) + sine * 0.34f;
            if (voice->note < 96U) value += wave_sine(voice->phase * 2U) * 0.10f;
            return value;
        }
        case 1: { // Chromatic percussion
            float value = sine * 0.66f;
            if (voice->note < 96U) value += wave_sine(voice->phase * 2U) * 0.24f;
            if (voice->note < 84U) value += wave_sine(voice->phase * 3U) * 0.10f;
            return value;
        }
        case 2: { // Organ
            float value = sine * 0.56f;
            if (voice->note < 96U) value += wave_sine(voice->phase * 2U) * 0.24f;
            if (voice->note < 84U) value += wave_sine(voice->phase * 3U) * 0.14f;
            if (voice->note < 72U) value += wave_sine(voice->phase * 4U) * 0.06f;
            return value;
        }
        case 3:
            return wave_triangle(voice) * 0.58f + wave_saw(voice) * 0.42f;
        case 4:
            return wave_square(voice) * 0.44f + wave_triangle(voice) * 0.56f;
        case 5:
            return wave_saw(voice) * 0.54f + wave_triangle(voice) * 0.46f;
        case 6:
            return wave_triangle(voice) * 0.54f + wave_saw(voice) * 0.26f + sine * 0.20f;
        case 7:
            return wave_saw(voice) * 0.62f + wave_square(voice) * 0.18f + sine * 0.20f;
        case 8:
            return wave_square(voice) * 0.30f + wave_triangle(voice) * 0.50f + sine * 0.20f;
        case 9:
            return sine * 0.86f + wave_triangle(voice) * 0.14f;
        case 10:
            return wave_saw(voice) * (0.58f + variant * 0.12f) +
                wave_square(voice) * (0.42f - variant * 0.12f);
        case 11:
            return wave_triangle(voice) * 0.55f + wave_saw(voice) * 0.25f + sine * 0.20f;
        case 12:
            return wave_triangle(voice) * 0.50f + wave_noise(voice) * 0.18f + sine * 0.32f;
        case 13:
            return wave_triangle(voice) * 0.68f + wave_square(voice) * 0.20f + sine * 0.12f;
        case 14:
            return sine * 0.58f + wave_noise(voice) * 0.30f + wave_triangle(voice) * 0.12f;
        default:
            return wave_noise(voice) * 0.48f + wave_square(voice) * 0.32f + sine * 0.20f;
    }
}

static float drum_wave(MidiSynthVoice *voice)
{
    if (voice->note == 35U || voice->note == 36U) {
        // Kick：低频主体并缓慢向下扫频，减少单纯固定正弦的“测试音”感觉。
        const float value = wave_sine(voice->phase) * 0.88f + wave_triangle(voice) * 0.12f;
        voice->phase_increment -= voice->phase_increment >> 14U;
        return value;
    }

    const float noise = wave_noise(voice);
    if (voice->note == 42U || voice->note == 44U || voice->note == 46U ||
        voice->note == 49U || voice->note == 51U || voice->note == 57U || voice->note == 59U) {
        // Hi-hat / cymbal：用简单高通差分增强金属高频，不额外引入滤波器对象。
        const float high = noise - voice->noise_prev * 0.82f;
        voice->noise_prev = noise;
        return high * 0.82f + wave_square(voice) * 0.10f;
    }

    voice->noise_prev = noise;
    // Snare / tom / 其它：噪声与有音高成分混合。
    return noise * 0.66f + wave_triangle(voice) * 0.34f;
}

static float soft_limit(float value)
{
    const float magnitude = fabsf(value);
    if (magnitude <= 0.82f) return value;
    const float excess = magnitude - 0.82f;
    const float compressed = 0.82f + 0.18f * excess / (excess + 0.18f);
    return value < 0.0f ? -compressed : compressed;
}

static int32_t float_to_pcm32(float value)
{
    value = soft_limit(value);
    if (value > 0.9999f) value = 0.9999f;
    if (value < -1.0f) value = -1.0f;
    const int32_t sample16 = static_cast<int32_t>(value * 32767.0f);
    return sample16 * 65536;
}

} // namespace

esp_err_t midi_synth_open_owned(
    MidiSynth *synth,
    MidiSynthNoteEvent *owned_notes,
    size_t note_count,
    uint32_t duration_ms,
    uint32_t sample_rate_hz)
{
    if (synth == nullptr || owned_notes == nullptr || note_count == 0U ||
        duration_ms == 0U || sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    midi_synth_close(synth);
    init_bandlimited_tables(sample_rate_hz);
    synth->notes = owned_notes;
    synth->note_count = note_count;
    synth->duration_ms = duration_ms;
    synth->sample_rate_hz = sample_rate_hz;
    synth->total_frames = ms_to_frames(duration_ms, sample_rate_hz);
    if (synth->total_frames == 0ULL) synth->total_frames = 1ULL;

    for (uint32_t note = 0U; note < 128U; ++note) {
        const float frequency = note_frequency_hz(static_cast<uint8_t>(note));
        const double increment =
            (static_cast<double>(frequency) * 4294967296.0) /
            static_cast<double>(sample_rate_hz);
        synth->phase_increment[note] = increment < 1.0
            ? 1U
            : static_cast<uint32_t>(increment);
    }

    synth->open = true;
    midi_synth_restart(synth);
    return ESP_OK;
}

void midi_synth_close(MidiSynth *synth)
{
    if (synth == nullptr) return;
    if (synth->notes != nullptr) {
        heap_caps_free(synth->notes);
    }
    *synth = {};
}

void midi_synth_restart(MidiSynth *synth)
{
    if (synth == nullptr || !synth->open) return;
    synth->next_note_index = 0U;
    synth->position_frames = 0ULL;
    synth->eof = false;
    memset(synth->voices, 0, sizeof(synth->voices));
}

esp_err_t midi_synth_render_pcm32(
    MidiSynth *synth,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames)
{
    if (out_frames != nullptr) *out_frames = 0U;
    if (synth == nullptr || out_interleaved_stereo == nullptr || out_frames == nullptr ||
        max_frames == 0U || !synth->open) {
        return ESP_ERR_INVALID_ARG;
    }
    if (synth->eof || synth->position_frames >= synth->total_frames) {
        synth->eof = true;
        return ESP_OK;
    }

    size_t frames = max_frames;
    const uint64_t remaining = synth->total_frames - synth->position_frames;
    if (remaining < frames) frames = static_cast<size_t>(remaining);

    for (size_t frame = 0U; frame < frames; ++frame) {
        while (synth->next_note_index < synth->note_count) {
            const MidiSynthNoteEvent &note = synth->notes[synth->next_note_index];
            const uint64_t start_frame = ms_to_frames(note.start_ms, synth->sample_rate_hz);
            if (start_frame > synth->position_frames) break;
            start_voice(synth, note);
            ++synth->next_note_index;
        }

        float mix_left = 0.0f;
        float mix_right = 0.0f;
        for (size_t i = 0U; i < kVoiceCount; ++i) {
            MidiSynthVoice &voice = synth->voices[i];
            if (!voice.active) continue;
            if (!voice.releasing && synth->position_frames >= voice.end_frame) {
                begin_release(&voice, synth->sample_rate_hz);
            }

            float wave = 0.0f;
            if (voice.channel == 9U) {
                wave = drum_wave(&voice);
            } else {
                wave = melodic_wave(&voice);
                // 每 voice 一阶低通按 Program/Velocity/Pitch 预计算 alpha，削弱廉价波形的高频毛刺。
                voice.filter_state += voice.filter_alpha * (wave - voice.filter_state);
                wave = voice.filter_state;
            }

            const float level = voice.envelope * voice.velocity_gain *
                voice.tone_gain * kVoiceMasterGain;
            mix_left += wave * level * voice.pan_left;
            mix_right += wave * level * voice.pan_right;
            voice.phase += voice.phase_increment;
            advance_envelope(&voice, synth->sample_rate_hz);
        }

        out_interleaved_stereo[frame * 2U] = float_to_pcm32(mix_left);
        out_interleaved_stereo[frame * 2U + 1U] = float_to_pcm32(mix_right);
        ++synth->position_frames;
    }

    if (synth->position_frames >= synth->total_frames) synth->eof = true;
    *out_frames = frames;
    return ESP_OK;
}

bool midi_synth_is_open(const MidiSynth *synth)
{
    return synth != nullptr && synth->open;
}

bool midi_synth_is_eof(const MidiSynth *synth)
{
    return synth != nullptr && synth->open && synth->eof;
}

uint64_t midi_synth_position_frames(const MidiSynth *synth)
{
    return synth != nullptr && synth->open ? synth->position_frames : 0ULL;
}

uint64_t midi_synth_total_frames(const MidiSynth *synth)
{
    return synth != nullptr && synth->open ? synth->total_frames : 0ULL;
}
