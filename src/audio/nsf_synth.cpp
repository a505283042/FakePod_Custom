#include "nsf_synth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace
{

static const char *TAG = "NSF Synth";
static constexpr float kNtscCpuHz = 1789773.0f;
static constexpr uint16_t kDefaultNtscSpeedUs = 16639U;
static constexpr uint32_t kInitInstructionLimit = 2000000U;
static constexpr uint32_t kPlayInstructionLimit = 250000U;
static constexpr size_t kWorkRamBytes = 8192U;
static constexpr size_t kLoopHistoryCapacity = 24576U; // 典型60Hz下约409秒；双历史约192KB PSRAM，仅后台分析实例使用
static constexpr size_t kVisualTickCapacity = 32U; // 播放路每块通常不到1个PLAY；短暂UI锁竞争时仍有足够余量
static constexpr uint32_t kLoopDetectMinMs = 8000U;
static constexpr uint32_t kLoopDetectProbeMs = 1000U;
static constexpr uint32_t kLoopStructureProbeMs = 1000U;
// R4：结构 Loop 不再要求完整重复第二/第三整轮。
// 固定窗口至少覆盖 96 个 PLAY 状态且不少于 4 秒；首次命中后再向后 4 秒验证第二锚点。
static constexpr uint32_t kLoopFingerprintMinTicks = 96U;
static constexpr uint32_t kLoopFingerprintMinSpanMs = 4000U;
static constexpr uint32_t kLoopFingerprintVerifyOffsetMs = 4000U;
static constexpr uint32_t kLoopStructureVerifyMs = 90000U;
static constexpr uint32_t kLoopStructureFinalProbeMs = 5000U;
static constexpr uint32_t kLoopDetectMaxSearchMs = 150000U; // 覆盖较长BGM循环候选搜索
static constexpr uint16_t kLoopDetectMinTransitions = 4U;
static constexpr uint16_t kReturnSentinel = 0xFFFFU;
static constexpr float kTwoPi = 6.28318530718f;
static constexpr uint32_t kFnvOffset = 2166136261U;
static constexpr uint32_t kFnvPrime = 16777619U;

static constexpr uint8_t kLengthTable[32] = {
    10, 254, 20, 2, 40, 4, 80, 6,
    160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22,
    192, 24, 72, 26, 16, 28, 32, 30,
};

static constexpr uint16_t kNoisePeriodNtsc[16] = {
    4, 8, 16, 32, 64, 96, 128, 160,
    202, 254, 380, 508, 762, 1016, 2034, 4068,
};

static constexpr uint16_t kDmcPeriodNtsc[16] = {
    428, 380, 340, 320, 286, 254, 226, 214,
    190, 160, 142, 128, 106, 85, 72, 54,
};

static constexpr uint8_t kPulseDuty[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1},
};

static constexpr uint8_t kTriangleSequence[32] = {
    15, 14, 13, 12, 11, 10, 9, 8,
    7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 13, 14, 15,
};

enum CpuFlag : uint8_t
{
    FlagC = 0x01,
    FlagZ = 0x02,
    FlagI = 0x04,
    FlagD = 0x08,
    FlagB = 0x10,
    FlagU = 0x20,
    FlagV = 0x40,
    FlagN = 0x80,
};

struct Envelope
{
    uint8_t period = 0U;
    uint8_t divider = 0U;
    uint8_t decay = 0U;
    bool constant = false;
    bool loop = false;
    bool start = false;
};

struct PulseChannel
{
    uint16_t timer = 0U;
    uint8_t duty = 0U;
    uint8_t length = 0U;
    Envelope envelope = {};
    bool enabled = false;

    bool sweep_enabled = false;
    bool sweep_negate = false;
    bool sweep_reload = false;
    uint8_t sweep_period = 0U;
    uint8_t sweep_divider = 0U;
    uint8_t sweep_shift = 0U;

    float phase = 0.0f;
};

struct TriangleChannel
{
    uint16_t timer = 0U;
    uint8_t length = 0U;
    uint8_t linear_reload_value = 0U;
    uint8_t linear_counter = 0U;
    bool control = false;
    bool linear_reload = false;
    bool enabled = false;
    float phase = 0.0f;
};

struct NoiseChannel
{
    uint8_t period_index = 0U;
    uint8_t length = 0U;
    uint16_t lfsr = 1U;
    Envelope envelope = {};
    bool mode = false;
    bool enabled = false;
    float phase = 0.0f;
};

struct DmcChannel
{
    uint8_t rate_index = 0U;
    uint8_t output_level = 0U;
    uint8_t sample_address_reg = 0U;
    uint8_t sample_length_reg = 0U;
    uint16_t current_address = 0xC000U;
    uint16_t bytes_remaining = 0U;
    uint8_t shift_register = 0U;
    uint8_t bits_remaining = 0U;
    bool loop = false;
    bool enabled = false;
    bool silence = true;
    float phase = 0.0f;
};

struct NesApu
{
    PulseChannel pulse[2] = {};
    TriangleChannel triangle = {};
    NoiseChannel noise = {};
    DmcChannel dmc = {};
    uint8_t status = 0U;
    uint8_t frame_counter = 0x40U;
    uint32_t quarter_step = 0U;
    float quarter_phase = 0.0f;
    float hp_prev_in = 0.0f;
    float hp_prev_out = 0.0f;
    float lp_state = 0.0f;
};

struct Cpu6502
{
    uint8_t a = 0U;
    uint8_t x = 0U;
    uint8_t y = 0U;
    uint8_t sp = 0xFDU;
    uint8_t p = static_cast<uint8_t>(FlagI | FlagU);
    uint16_t pc = 0U;
    bool jammed = false;
    uint8_t jam_opcode = 0U;
};

struct NsfSynthImpl
{
    uint8_t *prg = nullptr;       // PSRAM，Synth 独占
    size_t prg_size = 0U;
    uint8_t ram[2048] = {};       // 6502 Zero-page/Stack 热路径留在内部 RAM
    uint8_t *work_ram = nullptr;  // $6000-$7FFF NSF 工作 RAM，8KB PSRAM

    NsfSynthConfig config = {};
    Cpu6502 cpu = {};
    NesApu apu = {};
    uint8_t bank[8] = {};
    bool uses_banking = false;
    uint16_t load_padding = 0U;

    uint8_t apu_registers[0x18] = {};
    // Loop 签名只记录会产生重触发/时序副作用的寄存器写入。
    // 普通参数寄存器的最终值已包含在 apu_registers 中，重复写入本身不应导致循环失配。
    uint32_t play_write_mask = 0U;

    // 仅后台分析实例在约60Hz PLAY 边界记录驱动状态；实时48kHz播放实例不开启循环搜索。
    uint32_t *loop_history = nullptr; // PSRAM，每项一个严格可听状态签名
    uint32_t *loop_structure_history = nullptr; // PSRAM，每项一个音高/节奏结构签名
    uint32_t loop_history_count = 0U;
    uint32_t loop_candidate_start = 0U;
    uint32_t loop_candidate_current = 0U;
    uint32_t loop_candidate_match = 0U;
    uint16_t loop_candidate_transitions = 0U;
    bool loop_candidate_active = false;
    bool loop_detected = false;
    uint64_t loop_start_frame = 0ULL;
    uint64_t loop_length_frames = 0ULL;
    bool loop_hint_available = false;
    uint64_t loop_hint_start_frame = 0ULL;
    uint64_t loop_hint_length_frames = 0ULL;
    bool structure_candidate_active = false;
    uint32_t structure_candidate_start = 0U;
    uint32_t structure_candidate_repeat = 0U;

    // 后台时长分析同时跟踪自然静音；实时48kHz实例不开启该统计。
    bool analysis_seen_audible = false;
    uint64_t analysis_silent_frames = 0ULL;

    // 可视状态只由真实播放实例产生；后台分析实例不参与瀑布时间轴。
    NsfSynthVisualTick *visual_ticks = nullptr;
    size_t visual_tick_read = 0U;
    size_t visual_tick_count = 0U;
    // 音高换算只在定时器变化时执行；把实时播放路的 log2f 成本限制在音高真正变化时。
    uint16_t visual_pitch_timer[3] = {};
    uint8_t visual_pitch_cache[3] = {};
    uint8_t visual_pitch_valid_mask = 0U;

    uint32_t sample_rate_hz = 0U;
    uint32_t play_speed_us = kDefaultNtscSpeedUs;
    float hp_alpha = 0.0f;
    float lp_alpha = 0.0f;
    uint64_t play_interval_q32 = 0ULL;
    uint64_t play_phase_q32 = 0ULL;
    uint64_t analysis_position_q32 = 0ULL;
    bool failed = false;
};

static uint32_t fnv_mix(uint32_t hash, uint8_t value)
{
    hash ^= value;
    return hash * kFnvPrime;
}

static bool is_loop_audio_register(uint16_t address)
{
    // 只保留当前 APU 实现真正产生音频状态变化的寄存器；忽略 $4009/$400D/$4014/$4016 等无关写入。
    return (address >= 0x4000U && address <= 0x4008U) ||
        (address >= 0x400AU && address <= 0x400CU) ||
        (address >= 0x400EU && address <= 0x4013U) ||
        address == 0x4015U || address == 0x4017U;
}

static bool is_loop_trigger_register(uint16_t address)
{
    // 这些寄存器即使重复写入相同值，也会重载包络/长度/相位或帧时序，因此保留“本帧写过”信息。
    return address == 0x4001U || address == 0x4003U ||
        address == 0x4005U || address == 0x4007U ||
        address == 0x400BU || address == 0x400FU ||
        address == 0x4015U || address == 0x4017U;
}

static uint32_t play_ticks_for_ms(const NsfSynthImpl *impl, uint32_t ms)
{
    if (impl == nullptr || impl->play_speed_us == 0U) return 1U;
    const uint64_t numerator = static_cast<uint64_t>(ms) * 1000ULL + impl->play_speed_us - 1ULL;
    const uint64_t ticks = numerator / impl->play_speed_us;
    return static_cast<uint32_t>(ticks > 0ULL ? ticks : 1ULL);
}

static uint64_t play_tick_to_frame(const NsfSynthImpl *impl, uint32_t tick)
{
    if (impl == nullptr) return 0ULL;
    // NSF Header 的 speed_us 为16位；在当前历史容量内乘积远低于 uint64_t 上限。
    return (static_cast<uint64_t>(tick) * impl->play_interval_q32) >> 32U;
}

static uint32_t build_play_signature(const NsfSynthImpl *impl)
{
    uint32_t hash = kFnvOffset;
    for (uint8_t value : impl->apu_registers) hash = fnv_mix(hash, value);
    // Bank 是程序/数据映射状态，不是可听状态；音乐数据跨 Bank 轮换时不应阻止等价 BGM 循环识别。
    // 保留会产生重触发副作用的寄存器写入节奏，但忽略普通参数寄存器的无害重复写与写入顺序。
    for (uint8_t shift = 0U; shift < 32U; shift += 8U) {
        hash = fnv_mix(hash, static_cast<uint8_t>(impl->play_write_mask >> shift));
    }
    return hash;
}

static uint32_t build_structure_signature(const NsfSynthImpl *impl)
{
    // 第二级检测只比较决定旋律/节奏结构的参数，忽略音量包络、长度计数与 Sweep 细节。
    // 同一 B 段即使每轮动态细节略有变化，只要音高与重触发节奏一致，仍可识别结构循环。
    uint32_t hash = kFnvOffset;
    hash = fnv_mix(hash, static_cast<uint8_t>(impl->apu_registers[0x00U] & 0xC0U));
    hash = fnv_mix(hash, impl->apu_registers[0x02U]);
    hash = fnv_mix(hash, static_cast<uint8_t>(impl->apu_registers[0x03U] & 0x07U));
    hash = fnv_mix(hash, static_cast<uint8_t>(impl->apu_registers[0x04U] & 0xC0U));
    hash = fnv_mix(hash, impl->apu_registers[0x06U]);
    hash = fnv_mix(hash, static_cast<uint8_t>(impl->apu_registers[0x07U] & 0x07U));
    hash = fnv_mix(hash, impl->apu_registers[0x0AU]);
    hash = fnv_mix(hash, static_cast<uint8_t>(impl->apu_registers[0x0BU] & 0x07U));
    hash = fnv_mix(hash, static_cast<uint8_t>(impl->apu_registers[0x0EU] & 0x8FU));
    hash = fnv_mix(hash, static_cast<uint8_t>(impl->apu_registers[0x10U] & 0x0FU));
    hash = fnv_mix(hash, static_cast<uint8_t>(impl->apu_registers[0x15U] & 0x1FU));
    const uint32_t rhythm_mask = impl->play_write_mask &
        ((1UL << 0x03U) | (1UL << 0x07U) | (1UL << 0x0BU) |
         (1UL << 0x0FU) | (1UL << 0x15U));
    for (uint8_t shift = 0U; shift < 24U; shift += 8U) {
        hash = fnv_mix(hash, static_cast<uint8_t>(rhythm_mask >> shift));
    }
    return hash;
}

static uint32_t structure_fingerprint_window_ticks(const NsfSynthImpl *impl)
{
    uint32_t ticks = play_ticks_for_ms(impl, kLoopFingerprintMinSpanMs);
    if (ticks < kLoopFingerprintMinTicks) ticks = kLoopFingerprintMinTicks;
    return ticks;
}

static bool structure_window_equal(
    const NsfSynthImpl *impl,
    uint32_t first,
    uint32_t second,
    uint32_t ticks,
    uint16_t *out_transitions)
{
    if (out_transitions != nullptr) *out_transitions = 0U;
    if (impl == nullptr || impl->loop_structure_history == nullptr || ticks == 0U) return false;

    uint16_t transitions = 0U;
    for (uint32_t i = 0U; i < ticks; ++i) {
        const uint32_t expected = impl->loop_structure_history[first + i];
        if (expected != impl->loop_structure_history[second + i]) return false;
        if (i > 0U && expected != impl->loop_structure_history[first + i - 1U] &&
            transitions != UINT16_MAX) {
            ++transitions;
        }
    }
    if (out_transitions != nullptr) *out_transitions = transitions;
    return true;
}

static bool try_find_structure_fingerprint_hint(NsfSynthImpl *impl, uint32_t current)
{
    if (impl == nullptr || impl->loop_structure_history == nullptr ||
        impl->loop_hint_available || current < 2U) {
        return false;
    }

    const uint32_t window_ticks = structure_fingerprint_window_ticks(impl);
    const uint32_t verify_offset_ticks =
        play_ticks_for_ms(impl, kLoopFingerprintVerifyOffsetMs);
    const uint32_t count = current + 1U;

    // 已找到第一锚点 A/A' 后，只需等待固定偏移处的 B' 窗口完整出现。
    if (impl->structure_candidate_active) {
        const uint64_t verify_end64 =
            static_cast<uint64_t>(impl->structure_candidate_repeat) +
            verify_offset_ticks + window_ticks;
        if (verify_end64 <= count) {
            uint16_t transitions = 0U;
            const bool same = structure_window_equal(
                impl,
                impl->structure_candidate_start + verify_offset_ticks,
                impl->structure_candidate_repeat + verify_offset_ticks,
                window_ticks,
                &transitions);
            if (same && transitions >= kLoopDetectMinTransitions) {
                uint32_t start = impl->structure_candidate_start;
                uint32_t repeated = impl->structure_candidate_repeat;
                while (start > 0U && repeated > 0U &&
                       impl->loop_structure_history[start - 1U] ==
                           impl->loop_structure_history[repeated - 1U]) {
                    --start;
                    --repeated;
                }
                const uint32_t loop_ticks = repeated - start;
                impl->loop_hint_start_frame = play_tick_to_frame(impl, start);
                impl->loop_hint_length_frames = play_tick_to_frame(impl, loop_ticks);
                impl->loop_hint_available = impl->loop_hint_length_frames > 0ULL;
                if (impl->loop_hint_available) {
                    ESP_LOGI(TAG,
                        "NSF循环候选已识别：start=%llums loop=%llums verified=fixed_windows window=%lums offset=%lums signatures=%lu",
                        static_cast<unsigned long long>(
                            impl->loop_hint_start_frame * 1000ULL / impl->sample_rate_hz),
                        static_cast<unsigned long long>(
                            impl->loop_hint_length_frames * 1000ULL / impl->sample_rate_hz),
                        static_cast<unsigned long>(
                            window_ticks * impl->play_speed_us / 1000U),
                        static_cast<unsigned long>(
                            verify_offset_ticks * impl->play_speed_us / 1000U),
                        static_cast<unsigned long>(impl->loop_history_count));
                }
                impl->structure_candidate_active = false;
                return impl->loop_hint_available;
            }
            // 第一锚点只是局部重复，不保留错误候选；继续寻找下一组 A/A'。
            impl->structure_candidate_active = false;
        }
        return false;
    }

    const uint32_t probe_ticks = play_ticks_for_ms(impl, kLoopStructureProbeMs);
    if (probe_ticks == 0U || (current % probe_ticks) != 0U || count < window_ticks) return false;

    const uint32_t repeat_start = count - window_ticks;
    const uint32_t min_loop_ticks = play_ticks_for_ms(impl, kLoopDetectMinMs);
    const uint32_t max_loop_ticks = play_ticks_for_ms(impl, kLoopDetectMaxSearchMs);
    if (repeat_start < min_loop_ticks) return false;

    const uint32_t newest_candidate = repeat_start - min_loop_ticks;
    const uint32_t oldest_candidate = repeat_start > max_loop_ticks
        ? repeat_start - max_loop_ticks
        : 0U;
    const uint32_t middle = window_ticks / 2U;
    const uint32_t last = window_ticks - 1U;

    // 从最近的合法周期向前搜索。先比较三个种子点，命中后只核验固定窗口，
    // 不再把 candidate_period 个状态完整比较一整轮。
    for (uint32_t candidate = newest_candidate;; --candidate) {
        if (impl->loop_structure_history[candidate] ==
                impl->loop_structure_history[repeat_start] &&
            impl->loop_structure_history[candidate + middle] ==
                impl->loop_structure_history[repeat_start + middle] &&
            impl->loop_structure_history[candidate + last] ==
                impl->loop_structure_history[repeat_start + last]) {
            uint16_t transitions = 0U;
            if (structure_window_equal(
                    impl, candidate, repeat_start, window_ticks, &transitions) &&
                transitions >= kLoopDetectMinTransitions) {
                impl->structure_candidate_active = true;
                impl->structure_candidate_start = candidate;
                impl->structure_candidate_repeat = repeat_start;
                break;
            }
        }
        if (candidate == oldest_candidate) break;
    }
    return false;
}

static bool try_detect_structure_loop(NsfSynthImpl *impl, uint32_t current)
{
    if (impl == nullptr || impl->loop_structure_history == nullptr || current < 2U) return false;

    const uint32_t probe_ticks = play_ticks_for_ms(impl, kLoopStructureFinalProbeMs);
    if (probe_ticks == 0U || (current % probe_ticks) != 0U) return false;

    const uint32_t min_loop_ticks = play_ticks_for_ms(impl, kLoopDetectMinMs);
    const uint32_t max_loop_ticks = play_ticks_for_ms(impl, kLoopDetectMaxSearchMs);
    const uint32_t verify_ticks = play_ticks_for_ms(impl, kLoopStructureVerifyMs);
    const uint32_t count = current + 1U;
    if (count < verify_ticks) return false;

    uint32_t max_period = count / 3U;
    if (max_period > max_loop_ticks) max_period = max_loop_ticks;
    if (max_period < min_loop_ticks) return false;

    // 最终 EOF 仍保留 R1 的严格语义：至少90秒且不少于三整轮。
    // R4 固定窗口只负责提前给 UI duration hint，不直接截断 Track。
    for (uint32_t period = min_loop_ticks; period <= max_period; ++period) {
        uint32_t repeats = (verify_ticks + period - 1U) / period;
        if (repeats < 3U) repeats = 3U;
        const uint64_t span64 = static_cast<uint64_t>(period) * repeats;
        if (span64 > count) continue;
        const uint32_t span = static_cast<uint32_t>(span64);
        const uint32_t start = count - span;
        const uint32_t middle = period / 2U;
        const uint32_t last = period - 1U;

        bool seed_match = true;
        for (uint32_t r = 1U; r < repeats; ++r) {
            const uint32_t base = start + r * period;
            if (impl->loop_structure_history[start] != impl->loop_structure_history[base] ||
                impl->loop_structure_history[start + middle] !=
                    impl->loop_structure_history[base + middle] ||
                impl->loop_structure_history[start + last] !=
                    impl->loop_structure_history[base + last]) {
                seed_match = false;
                break;
            }
        }
        if (!seed_match) continue;

        uint16_t transitions = 0U;
        bool same = true;
        for (uint32_t i = 0U; i < period && same; ++i) {
            const uint32_t expected = impl->loop_structure_history[start + i];
            for (uint32_t r = 1U; r < repeats; ++r) {
                if (expected != impl->loop_structure_history[start + r * period + i]) {
                    same = false;
                    break;
                }
            }
            if (i > 0U && expected != impl->loop_structure_history[start + i - 1U] &&
                transitions != UINT16_MAX) {
                ++transitions;
            }
        }
        if (!same || transitions < kLoopDetectMinTransitions) continue;

        uint32_t loop_start = start;
        while (loop_start > 0U &&
               impl->loop_structure_history[loop_start - 1U] ==
                   impl->loop_structure_history[loop_start - 1U + period]) {
            --loop_start;
        }
        impl->loop_start_frame = play_tick_to_frame(impl, loop_start);
        impl->loop_length_frames = play_tick_to_frame(impl, period);
        impl->loop_detected = impl->loop_length_frames > 0ULL;
        if (impl->loop_detected) {
            ESP_LOGI(TAG,
                "NSF循环已识别：start=%llums loop=%llums verified=%lu_cycles signature=music_structure signatures=%lu",
                static_cast<unsigned long long>(
                    impl->loop_start_frame * 1000ULL / impl->sample_rate_hz),
                static_cast<unsigned long long>(
                    impl->loop_length_frames * 1000ULL / impl->sample_rate_hz),
                static_cast<unsigned long>(repeats),
                static_cast<unsigned long>(impl->loop_history_count));
            return true;
        }
    }
    return false;
}

static void reset_loop_detector(NsfSynthImpl *impl)
{
    if (impl == nullptr) return;
    impl->loop_history_count = 0U;
    impl->loop_candidate_start = 0U;
    impl->loop_candidate_current = 0U;
    impl->loop_candidate_match = 0U;
    impl->loop_candidate_transitions = 0U;
    impl->loop_candidate_active = false;
    impl->loop_detected = false;
    impl->loop_start_frame = 0ULL;
    impl->loop_length_frames = 0ULL;
    impl->loop_hint_available = false;
    impl->loop_hint_start_frame = 0ULL;
    impl->loop_hint_length_frames = 0ULL;
    impl->structure_candidate_active = false;
    impl->structure_candidate_start = 0U;
    impl->structure_candidate_repeat = 0U;
}

static void loop_detector_on_play(NsfSynth *synth)
{
    if (synth == nullptr || synth->impl == nullptr) return;
    NsfSynthImpl *impl = static_cast<NsfSynthImpl *>(synth->impl);
    if (!impl->config.enable_loop_detection || impl->loop_history == nullptr || impl->loop_detected ||
        impl->loop_history_count >= kLoopHistoryCapacity) {
        return;
    }

    const uint32_t signature = build_play_signature(impl);
    const uint32_t current = impl->loop_history_count;
    impl->loop_history[current] = signature;
    if (impl->loop_structure_history != nullptr) {
        impl->loop_structure_history[current] = build_structure_signature(impl);
    }
    ++impl->loop_history_count;

    // R4：固定窗口 + 第二锚点先给 duration hint；最终 EOF 仍由严格检测确认。
    (void)try_find_structure_fingerprint_hint(impl, current);
    if (try_detect_structure_loop(impl, current)) return;

    const uint32_t min_loop_ticks = play_ticks_for_ms(impl, kLoopDetectMinMs);
    const uint32_t probe_ticks = play_ticks_for_ms(impl, kLoopDetectProbeMs);
    const uint32_t max_search_ticks = play_ticks_for_ms(impl, kLoopDetectMaxSearchMs);

    if (impl->loop_candidate_active) {
        const uint32_t expected = impl->loop_candidate_start + impl->loop_candidate_match;
        const uint32_t expected_current = impl->loop_candidate_current + impl->loop_candidate_match;
        if (current == expected_current && expected < impl->loop_candidate_current &&
            impl->loop_history[expected] == signature) {
            if (current > 0U && impl->loop_history[current - 1U] != signature &&
                impl->loop_candidate_transitions != UINT16_MAX) {
                ++impl->loop_candidate_transitions;
            }
            ++impl->loop_candidate_match;
            const uint32_t candidate_period =
                impl->loop_candidate_current - impl->loop_candidate_start;
            // 不能只靠几秒相似片段确认循环；候选周期必须完整重复一整轮。
            if (candidate_period > 0U && impl->loop_candidate_match >= candidate_period) {
                if (impl->loop_candidate_transitions >= kLoopDetectMinTransitions) {
                    uint32_t start = impl->loop_candidate_start;
                    uint32_t repeated = impl->loop_candidate_current;
                    while (start > 0U && repeated > 0U &&
                           impl->loop_history[start - 1U] == impl->loop_history[repeated - 1U]) {
                        --start;
                        --repeated;
                    }
                    const uint32_t loop_ticks = repeated - start;
                    impl->loop_start_frame = play_tick_to_frame(impl, start);
                    impl->loop_length_frames = play_tick_to_frame(impl, loop_ticks);
                    impl->loop_detected = impl->loop_length_frames > 0ULL;
                    if (impl->loop_detected) {
                        ESP_LOGI(TAG,
                            "NSF循环已识别：start=%llums loop=%llums verified=full_cycle signature=audible_state signatures=%lu",
                            static_cast<unsigned long long>(
                                impl->loop_start_frame * 1000ULL / impl->sample_rate_hz),
                            static_cast<unsigned long long>(
                                impl->loop_length_frames * 1000ULL / impl->sample_rate_hz),
                            static_cast<unsigned long>(impl->loop_history_count));
                    }
                }
                impl->loop_candidate_active = false;
            }
            return;
        }
        impl->loop_candidate_active = false;
    }

    if (current < min_loop_ticks + 2U || probe_ticks == 0U ||
        (current % probe_ticks) != 0U) {
        return;
    }

    const uint32_t newest_candidate = current - min_loop_ticks;
    const uint32_t oldest_candidate = current > max_search_ticks
        ? current - max_search_ticks
        : 2U;
    if (newest_candidate < oldest_candidate || current < 2U) return;

    for (uint32_t candidate = newest_candidate;; --candidate) {
        if (candidate >= 2U &&
            impl->loop_history[candidate] == signature &&
            impl->loop_history[candidate - 1U] == impl->loop_history[current - 1U] &&
            impl->loop_history[candidate - 2U] == impl->loop_history[current - 2U]) {
            impl->loop_candidate_start = candidate - 2U;
            impl->loop_candidate_current = current - 2U;
            impl->loop_candidate_match = 3U;
            impl->loop_candidate_transitions =
                (impl->loop_history[current - 2U] != impl->loop_history[current - 1U] ? 1U : 0U) +
                (impl->loop_history[current - 1U] != signature ? 1U : 0U);
            impl->loop_candidate_active = true;
            break;
        }
        if (candidate == oldest_candidate) break;
    }
}

static void set_flag(Cpu6502 *cpu, uint8_t flag, bool value)
{
    if (value) cpu->p |= flag;
    else cpu->p &= static_cast<uint8_t>(~flag);
    cpu->p |= FlagU;
}

static bool get_flag(const Cpu6502 *cpu, uint8_t flag)
{
    return (cpu->p & flag) != 0U;
}

static void set_nz(Cpu6502 *cpu, uint8_t value)
{
    set_flag(cpu, FlagZ, value == 0U);
    set_flag(cpu, FlagN, (value & 0x80U) != 0U);
}

static uint8_t envelope_volume(const Envelope &envelope)
{
    return envelope.constant ? envelope.period : envelope.decay;
}

static void envelope_clock(Envelope *envelope)
{
    if (envelope->start) {
        envelope->start = false;
        envelope->decay = 15U;
        envelope->divider = envelope->period;
        return;
    }
    if (envelope->divider > 0U) {
        --envelope->divider;
        return;
    }
    envelope->divider = envelope->period;
    if (envelope->decay > 0U) --envelope->decay;
    else if (envelope->loop) envelope->decay = 15U;
}

static void clock_length(uint8_t *length, bool halt)
{
    if (!halt && *length > 0U) --(*length);
}

static uint16_t pulse_sweep_target(const PulseChannel &pulse, bool first_channel)
{
    if (pulse.sweep_shift == 0U) return pulse.timer;
    const uint16_t change = static_cast<uint16_t>(pulse.timer >> pulse.sweep_shift);
    if (pulse.sweep_negate) {
        const uint16_t extra = first_channel ? 1U : 0U;
        return pulse.timer > static_cast<uint16_t>(change + extra)
            ? static_cast<uint16_t>(pulse.timer - change - extra)
            : 0U;
    }
    return static_cast<uint16_t>(pulse.timer + change);
}

static void pulse_sweep_clock(PulseChannel *pulse, bool first_channel)
{
    const bool divider_zero = pulse->sweep_divider == 0U;
    if (divider_zero && pulse->sweep_enabled && pulse->sweep_shift > 0U) {
        const uint16_t target = pulse_sweep_target(*pulse, first_channel);
        if (pulse->timer >= 8U && target <= 0x07FFU) pulse->timer = target;
    }

    if (divider_zero || pulse->sweep_reload) {
        pulse->sweep_divider = pulse->sweep_period;
        pulse->sweep_reload = false;
    } else {
        --pulse->sweep_divider;
    }
}

static void apu_quarter_frame(NesApu *apu)
{
    envelope_clock(&apu->pulse[0].envelope);
    envelope_clock(&apu->pulse[1].envelope);
    envelope_clock(&apu->noise.envelope);

    TriangleChannel &triangle = apu->triangle;
    if (triangle.linear_reload) triangle.linear_counter = triangle.linear_reload_value;
    else if (triangle.linear_counter > 0U) --triangle.linear_counter;
    if (!triangle.control) triangle.linear_reload = false;
}

static void apu_half_frame(NesApu *apu)
{
    clock_length(&apu->pulse[0].length, apu->pulse[0].envelope.loop);
    clock_length(&apu->pulse[1].length, apu->pulse[1].envelope.loop);
    clock_length(&apu->triangle.length, apu->triangle.control);
    clock_length(&apu->noise.length, apu->noise.envelope.loop);
    pulse_sweep_clock(&apu->pulse[0], true);
    pulse_sweep_clock(&apu->pulse[1], false);
}

static void apu_advance_frame_sequencer(NesApu *apu, float quarter_ticks)
{
    if (apu == nullptr || quarter_ticks <= 0.0f) return;
    apu->quarter_phase += quarter_ticks;
    while (apu->quarter_phase >= 1.0f) {
        apu->quarter_phase -= 1.0f;
        if ((apu->frame_counter & 0x80U) != 0U) {
            // 5-step：Q, Q+H, Q, -, Q+H。
            switch (apu->quarter_step) {
                case 0U:
                    apu_quarter_frame(apu);
                    break;
                case 1U:
                    apu_quarter_frame(apu);
                    apu_half_frame(apu);
                    break;
                case 2U:
                    apu_quarter_frame(apu);
                    break;
                case 3U:
                    break;
                default:
                    apu_quarter_frame(apu);
                    apu_half_frame(apu);
                    break;
            }
            apu->quarter_step = (apu->quarter_step + 1U) % 5U;
        } else {
            // 4-step：Q, Q+H, Q, Q+H；IRQ 在 NSF1 基础播放器中不参与。
            apu_quarter_frame(apu);
            if ((apu->quarter_step & 1U) != 0U) apu_half_frame(apu);
            apu->quarter_step = (apu->quarter_step + 1U) & 0x03U;
        }
    }
}

static void apu_reset(NesApu *apu)
{
    *apu = {};
    apu->noise.lfsr = 1U;
    apu->frame_counter = 0x40U;
}

static void apu_write(NesApu *apu, uint16_t address, uint8_t value)
{
    if (address >= 0x4000U && address <= 0x4007U) {
        const uint8_t index = address >= 0x4004U ? 1U : 0U;
        PulseChannel &pulse = apu->pulse[index];
        const uint8_t reg = static_cast<uint8_t>((address - 0x4000U) & 0x03U);
        if (reg == 0U) {
            pulse.duty = static_cast<uint8_t>((value >> 6U) & 0x03U);
            pulse.envelope.loop = (value & 0x20U) != 0U;
            pulse.envelope.constant = (value & 0x10U) != 0U;
            pulse.envelope.period = static_cast<uint8_t>(value & 0x0FU);
        } else if (reg == 1U) {
            pulse.sweep_enabled = (value & 0x80U) != 0U;
            pulse.sweep_period = static_cast<uint8_t>((value >> 4U) & 0x07U);
            pulse.sweep_negate = (value & 0x08U) != 0U;
            pulse.sweep_shift = static_cast<uint8_t>(value & 0x07U);
            pulse.sweep_reload = true;
        } else if (reg == 2U) {
            pulse.timer = static_cast<uint16_t>((pulse.timer & 0x0700U) | value);
        } else {
            pulse.timer = static_cast<uint16_t>((pulse.timer & 0x00FFU) | ((value & 0x07U) << 8U));
            if (pulse.enabled) pulse.length = kLengthTable[value >> 3U];
            pulse.envelope.start = true;
            pulse.phase = 0.0;
        }
        return;
    }

    switch (address) {
        case 0x4008U:
            apu->triangle.control = (value & 0x80U) != 0U;
            apu->triangle.linear_reload_value = static_cast<uint8_t>(value & 0x7FU);
            break;
        case 0x400AU:
            apu->triangle.timer = static_cast<uint16_t>((apu->triangle.timer & 0x0700U) | value);
            break;
        case 0x400BU:
            apu->triangle.timer = static_cast<uint16_t>(
                (apu->triangle.timer & 0x00FFU) | ((value & 0x07U) << 8U));
            if (apu->triangle.enabled) apu->triangle.length = kLengthTable[value >> 3U];
            apu->triangle.linear_reload = true;
            break;
        case 0x400CU:
            apu->noise.envelope.loop = (value & 0x20U) != 0U;
            apu->noise.envelope.constant = (value & 0x10U) != 0U;
            apu->noise.envelope.period = static_cast<uint8_t>(value & 0x0FU);
            break;
        case 0x400EU:
            apu->noise.mode = (value & 0x80U) != 0U;
            apu->noise.period_index = static_cast<uint8_t>(value & 0x0FU);
            break;
        case 0x400FU:
            if (apu->noise.enabled) apu->noise.length = kLengthTable[value >> 3U];
            apu->noise.envelope.start = true;
            break;
        case 0x4010U:
            apu->dmc.loop = (value & 0x40U) != 0U;
            apu->dmc.rate_index = static_cast<uint8_t>(value & 0x0FU);
            break;
        case 0x4011U:
            apu->dmc.output_level = static_cast<uint8_t>(value & 0x7FU);
            break;
        case 0x4012U:
            apu->dmc.sample_address_reg = value;
            break;
        case 0x4013U:
            apu->dmc.sample_length_reg = value;
            break;
        case 0x4015U: {
            apu->status = static_cast<uint8_t>(value & 0x1FU);
            apu->pulse[0].enabled = (value & 0x01U) != 0U;
            apu->pulse[1].enabled = (value & 0x02U) != 0U;
            apu->triangle.enabled = (value & 0x04U) != 0U;
            apu->noise.enabled = (value & 0x08U) != 0U;
            apu->dmc.enabled = (value & 0x10U) != 0U;
            if (!apu->pulse[0].enabled) apu->pulse[0].length = 0U;
            if (!apu->pulse[1].enabled) apu->pulse[1].length = 0U;
            if (!apu->triangle.enabled) apu->triangle.length = 0U;
            if (!apu->noise.enabled) apu->noise.length = 0U;
            if (!apu->dmc.enabled) {
                apu->dmc.bytes_remaining = 0U;
                apu->dmc.bits_remaining = 0U;
                apu->dmc.silence = true;
            } else if (apu->dmc.bytes_remaining == 0U) {
                apu->dmc.current_address = static_cast<uint16_t>(
                    0xC000U + static_cast<uint16_t>(apu->dmc.sample_address_reg) * 64U);
                apu->dmc.bytes_remaining = static_cast<uint16_t>(
                    static_cast<uint16_t>(apu->dmc.sample_length_reg) * 16U + 1U);
            }
            break;
        }
        case 0x4017U:
            apu->frame_counter = value;
            apu->quarter_step = 0U;
            apu->quarter_phase = 0.0;
            if ((value & 0x80U) != 0U) {
                apu_quarter_frame(apu);
                apu_half_frame(apu);
            }
            break;
        default:
            break;
    }
}

static uint8_t apu_status_read(const NesApu *apu)
{
    uint8_t status = 0U;
    if (apu->pulse[0].length > 0U) status |= 0x01U;
    if (apu->pulse[1].length > 0U) status |= 0x02U;
    if (apu->triangle.length > 0U) status |= 0x04U;
    if (apu->noise.length > 0U) status |= 0x08U;
    if (apu->dmc.bytes_remaining > 0U) status |= 0x10U;
    return status;
}

static uint8_t memory_read(NsfSynthImpl *impl, uint16_t address);

static void dmc_restart(DmcChannel *dmc)
{
    dmc->current_address = static_cast<uint16_t>(
        0xC000U + static_cast<uint16_t>(dmc->sample_address_reg) * 64U);
    dmc->bytes_remaining = static_cast<uint16_t>(
        static_cast<uint16_t>(dmc->sample_length_reg) * 16U + 1U);
}

static void dmc_clock(NsfSynthImpl *impl)
{
    DmcChannel &dmc = impl->apu.dmc;
    if (!dmc.enabled) return;

    if (dmc.bits_remaining == 0U) {
        if (dmc.bytes_remaining == 0U) {
            if (dmc.loop) dmc_restart(&dmc);
            else {
                dmc.silence = true;
                return;
            }
        }
        dmc.shift_register = memory_read(impl, dmc.current_address);
        dmc.current_address = dmc.current_address == 0xFFFFU
            ? 0x8000U
            : static_cast<uint16_t>(dmc.current_address + 1U);
        --dmc.bytes_remaining;
        dmc.bits_remaining = 8U;
        dmc.silence = false;
    }

    if (!dmc.silence) {
        if ((dmc.shift_register & 0x01U) != 0U) {
            if (dmc.output_level <= 125U) dmc.output_level = static_cast<uint8_t>(dmc.output_level + 2U);
        } else if (dmc.output_level >= 2U) {
            dmc.output_level = static_cast<uint8_t>(dmc.output_level - 2U);
        }
    }
    dmc.shift_register >>= 1U;
    if (dmc.bits_remaining > 0U) --dmc.bits_remaining;
}

static float pulse_output(PulseChannel *pulse, bool first_channel, uint32_t sample_rate_hz)
{
    if (!pulse->enabled || pulse->length == 0U || pulse->timer < 8U) return 0.0f;
    const uint16_t sweep_target = pulse_sweep_target(*pulse, first_channel);
    if (pulse->sweep_shift > 0U && sweep_target > 0x07FFU) return 0.0f;

    const float frequency = kNtscCpuHz / (16.0f * static_cast<float>(pulse->timer + 1U));
    pulse->phase += frequency / static_cast<float>(sample_rate_hz);
    // timer>=8 时单个 48kHz sample 的相位增量恒小于1，无需每 sample 调 floorf。
    if (pulse->phase >= 1.0f) pulse->phase -= 1.0f;
    const uint8_t step = static_cast<uint8_t>(pulse->phase * 8.0f) & 0x07U;
    if (kPulseDuty[pulse->duty][step] == 0U) return 0.0f;
    return static_cast<float>(envelope_volume(pulse->envelope));
}

static float triangle_output(TriangleChannel *triangle, uint32_t sample_rate_hz)
{
    if (!triangle->enabled || triangle->length == 0U || triangle->linear_counter == 0U ||
        triangle->timer < 2U) {
        return 0.0f;
    }
    const float frequency = kNtscCpuHz / (32.0f * static_cast<float>(triangle->timer + 1U));
    triangle->phase += frequency / static_cast<float>(sample_rate_hz);
    // timer>=2 时单个 sample 的相位增量恒小于1，直接减1比 floorf 更适合 AudioTask 热路径。
    if (triangle->phase >= 1.0f) triangle->phase -= 1.0f;
    const uint8_t step = static_cast<uint8_t>(triangle->phase * 32.0f) & 0x1FU;
    return static_cast<float>(kTriangleSequence[step]);
}

static bool apu_has_audible_activity(const NesApu &apu)
{
    for (uint8_t index = 0U; index < 2U; ++index) {
        const PulseChannel &pulse = apu.pulse[index];
        if (pulse.enabled && pulse.length > 0U && pulse.timer >= 8U &&
            envelope_volume(pulse.envelope) > 0U) {
            const uint16_t sweep_target = pulse_sweep_target(pulse, index == 0U);
            if (pulse.sweep_shift == 0U || sweep_target <= 0x07FFU) return true;
        }
    }

    const TriangleChannel &triangle = apu.triangle;
    if (triangle.enabled && triangle.length > 0U && triangle.linear_counter > 0U &&
        triangle.timer >= 2U) {
        return true;
    }

    const NoiseChannel &noise = apu.noise;
    if (noise.enabled && noise.length > 0U && envelope_volume(noise.envelope) > 0U) {
        return true;
    }

    const DmcChannel &dmc = apu.dmc;
    return dmc.enabled && (!dmc.silence || dmc.bytes_remaining > 0U || dmc.bits_remaining > 0U);
}

static void dmc_fast_advance_analysis(NsfSynthImpl *impl, uint32_t interval_us)
{
    if (impl == nullptr || interval_us == 0U) return;
    DmcChannel &dmc = impl->apu.dmc;
    if (!dmc.enabled) return;

    const float dmc_hz = kNtscCpuHz /
        static_cast<float>(kDmcPeriodNtsc[dmc.rate_index]);
    dmc.phase += dmc_hz * static_cast<float>(interval_us) / 1000000.0f;
    const uint32_t clocks = static_cast<uint32_t>(dmc.phase);
    dmc.phase -= static_cast<float>(clocks);
    if (clocks == 0U) return;

    uint64_t remaining_bits =
        static_cast<uint64_t>(dmc.bytes_remaining) * 8ULL + dmc.bits_remaining;
    const uint64_t sample_bits =
        (static_cast<uint64_t>(dmc.sample_length_reg) * 16ULL + 1ULL) * 8ULL;
    if (remaining_bits == 0ULL && dmc.loop) remaining_bits = sample_bits;

    if (dmc.loop && sample_bits > 0ULL) {
        uint64_t consume = clocks;
        if (consume >= remaining_bits) {
            consume -= remaining_bits;
            consume %= sample_bits;
            remaining_bits = sample_bits - consume;
            if (remaining_bits == 0ULL) remaining_bits = sample_bits;
        } else {
            remaining_bits -= consume;
        }
    } else {
        remaining_bits = clocks >= remaining_bits ? 0ULL : remaining_bits - clocks;
    }

    dmc.bytes_remaining = static_cast<uint16_t>(remaining_bits / 8ULL);
    dmc.bits_remaining = static_cast<uint8_t>(remaining_bits % 8ULL);
    dmc.silence = remaining_bits == 0ULL;
}

static void apu_fast_advance_analysis_interval(NsfSynthImpl *impl)
{
    if (impl == nullptr || impl->play_speed_us == 0U) return;
    apu_advance_frame_sequencer(
        &impl->apu,
        240.0f * static_cast<float>(impl->play_speed_us) / 1000000.0f);
    dmc_fast_advance_analysis(impl, impl->play_speed_us);
}

static bool pulse_has_audible_activity(const PulseChannel &pulse, bool first_channel)
{
    if (!pulse.enabled || pulse.length == 0U || pulse.timer < 8U ||
        envelope_volume(pulse.envelope) == 0U) {
        return false;
    }
    const uint16_t sweep_target = pulse_sweep_target(pulse, first_channel);
    return pulse.sweep_shift == 0U || sweep_target <= 0x07FFU;
}

static uint8_t frequency_to_midi_note(float frequency_hz)
{
    if (frequency_hz <= 0.0f) return 0U;
    const float note_f = 69.0f + 12.0f * log2f(frequency_hz / 440.0f);
    int32_t note = static_cast<int32_t>(note_f + (note_f >= 0.0f ? 0.5f : -0.5f));
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return static_cast<uint8_t>(note);
}

static uint8_t visual_pitch_from_timer(
    NsfSynthImpl *impl,
    uint8_t voice,
    uint16_t timer,
    float divider)
{
    if (impl == nullptr || voice >= 3U) return 0U;
    const uint8_t bit = static_cast<uint8_t>(1U << voice);
    if ((impl->visual_pitch_valid_mask & bit) != 0U &&
        impl->visual_pitch_timer[voice] == timer) {
        return impl->visual_pitch_cache[voice];
    }

    const float hz = kNtscCpuHz /
        (divider * static_cast<float>(static_cast<uint32_t>(timer) + 1U));
    const uint8_t note = frequency_to_midi_note(hz);
    impl->visual_pitch_timer[voice] = timer;
    impl->visual_pitch_cache[voice] = note;
    impl->visual_pitch_valid_mask |= bit;
    return note;
}

static void capture_visual_tick(NsfSynth *synth)
{
    if (synth == nullptr || synth->impl == nullptr) return;
    NsfSynthImpl *impl = static_cast<NsfSynthImpl *>(synth->impl);
    if (impl->visual_ticks == nullptr) return;

    NsfSynthVisualTick tick = {};
    tick.frame = synth->position_frames > UINT32_MAX
        ? UINT32_MAX
        : static_cast<uint32_t>(synth->position_frames);

    for (uint8_t index = 0U; index < 2U; ++index) {
        const PulseChannel &pulse = impl->apu.pulse[index];
        if (pulse_has_audible_activity(pulse, index == 0U)) {
            tick.active_mask |= static_cast<uint8_t>(1U << index);
            tick.pitch[index] = visual_pitch_from_timer(impl, index, pulse.timer, 16.0f);
            tick.level[index] = static_cast<uint8_t>(envelope_volume(pulse.envelope) * 8U);
        }
    }

    const TriangleChannel &triangle = impl->apu.triangle;
    if (triangle.enabled && triangle.length > 0U && triangle.linear_counter > 0U &&
        triangle.timer >= 2U) {
        tick.active_mask |= 1U << 2U;
        tick.pitch[2] = visual_pitch_from_timer(impl, 2U, triangle.timer, 32.0f);
        tick.level[2] = 96U;
    }

    const NoiseChannel &noise = impl->apu.noise;
    if (noise.enabled && noise.length > 0U && envelope_volume(noise.envelope) > 0U) {
        tick.active_mask |= 1U << 3U;
        tick.level[3] = static_cast<uint8_t>(envelope_volume(noise.envelope) * 8U);
    }

    const DmcChannel &dmc = impl->apu.dmc;
    if (dmc.enabled && (!dmc.silence || dmc.bytes_remaining > 0U || dmc.bits_remaining > 0U)) {
        tick.active_mask |= 1U << 4U;
        tick.level[4] = dmc.output_level;
    }

    if ((impl->play_write_mask & (1UL << 0x03U)) != 0U) tick.trigger_mask |= 1U << 0U;
    if ((impl->play_write_mask & (1UL << 0x07U)) != 0U) tick.trigger_mask |= 1U << 1U;
    if ((impl->play_write_mask & (1UL << 0x0BU)) != 0U) tick.trigger_mask |= 1U << 2U;
    if ((impl->play_write_mask & (1UL << 0x0FU)) != 0U) tick.trigger_mask |= 1U << 3U;
    if ((impl->play_write_mask & (1UL << 0x15U)) != 0U) tick.trigger_mask |= 1U << 4U;

    if (impl->visual_tick_count == kVisualTickCapacity) {
        impl->visual_tick_read = (impl->visual_tick_read + 1U) % kVisualTickCapacity;
        --impl->visual_tick_count;
    }
    const size_t write =
        (impl->visual_tick_read + impl->visual_tick_count) % kVisualTickCapacity;
    impl->visual_ticks[write] = tick;
    ++impl->visual_tick_count;
}

static float noise_output(NoiseChannel *noise, uint32_t sample_rate_hz)
{
    if (!noise->enabled || noise->length == 0U) return 0.0f;
    const float tick_hz = kNtscCpuHz /
        static_cast<float>(kNoisePeriodNtsc[noise->period_index]);
    noise->phase += tick_hz / static_cast<float>(sample_rate_hz);
    while (noise->phase >= 1.0) {
        noise->phase -= 1.0;
        const uint8_t tap = noise->mode ? 6U : 1U;
        const uint16_t feedback = static_cast<uint16_t>(
            (noise->lfsr & 0x01U) ^ ((noise->lfsr >> tap) & 0x01U));
        noise->lfsr = static_cast<uint16_t>((noise->lfsr >> 1U) | (feedback << 14U));
        if (noise->lfsr == 0U) noise->lfsr = 1U;
    }
    return (noise->lfsr & 0x01U) == 0U
        ? static_cast<float>(envelope_volume(noise->envelope))
        : 0.0f;
}

static float apu_sample(NsfSynthImpl *impl)
{
    NesApu &apu = impl->apu;
    const uint32_t rate = impl->sample_rate_hz;

    // Frame Counter 的基础事件间隔约为 240Hz；共享推进器也供 PLAY-driven 分析路径使用。
    apu_advance_frame_sequencer(&apu, 240.0f / static_cast<float>(rate));

    DmcChannel &dmc = apu.dmc;
    if (dmc.enabled) {
        const float dmc_hz = kNtscCpuHz /
            static_cast<float>(kDmcPeriodNtsc[dmc.rate_index]);
        dmc.phase += dmc_hz / static_cast<float>(rate);
        while (dmc.phase >= 1.0) {
            dmc.phase -= 1.0;
            dmc_clock(impl);
        }
    }

    const float p1 = pulse_output(&apu.pulse[0], true, rate);
    const float p2 = pulse_output(&apu.pulse[1], false, rate);
    const float tri = triangle_output(&apu.triangle, rate);
    const float noise = noise_output(&apu.noise, rate);
    const float dmc_level = static_cast<float>(apu.dmc.output_level);

    float pulse_mix = 0.0f;
    if (p1 + p2 > 0.0f) pulse_mix = 95.88f / (8128.0f / (p1 + p2) + 100.0f);

    const float tnd_term = tri / 8227.0f + noise / 12241.0f + dmc_level / 22638.0f;
    const float tnd_mix = tnd_term > 0.0f
        ? 159.79f / (1.0f / tnd_term + 100.0f)
        : 0.0f;

    // NES DAC 为单极性输出；先做简单高通去直流，再低通压住廉价离散仿真的超高频毛刺。
    const float raw = (pulse_mix + tnd_mix) * 1.16f;
    const float hp = impl->hp_alpha * (apu.hp_prev_out + raw - apu.hp_prev_in);
    apu.hp_prev_in = raw;
    apu.hp_prev_out = hp;

    apu.lp_state += impl->lp_alpha * (hp - apu.lp_state);
    return apu.lp_state;
}

static int32_t float_to_pcm32(float value)
{
    // NSF 合成平均响度低于常规母带音乐；在最终 PCM 统一补约 +2.1dB，峰值继续由下方限幅保护。
    // 用户音量仍完全由 CS43131 控制，不改变全局音量曲线。
    value *= 1.15f;
    if (value > 0.999f) value = 0.999f;
    if (value < -0.999f) value = -0.999f;
    const int32_t sample16 = static_cast<int32_t>(value * 32767.0f);
    return sample16 * 65536;
}

static uint8_t banked_prg_read(const NsfSynthImpl *impl, uint16_t address)
{
    if (address < 0x8000U || impl->prg == nullptr || impl->prg_size == 0U) return 0U;
    const uint8_t slot = static_cast<uint8_t>((address - 0x8000U) >> 12U);
    const size_t padded_offset =
        static_cast<size_t>(impl->bank[slot]) * 4096U + (address & 0x0FFFU);
    if (padded_offset < impl->load_padding) return 0U;
    const size_t prg_offset = padded_offset - impl->load_padding;
    return prg_offset < impl->prg_size ? impl->prg[prg_offset] : 0U;
}

static uint8_t memory_read(NsfSynthImpl *impl, uint16_t address)
{
    if (address < 0x2000U) return impl->ram[address & 0x07FFU];
    if (address == 0x4015U) return apu_status_read(&impl->apu);
    if (address >= 0x4000U && address <= 0x4017U) return 0U;
    if (address >= 0x6000U && address < 0x8000U) {
        return impl->work_ram[address - 0x6000U];
    }
    if (address >= 0x8000U) {
        if (impl->uses_banking) return banked_prg_read(impl, address);
        if (address < impl->config.load_address) return 0U;
        const size_t offset = static_cast<size_t>(address - impl->config.load_address);
        return offset < impl->prg_size ? impl->prg[offset] : 0U;
    }
    return 0U;
}

static void memory_write(NsfSynthImpl *impl, uint16_t address, uint8_t value)
{
    if (address < 0x2000U) {
        impl->ram[address & 0x07FFU] = value;
        return;
    }
    if (address >= 0x4000U && address <= 0x4017U) {
        const bool track_write =
            impl->config.enable_loop_detection || impl->config.enable_visual_capture;
        if (track_write && is_loop_audio_register(address)) {
            const uint8_t reg = static_cast<uint8_t>(address - 0x4000U);
            if (impl->config.enable_loop_detection) {
                impl->apu_registers[reg] = value;
            }
            if (is_loop_trigger_register(address)) {
                impl->play_write_mask |= 1UL << reg;
            }
        }
        apu_write(&impl->apu, address, value);
        return;
    }
    if (address >= 0x5FF8U && address <= 0x5FFFU && impl->uses_banking) {
        const uint8_t slot = static_cast<uint8_t>(address - 0x5FF8U);
        impl->bank[slot] = value;
        return;
    }
    if (address >= 0x6000U && address < 0x8000U) {
        impl->work_ram[address - 0x6000U] = value;
    }
}

static uint8_t cpu_read(NsfSynthImpl *impl, uint16_t address)
{
    return memory_read(impl, address);
}

static void cpu_write(NsfSynthImpl *impl, uint16_t address, uint8_t value)
{
    memory_write(impl, address, value);
}

static uint8_t fetch8(NsfSynthImpl *impl)
{
    return cpu_read(impl, impl->cpu.pc++);
}

static uint16_t fetch16(NsfSynthImpl *impl)
{
    const uint8_t lo = fetch8(impl);
    const uint8_t hi = fetch8(impl);
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
}

static uint16_t read16_zp(NsfSynthImpl *impl, uint8_t zp)
{
    const uint8_t lo = cpu_read(impl, zp);
    const uint8_t hi = cpu_read(impl, static_cast<uint8_t>(zp + 1U));
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8U));
}

static void push8(NsfSynthImpl *impl, uint8_t value)
{
    cpu_write(impl, static_cast<uint16_t>(0x0100U | impl->cpu.sp), value);
    --impl->cpu.sp;
}

static uint8_t pop8(NsfSynthImpl *impl)
{
    ++impl->cpu.sp;
    return cpu_read(impl, static_cast<uint16_t>(0x0100U | impl->cpu.sp));
}

static uint16_t addr_zp(NsfSynthImpl *impl) { return fetch8(impl); }
static uint16_t addr_zpx(NsfSynthImpl *impl) { return static_cast<uint8_t>(fetch8(impl) + impl->cpu.x); }
static uint16_t addr_zpy(NsfSynthImpl *impl) { return static_cast<uint8_t>(fetch8(impl) + impl->cpu.y); }
static uint16_t addr_abs(NsfSynthImpl *impl) { return fetch16(impl); }
static uint16_t addr_absx(NsfSynthImpl *impl) { return static_cast<uint16_t>(fetch16(impl) + impl->cpu.x); }
static uint16_t addr_absy(NsfSynthImpl *impl) { return static_cast<uint16_t>(fetch16(impl) + impl->cpu.y); }
static uint16_t addr_indx(NsfSynthImpl *impl)
{
    const uint8_t zp = static_cast<uint8_t>(fetch8(impl) + impl->cpu.x);
    return read16_zp(impl, zp);
}
static uint16_t addr_indy(NsfSynthImpl *impl)
{
    const uint8_t zp = fetch8(impl);
    return static_cast<uint16_t>(read16_zp(impl, zp) + impl->cpu.y);
}

static void op_adc(Cpu6502 *cpu, uint8_t value)
{
    const uint16_t sum = static_cast<uint16_t>(cpu->a) + value + (get_flag(cpu, FlagC) ? 1U : 0U);
    const uint8_t result = static_cast<uint8_t>(sum);
    set_flag(cpu, FlagC, sum > 0xFFU);
    set_flag(cpu, FlagV, ((~(cpu->a ^ value) & (cpu->a ^ result)) & 0x80U) != 0U);
    cpu->a = result;
    set_nz(cpu, cpu->a);
}

static void op_sbc(Cpu6502 *cpu, uint8_t value)
{
    op_adc(cpu, static_cast<uint8_t>(~value));
}

static void op_cmp(Cpu6502 *cpu, uint8_t reg, uint8_t value)
{
    const uint8_t result = static_cast<uint8_t>(reg - value);
    set_flag(cpu, FlagC, reg >= value);
    set_nz(cpu, result);
}

static uint8_t op_asl(Cpu6502 *cpu, uint8_t value)
{
    set_flag(cpu, FlagC, (value & 0x80U) != 0U);
    value = static_cast<uint8_t>(value << 1U);
    set_nz(cpu, value);
    return value;
}

static uint8_t op_lsr(Cpu6502 *cpu, uint8_t value)
{
    set_flag(cpu, FlagC, (value & 0x01U) != 0U);
    value >>= 1U;
    set_nz(cpu, value);
    return value;
}

static uint8_t op_rol(Cpu6502 *cpu, uint8_t value)
{
    const bool carry = get_flag(cpu, FlagC);
    set_flag(cpu, FlagC, (value & 0x80U) != 0U);
    value = static_cast<uint8_t>((value << 1U) | (carry ? 1U : 0U));
    set_nz(cpu, value);
    return value;
}

static uint8_t op_ror(Cpu6502 *cpu, uint8_t value)
{
    const bool carry = get_flag(cpu, FlagC);
    set_flag(cpu, FlagC, (value & 0x01U) != 0U);
    value = static_cast<uint8_t>((value >> 1U) | (carry ? 0x80U : 0U));
    set_nz(cpu, value);
    return value;
}

static void branch(NsfSynthImpl *impl, bool take)
{
    const int8_t offset = static_cast<int8_t>(fetch8(impl));
    if (take) impl->cpu.pc = static_cast<uint16_t>(impl->cpu.pc + offset);
}

static void illegal_nop(NsfSynthImpl *impl, uint8_t opcode)
{
    switch (opcode) {
        case 0x04: case 0x44: case 0x64:
        case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4:
        case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:
            (void)fetch8(impl);
            break;
        case 0x0C:
        case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
            (void)fetch16(impl);
            break;
        default:
            break;
    }
}

static bool execute_instruction(NsfSynthImpl *impl)
{
    Cpu6502 &c = impl->cpu;
    const uint8_t opcode = fetch8(impl);
    uint16_t a = 0U;
    uint8_t v = 0U;

#define RD(addr) cpu_read(impl, (addr))
#define WR(addr, val) cpu_write(impl, (addr), (val))
#define LOAD_A(addr) do { c.a = RD(addr); set_nz(&c, c.a); } while (0)
#define LOAD_X(addr) do { c.x = RD(addr); set_nz(&c, c.x); } while (0)
#define LOAD_Y(addr) do { c.y = RD(addr); set_nz(&c, c.y); } while (0)
#define ORA(addr) do { c.a = static_cast<uint8_t>(c.a | RD(addr)); set_nz(&c, c.a); } while (0)
#define AND(addr) do { c.a = static_cast<uint8_t>(c.a & RD(addr)); set_nz(&c, c.a); } while (0)
#define EOR(addr) do { c.a = static_cast<uint8_t>(c.a ^ RD(addr)); set_nz(&c, c.a); } while (0)
#define ADC(addr) op_adc(&c, RD(addr))
#define SBC(addr) op_sbc(&c, RD(addr))
#define CMP_R(reg, addr) op_cmp(&c, (reg), RD(addr))
#define RMW(addr, fn) do { uint8_t _v = RD(addr); _v = fn(&c, _v); WR(addr, _v); } while (0)

    switch (opcode) {
        // ORA
        case 0x09: c.a |= fetch8(impl); set_nz(&c, c.a); break;
        case 0x05: ORA(addr_zp(impl)); break; case 0x15: ORA(addr_zpx(impl)); break;
        case 0x0D: ORA(addr_abs(impl)); break; case 0x1D: ORA(addr_absx(impl)); break;
        case 0x19: ORA(addr_absy(impl)); break; case 0x01: ORA(addr_indx(impl)); break;
        case 0x11: ORA(addr_indy(impl)); break;
        // AND
        case 0x29: c.a &= fetch8(impl); set_nz(&c, c.a); break;
        case 0x25: AND(addr_zp(impl)); break; case 0x35: AND(addr_zpx(impl)); break;
        case 0x2D: AND(addr_abs(impl)); break; case 0x3D: AND(addr_absx(impl)); break;
        case 0x39: AND(addr_absy(impl)); break; case 0x21: AND(addr_indx(impl)); break;
        case 0x31: AND(addr_indy(impl)); break;
        // EOR
        case 0x49: c.a ^= fetch8(impl); set_nz(&c, c.a); break;
        case 0x45: EOR(addr_zp(impl)); break; case 0x55: EOR(addr_zpx(impl)); break;
        case 0x4D: EOR(addr_abs(impl)); break; case 0x5D: EOR(addr_absx(impl)); break;
        case 0x59: EOR(addr_absy(impl)); break; case 0x41: EOR(addr_indx(impl)); break;
        case 0x51: EOR(addr_indy(impl)); break;
        // ADC
        case 0x69: op_adc(&c, fetch8(impl)); break;
        case 0x65: ADC(addr_zp(impl)); break; case 0x75: ADC(addr_zpx(impl)); break;
        case 0x6D: ADC(addr_abs(impl)); break; case 0x7D: ADC(addr_absx(impl)); break;
        case 0x79: ADC(addr_absy(impl)); break; case 0x61: ADC(addr_indx(impl)); break;
        case 0x71: ADC(addr_indy(impl)); break;
        // SBC
        case 0xE9: case 0xEB: op_sbc(&c, fetch8(impl)); break;
        case 0xE5: SBC(addr_zp(impl)); break; case 0xF5: SBC(addr_zpx(impl)); break;
        case 0xED: SBC(addr_abs(impl)); break; case 0xFD: SBC(addr_absx(impl)); break;
        case 0xF9: SBC(addr_absy(impl)); break; case 0xE1: SBC(addr_indx(impl)); break;
        case 0xF1: SBC(addr_indy(impl)); break;

        // Loads
        case 0xA9: c.a = fetch8(impl); set_nz(&c, c.a); break;
        case 0xA5: LOAD_A(addr_zp(impl)); break; case 0xB5: LOAD_A(addr_zpx(impl)); break;
        case 0xAD: LOAD_A(addr_abs(impl)); break; case 0xBD: LOAD_A(addr_absx(impl)); break;
        case 0xB9: LOAD_A(addr_absy(impl)); break; case 0xA1: LOAD_A(addr_indx(impl)); break;
        case 0xB1: LOAD_A(addr_indy(impl)); break;
        case 0xA2: c.x = fetch8(impl); set_nz(&c, c.x); break;
        case 0xA6: LOAD_X(addr_zp(impl)); break; case 0xB6: LOAD_X(addr_zpy(impl)); break;
        case 0xAE: LOAD_X(addr_abs(impl)); break; case 0xBE: LOAD_X(addr_absy(impl)); break;
        case 0xA0: c.y = fetch8(impl); set_nz(&c, c.y); break;
        case 0xA4: LOAD_Y(addr_zp(impl)); break; case 0xB4: LOAD_Y(addr_zpx(impl)); break;
        case 0xAC: LOAD_Y(addr_abs(impl)); break; case 0xBC: LOAD_Y(addr_absx(impl)); break;
        // Stores
        case 0x85: WR(addr_zp(impl), c.a); break; case 0x95: WR(addr_zpx(impl), c.a); break;
        case 0x8D: WR(addr_abs(impl), c.a); break; case 0x9D: WR(addr_absx(impl), c.a); break;
        case 0x99: WR(addr_absy(impl), c.a); break; case 0x81: WR(addr_indx(impl), c.a); break;
        case 0x91: WR(addr_indy(impl), c.a); break;
        case 0x86: WR(addr_zp(impl), c.x); break; case 0x96: WR(addr_zpy(impl), c.x); break;
        case 0x8E: WR(addr_abs(impl), c.x); break;
        case 0x84: WR(addr_zp(impl), c.y); break; case 0x94: WR(addr_zpx(impl), c.y); break;
        case 0x8C: WR(addr_abs(impl), c.y); break;

        // Compare
        case 0xC9: op_cmp(&c, c.a, fetch8(impl)); break;
        case 0xC5: CMP_R(c.a, addr_zp(impl)); break; case 0xD5: CMP_R(c.a, addr_zpx(impl)); break;
        case 0xCD: CMP_R(c.a, addr_abs(impl)); break; case 0xDD: CMP_R(c.a, addr_absx(impl)); break;
        case 0xD9: CMP_R(c.a, addr_absy(impl)); break; case 0xC1: CMP_R(c.a, addr_indx(impl)); break;
        case 0xD1: CMP_R(c.a, addr_indy(impl)); break;
        case 0xE0: op_cmp(&c, c.x, fetch8(impl)); break;
        case 0xE4: CMP_R(c.x, addr_zp(impl)); break; case 0xEC: CMP_R(c.x, addr_abs(impl)); break;
        case 0xC0: op_cmp(&c, c.y, fetch8(impl)); break;
        case 0xC4: CMP_R(c.y, addr_zp(impl)); break; case 0xCC: CMP_R(c.y, addr_abs(impl)); break;

        // BIT
        case 0x24: a = addr_zp(impl); v = RD(a); set_flag(&c, FlagZ, (c.a & v) == 0U); set_flag(&c, FlagN, (v & 0x80U) != 0U); set_flag(&c, FlagV, (v & 0x40U) != 0U); break;
        case 0x2C: a = addr_abs(impl); v = RD(a); set_flag(&c, FlagZ, (c.a & v) == 0U); set_flag(&c, FlagN, (v & 0x80U) != 0U); set_flag(&c, FlagV, (v & 0x40U) != 0U); break;

        // INC/DEC
        case 0xE6: a=addr_zp(impl); v=static_cast<uint8_t>(RD(a)+1U); WR(a,v); set_nz(&c,v); break;
        case 0xF6: a=addr_zpx(impl); v=static_cast<uint8_t>(RD(a)+1U); WR(a,v); set_nz(&c,v); break;
        case 0xEE: a=addr_abs(impl); v=static_cast<uint8_t>(RD(a)+1U); WR(a,v); set_nz(&c,v); break;
        case 0xFE: a=addr_absx(impl); v=static_cast<uint8_t>(RD(a)+1U); WR(a,v); set_nz(&c,v); break;
        case 0xC6: a=addr_zp(impl); v=static_cast<uint8_t>(RD(a)-1U); WR(a,v); set_nz(&c,v); break;
        case 0xD6: a=addr_zpx(impl); v=static_cast<uint8_t>(RD(a)-1U); WR(a,v); set_nz(&c,v); break;
        case 0xCE: a=addr_abs(impl); v=static_cast<uint8_t>(RD(a)-1U); WR(a,v); set_nz(&c,v); break;
        case 0xDE: a=addr_absx(impl); v=static_cast<uint8_t>(RD(a)-1U); WR(a,v); set_nz(&c,v); break;

        // Shifts / rotates
        case 0x0A: c.a=op_asl(&c,c.a); break;
        case 0x06: a=addr_zp(impl); RMW(a, op_asl); break; case 0x16: a=addr_zpx(impl); RMW(a, op_asl); break;
        case 0x0E: a=addr_abs(impl); RMW(a, op_asl); break; case 0x1E: a=addr_absx(impl); RMW(a, op_asl); break;
        case 0x4A: c.a=op_lsr(&c,c.a); break;
        case 0x46: a=addr_zp(impl); RMW(a, op_lsr); break; case 0x56: a=addr_zpx(impl); RMW(a, op_lsr); break;
        case 0x4E: a=addr_abs(impl); RMW(a, op_lsr); break; case 0x5E: a=addr_absx(impl); RMW(a, op_lsr); break;
        case 0x2A: c.a=op_rol(&c,c.a); break;
        case 0x26: a=addr_zp(impl); RMW(a, op_rol); break; case 0x36: a=addr_zpx(impl); RMW(a, op_rol); break;
        case 0x2E: a=addr_abs(impl); RMW(a, op_rol); break; case 0x3E: a=addr_absx(impl); RMW(a, op_rol); break;
        case 0x6A: c.a=op_ror(&c,c.a); break;
        case 0x66: a=addr_zp(impl); RMW(a, op_ror); break; case 0x76: a=addr_zpx(impl); RMW(a, op_ror); break;
        case 0x6E: a=addr_abs(impl); RMW(a, op_ror); break; case 0x7E: a=addr_absx(impl); RMW(a, op_ror); break;

        // Transfers / increments
        case 0xAA: c.x=c.a; set_nz(&c,c.x); break; case 0xA8: c.y=c.a; set_nz(&c,c.y); break;
        case 0x8A: c.a=c.x; set_nz(&c,c.a); break; case 0x98: c.a=c.y; set_nz(&c,c.a); break;
        case 0xBA: c.x=c.sp; set_nz(&c,c.x); break; case 0x9A: c.sp=c.x; break;
        case 0xE8: ++c.x; set_nz(&c,c.x); break; case 0xC8: ++c.y; set_nz(&c,c.y); break;
        case 0xCA: --c.x; set_nz(&c,c.x); break; case 0x88: --c.y; set_nz(&c,c.y); break;

        // Stack
        case 0x48: push8(impl,c.a); break;
        case 0x08: push8(impl, static_cast<uint8_t>(c.p | FlagB | FlagU)); break;
        case 0x68: c.a=pop8(impl); set_nz(&c,c.a); break;
        case 0x28: c.p=static_cast<uint8_t>((pop8(impl) & ~FlagB) | FlagU); break;

        // Jumps / calls
        case 0x4C: c.pc=fetch16(impl); break;
        case 0x6C: {
            const uint16_t ptr=fetch16(impl);
            const uint8_t lo=RD(ptr);
            const uint16_t hi_addr=static_cast<uint16_t>((ptr & 0xFF00U) | ((ptr + 1U) & 0x00FFU));
            c.pc=static_cast<uint16_t>(lo | (static_cast<uint16_t>(RD(hi_addr)) << 8U));
            break;
        }
        case 0x20: {
            const uint16_t target=fetch16(impl);
            const uint16_t ret=static_cast<uint16_t>(c.pc-1U);
            push8(impl, static_cast<uint8_t>(ret >> 8U)); push8(impl, static_cast<uint8_t>(ret));
            c.pc=target; break;
        }
        case 0x60: { const uint8_t lo=pop8(impl); const uint8_t hi=pop8(impl); c.pc=static_cast<uint16_t>((lo | (static_cast<uint16_t>(hi)<<8U))+1U); break; }
        case 0x40: { c.p=static_cast<uint8_t>((pop8(impl)&~FlagB)|FlagU); const uint8_t lo=pop8(impl); const uint8_t hi=pop8(impl); c.pc=static_cast<uint16_t>(lo|(static_cast<uint16_t>(hi)<<8U)); break; }

        // Branches
        case 0x10: branch(impl,!get_flag(&c,FlagN)); break; case 0x30: branch(impl,get_flag(&c,FlagN)); break;
        case 0x50: branch(impl,!get_flag(&c,FlagV)); break; case 0x70: branch(impl,get_flag(&c,FlagV)); break;
        case 0x90: branch(impl,!get_flag(&c,FlagC)); break; case 0xB0: branch(impl,get_flag(&c,FlagC)); break;
        case 0xD0: branch(impl,!get_flag(&c,FlagZ)); break; case 0xF0: branch(impl,get_flag(&c,FlagZ)); break;

        // Flags
        case 0x18: set_flag(&c,FlagC,false); break; case 0x38: set_flag(&c,FlagC,true); break;
        case 0x58: set_flag(&c,FlagI,false); break; case 0x78: set_flag(&c,FlagI,true); break;
        case 0xB8: set_flag(&c,FlagV,false); break; case 0xD8: set_flag(&c,FlagD,false); break;
        case 0xF8: set_flag(&c,FlagD,true); break;

        // NOP
        case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
            break;

        // 常见 undocumented LAX / SAX。部分老 NSF 驱动会使用，低成本支持可显著提高兼容性。
        case 0xA7: a=addr_zp(impl); c.a=c.x=RD(a); set_nz(&c,c.a); break;
        case 0xB7: a=addr_zpy(impl); c.a=c.x=RD(a); set_nz(&c,c.a); break;
        case 0xAF: a=addr_abs(impl); c.a=c.x=RD(a); set_nz(&c,c.a); break;
        case 0xBF: a=addr_absy(impl); c.a=c.x=RD(a); set_nz(&c,c.a); break;
        case 0xA3: a=addr_indx(impl); c.a=c.x=RD(a); set_nz(&c,c.a); break;
        case 0xB3: a=addr_indy(impl); c.a=c.x=RD(a); set_nz(&c,c.a); break;
        case 0x87: WR(addr_zp(impl), static_cast<uint8_t>(c.a & c.x)); break;
        case 0x97: WR(addr_zpy(impl), static_cast<uint8_t>(c.a & c.x)); break;
        case 0x8F: WR(addr_abs(impl), static_cast<uint8_t>(c.a & c.x)); break;
        case 0x83: WR(addr_indx(impl), static_cast<uint8_t>(c.a & c.x)); break;

        // 常见 undocumented RMW 组合。
        case 0x07: a=addr_zp(impl); v=op_asl(&c,RD(a)); WR(a,v); c.a|=v; set_nz(&c,c.a); break;
        case 0x17: a=addr_zpx(impl); v=op_asl(&c,RD(a)); WR(a,v); c.a|=v; set_nz(&c,c.a); break;
        case 0x0F: a=addr_abs(impl); v=op_asl(&c,RD(a)); WR(a,v); c.a|=v; set_nz(&c,c.a); break;
        case 0x1F: a=addr_absx(impl); v=op_asl(&c,RD(a)); WR(a,v); c.a|=v; set_nz(&c,c.a); break;
        case 0x1B: a=addr_absy(impl); v=op_asl(&c,RD(a)); WR(a,v); c.a|=v; set_nz(&c,c.a); break;
        case 0x03: a=addr_indx(impl); v=op_asl(&c,RD(a)); WR(a,v); c.a|=v; set_nz(&c,c.a); break;
        case 0x13: a=addr_indy(impl); v=op_asl(&c,RD(a)); WR(a,v); c.a|=v; set_nz(&c,c.a); break;
        case 0x27: a=addr_zp(impl); v=op_rol(&c,RD(a)); WR(a,v); c.a&=v; set_nz(&c,c.a); break;
        case 0x37: a=addr_zpx(impl); v=op_rol(&c,RD(a)); WR(a,v); c.a&=v; set_nz(&c,c.a); break;
        case 0x2F: a=addr_abs(impl); v=op_rol(&c,RD(a)); WR(a,v); c.a&=v; set_nz(&c,c.a); break;
        case 0x3F: a=addr_absx(impl); v=op_rol(&c,RD(a)); WR(a,v); c.a&=v; set_nz(&c,c.a); break;
        case 0x3B: a=addr_absy(impl); v=op_rol(&c,RD(a)); WR(a,v); c.a&=v; set_nz(&c,c.a); break;
        case 0x23: a=addr_indx(impl); v=op_rol(&c,RD(a)); WR(a,v); c.a&=v; set_nz(&c,c.a); break;
        case 0x33: a=addr_indy(impl); v=op_rol(&c,RD(a)); WR(a,v); c.a&=v; set_nz(&c,c.a); break;
        case 0x47: a=addr_zp(impl); v=op_lsr(&c,RD(a)); WR(a,v); c.a^=v; set_nz(&c,c.a); break;
        case 0x57: a=addr_zpx(impl); v=op_lsr(&c,RD(a)); WR(a,v); c.a^=v; set_nz(&c,c.a); break;
        case 0x4F: a=addr_abs(impl); v=op_lsr(&c,RD(a)); WR(a,v); c.a^=v; set_nz(&c,c.a); break;
        case 0x5F: a=addr_absx(impl); v=op_lsr(&c,RD(a)); WR(a,v); c.a^=v; set_nz(&c,c.a); break;
        case 0x5B: a=addr_absy(impl); v=op_lsr(&c,RD(a)); WR(a,v); c.a^=v; set_nz(&c,c.a); break;
        case 0x43: a=addr_indx(impl); v=op_lsr(&c,RD(a)); WR(a,v); c.a^=v; set_nz(&c,c.a); break;
        case 0x53: a=addr_indy(impl); v=op_lsr(&c,RD(a)); WR(a,v); c.a^=v; set_nz(&c,c.a); break;
        case 0x67: a=addr_zp(impl); v=op_ror(&c,RD(a)); WR(a,v); op_adc(&c,v); break;
        case 0x77: a=addr_zpx(impl); v=op_ror(&c,RD(a)); WR(a,v); op_adc(&c,v); break;
        case 0x6F: a=addr_abs(impl); v=op_ror(&c,RD(a)); WR(a,v); op_adc(&c,v); break;
        case 0x7F: a=addr_absx(impl); v=op_ror(&c,RD(a)); WR(a,v); op_adc(&c,v); break;
        case 0x7B: a=addr_absy(impl); v=op_ror(&c,RD(a)); WR(a,v); op_adc(&c,v); break;
        case 0x63: a=addr_indx(impl); v=op_ror(&c,RD(a)); WR(a,v); op_adc(&c,v); break;
        case 0x73: a=addr_indy(impl); v=op_ror(&c,RD(a)); WR(a,v); op_adc(&c,v); break;
        case 0xC7: a=addr_zp(impl); v=static_cast<uint8_t>(RD(a)-1U); WR(a,v); op_cmp(&c,c.a,v); break;
        case 0xD7: a=addr_zpx(impl); v=static_cast<uint8_t>(RD(a)-1U); WR(a,v); op_cmp(&c,c.a,v); break;
        case 0xCF: a=addr_abs(impl); v=static_cast<uint8_t>(RD(a)-1U); WR(a,v); op_cmp(&c,c.a,v); break;
        case 0xDF: a=addr_absx(impl); v=static_cast<uint8_t>(RD(a)-1U); WR(a,v); op_cmp(&c,c.a,v); break;
        case 0xDB: a=addr_absy(impl); v=static_cast<uint8_t>(RD(a)-1U); WR(a,v); op_cmp(&c,c.a,v); break;
        case 0xC3: a=addr_indx(impl); v=static_cast<uint8_t>(RD(a)-1U); WR(a,v); op_cmp(&c,c.a,v); break;
        case 0xD3: a=addr_indy(impl); v=static_cast<uint8_t>(RD(a)-1U); WR(a,v); op_cmp(&c,c.a,v); break;
        case 0xE7: a=addr_zp(impl); v=static_cast<uint8_t>(RD(a)+1U); WR(a,v); op_sbc(&c,v); break;
        case 0xF7: a=addr_zpx(impl); v=static_cast<uint8_t>(RD(a)+1U); WR(a,v); op_sbc(&c,v); break;
        case 0xEF: a=addr_abs(impl); v=static_cast<uint8_t>(RD(a)+1U); WR(a,v); op_sbc(&c,v); break;
        case 0xFF: a=addr_absx(impl); v=static_cast<uint8_t>(RD(a)+1U); WR(a,v); op_sbc(&c,v); break;
        case 0xFB: a=addr_absy(impl); v=static_cast<uint8_t>(RD(a)+1U); WR(a,v); op_sbc(&c,v); break;
        case 0xE3: a=addr_indx(impl); v=static_cast<uint8_t>(RD(a)+1U); WR(a,v); op_sbc(&c,v); break;
        case 0xF3: a=addr_indy(impl); v=static_cast<uint8_t>(RD(a)+1U); WR(a,v); op_sbc(&c,v); break;

        // Immediate / store 类 undocumented opcode。NES 6502 没有 decimal arithmetic，
        // 这里实现常见稳定语义，避免老音乐驱动因非法 opcode 错位。
        case 0x0B: case 0x2B: c.a &= fetch8(impl); set_nz(&c,c.a); set_flag(&c,FlagC,(c.a&0x80U)!=0U); break;
        case 0x4B: c.a &= fetch8(impl); c.a=op_lsr(&c,c.a); break;
        case 0x6B: {
            c.a &= fetch8(impl);
            const bool carry_in = get_flag(&c, FlagC);
            c.a = static_cast<uint8_t>((c.a >> 1U) | (carry_in ? 0x80U : 0U));
            set_nz(&c, c.a);
            set_flag(&c, FlagC, (c.a & 0x40U) != 0U);
            set_flag(&c, FlagV, (((c.a >> 6U) ^ (c.a >> 5U)) & 0x01U) != 0U);
            break;
        }
        case 0x8B: c.a = static_cast<uint8_t>(c.x & fetch8(impl)); set_nz(&c,c.a); break;
        case 0xAB: c.a = c.x = fetch8(impl); set_nz(&c,c.a); break;
        case 0xCB: v=fetch8(impl); a=static_cast<uint16_t>(c.a & c.x); op_cmp(&c,static_cast<uint8_t>(a),v); c.x=static_cast<uint8_t>(a-v); set_nz(&c,c.x); break;
        case 0x93: a=addr_indy(impl); WR(a, static_cast<uint8_t>(c.a & c.x & (((a >> 8U) + 1U) & 0xFFU))); break;
        case 0x9F: a=addr_absy(impl); WR(a, static_cast<uint8_t>(c.a & c.x & (((a >> 8U) + 1U) & 0xFFU))); break;
        case 0x9B: a=addr_absy(impl); c.sp=static_cast<uint8_t>(c.a & c.x); WR(a, static_cast<uint8_t>(c.sp & (((a >> 8U) + 1U) & 0xFFU))); break;
        case 0x9C: a=addr_absx(impl); WR(a, static_cast<uint8_t>(c.y & (((a >> 8U) + 1U) & 0xFFU))); break;
        case 0x9E: a=addr_absy(impl); WR(a, static_cast<uint8_t>(c.x & (((a >> 8U) + 1U) & 0xFFU))); break;
        case 0xBB: a=addr_absy(impl); c.a=c.x=c.sp=static_cast<uint8_t>(RD(a)&c.sp); set_nz(&c,c.a); break;

        // 合法 BRK 在 NSF init/play routine 中不应出现；视为驱动跑飞，避免 AudioTask 死循环。
        case 0x00:
            c.jammed = true; c.jam_opcode = opcode; return false;

        // KIL/JAM opcode。
        case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
        case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
            c.jammed = true; c.jam_opcode = opcode; return false;

        default:
            // 其余 undocumented NOP 使用正确操作数字节数跨过，避免把数据误当下一条 opcode。
            illegal_nop(impl, opcode);
            break;
    }

#undef RMW
#undef CMP_R
#undef SBC
#undef ADC
#undef EOR
#undef AND
#undef ORA
#undef LOAD_Y
#undef LOAD_X
#undef LOAD_A
#undef WR
#undef RD
    return true;
}

static esp_err_t cpu_call(
    NsfSynthImpl *impl,
    uint16_t address,
    uint8_t a,
    uint8_t x,
    uint8_t y,
    bool reset_registers,
    uint32_t instruction_limit,
    const char *stage)
{
    if (address == 0U) return ESP_ERR_INVALID_ARG;
    Cpu6502 &cpu = impl->cpu;
    if (reset_registers) {
        cpu.a = a;
        cpu.x = x;
        cpu.y = y;
        cpu.sp = 0xFFU;
        cpu.p = static_cast<uint8_t>(FlagI | FlagU);
    }
    // PLAY 调用保留 INIT/上一次 PLAY 留下的寄存器和栈状态，只模拟外部 JSR/RTS 边界。
    cpu.pc = address;
    cpu.jammed = false;
    cpu.jam_opcode = 0U;

    // 模拟播放器外部 JSR：RTS 弹出 $FFFE 后 +1 到 $FFFF 作为返回哨兵。
    push8(impl, 0xFFU);
    push8(impl, 0xFEU);

    for (uint32_t count = 0U; count < instruction_limit; ++count) {
        if (cpu.pc == kReturnSentinel) return ESP_OK;
        if (!execute_instruction(impl)) {
            ESP_LOGE(TAG, "6502 %s失败：opcode=%02X pc=%04X track=%u",
                stage != nullptr ? stage : "routine",
                static_cast<unsigned>(cpu.jam_opcode),
                static_cast<unsigned>(cpu.pc),
                static_cast<unsigned>(impl->config.track + 1U));
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    ESP_LOGE(TAG, "6502 %s超出指令上限：pc=%04X track=%u limit=%lu",
        stage != nullptr ? stage : "routine",
        static_cast<unsigned>(cpu.pc),
        static_cast<unsigned>(impl->config.track + 1U),
        static_cast<unsigned long>(instruction_limit));
    return ESP_ERR_TIMEOUT;
}

static void prepare_memory(NsfSynthImpl *impl)
{
    memset(impl->ram, 0, sizeof(impl->ram));
    memset(impl->work_ram, 0, kWorkRamBytes);
    memcpy(impl->bank, impl->config.banks, sizeof(impl->bank));

    // $6000-$7FFF 是可写工作 RAM；若 NSF load address 落在这里，先把 PRG 前缀复制进去。
    size_t offset = 0U;
    uint32_t address = impl->config.load_address;
    while (offset < impl->prg_size && address < 0x8000U) {
        if (address >= 0x6000U) impl->work_ram[address - 0x6000U] = impl->prg[offset];
        ++offset;
        ++address;
    }
}

static esp_err_t reset_track(NsfSynth *synth, uint8_t track)
{
    if (synth == nullptr || synth->impl == nullptr || !synth->open) return ESP_ERR_INVALID_STATE;
    NsfSynthImpl *impl = static_cast<NsfSynthImpl *>(synth->impl);
    if (track >= impl->config.track_count) return ESP_ERR_INVALID_ARG;

    impl->config.track = track;
    prepare_memory(impl);
    apu_reset(&impl->apu);
    memset(impl->apu_registers, 0, sizeof(impl->apu_registers));
    impl->play_write_mask = 0U;
    reset_loop_detector(impl);
    impl->analysis_seen_audible = false;
    impl->analysis_silent_frames = 0ULL;
    impl->visual_tick_read = 0U;
    impl->visual_tick_count = 0U;
    impl->visual_pitch_valid_mask = 0U;
    // NSF1 初始化顺序与硬件播放器保持一致：先清通道，再启用四个基础波形；
    // DMC 由 INIT routine 自己决定是否开启。后台分析实例会同步循环检测所需的寄存器镜像。
    memory_write(impl, 0x4015U, 0x00U);
    memory_write(impl, 0x4015U, 0x0FU);
    memory_write(impl, 0x4017U, 0x40U);

    const esp_err_t init_ret = cpu_call(
        impl,
        impl->config.init_address,
        track,
        0U, // VM10 只选择 NTSC
        0U,
        true,
        kInitInstructionLimit,
        "INIT");
    if (init_ret != ESP_OK) {
        impl->failed = true;
        synth->failed = true;
        return init_ret;
    }

    const uint16_t speed_us = impl->config.ntsc_speed_us != 0U
        ? impl->config.ntsc_speed_us
        : kDefaultNtscSpeedUs;
    impl->play_speed_us = speed_us;
    impl->play_interval_q32 = static_cast<uint64_t>(
        (static_cast<long double>(speed_us) * impl->sample_rate_hz * 4294967296.0L) /
        1000000.0L);
    if (impl->play_interval_q32 == 0ULL) impl->play_interval_q32 = 1ULL << 32U;
    impl->play_phase_q32 = 0ULL;
    impl->analysis_position_q32 = 0ULL;
    impl->failed = false;
    synth->failed = false;
    synth->track = track;
    synth->position_frames = 0ULL;
    return ESP_OK;
}

static esp_err_t execute_one_play_call(NsfSynth *synth)
{
    if (synth == nullptr || synth->impl == nullptr) return ESP_ERR_INVALID_STATE;
    NsfSynthImpl *impl = static_cast<NsfSynthImpl *>(synth->impl);
    if (impl->config.enable_loop_detection || impl->config.enable_visual_capture) {
        impl->play_write_mask = 0U;
    }
    const esp_err_t ret = cpu_call(
        impl,
        impl->config.play_address,
        0U,
        0U,
        0U,
        false,
        kPlayInstructionLimit,
        "PLAY");
    if (ret != ESP_OK) {
        impl->failed = true;
        synth->failed = true;
        return ret;
    }
    loop_detector_on_play(synth);
    if (impl->config.enable_visual_capture) capture_visual_tick(synth);
    return ESP_OK;
}

static esp_err_t run_due_play_calls(NsfSynth *synth)
{
    NsfSynthImpl *impl = static_cast<NsfSynthImpl *>(synth->impl);
    impl->play_phase_q32 += 1ULL << 32U;
    uint8_t calls = 0U;
    while (impl->play_phase_q32 >= impl->play_interval_q32) {
        impl->play_phase_q32 -= impl->play_interval_q32;
        const esp_err_t ret = execute_one_play_call(synth);
        if (ret != ESP_OK) return ret;
        if (++calls >= 4U) {
            // 单个 PCM sample 正常最多跨过一个 PLAY tick；异常配置下丢弃多余欠账，避免 AudioTask 长时间追帧。
            impl->play_phase_q32 = 0ULL;
            break;
        }
    }
    return ESP_OK;
}


} // namespace

esp_err_t nsf_synth_open_owned(
    NsfSynth *synth,
    uint8_t *owned_prg,
    size_t prg_size,
    const NsfSynthConfig *config,
    uint32_t sample_rate_hz)
{
    if (synth == nullptr || owned_prg == nullptr || prg_size == 0U || config == nullptr ||
        sample_rate_hz == 0U || config->track_count == 0U ||
        config->track >= config->track_count || config->load_address < 0x6000U ||
        config->init_address < 0x6000U || config->play_address < 0x6000U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->version != 1U) return ESP_ERR_NOT_SUPPORTED;
    if (config->expansion_chips != 0U) return ESP_ERR_NOT_SUPPORTED;
    // bit0=1 且 bit1=0 表示纯 PAL；双制式文件优先 NTSC。
    if ((config->pal_ntsc_bits & 0x03U) == 0x01U) return ESP_ERR_NOT_SUPPORTED;

    nsf_synth_close(synth);

    NsfSynthImpl *impl = static_cast<NsfSynthImpl *>(heap_caps_calloc(
        1,
        sizeof(NsfSynthImpl),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (impl == nullptr) {
        impl = static_cast<NsfSynthImpl *>(calloc(1, sizeof(NsfSynthImpl)));
    }
    if (impl == nullptr) return ESP_ERR_NO_MEM;

    impl->work_ram = static_cast<uint8_t *>(heap_caps_calloc(
        1,
        kWorkRamBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (impl->work_ram == nullptr) {
        free(impl);
        return ESP_ERR_NO_MEM;
    }
    if (config->enable_loop_detection) {
        impl->loop_history = static_cast<uint32_t *>(heap_caps_malloc(
            kLoopHistoryCapacity * sizeof(uint32_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (impl->loop_history != nullptr) {
            impl->loop_structure_history = static_cast<uint32_t *>(heap_caps_malloc(
                kLoopHistoryCapacity * sizeof(uint32_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (impl->loop_structure_history == nullptr) {
                ESP_LOGW(TAG, "NSF结构Loop历史PSRAM申请失败：仅使用严格可听状态检测");
            }
        } else {
            // 后台循环分析是增强能力；申请失败时实时播放仍可继续，以静音作为结束兜底。
            ESP_LOGW(TAG, "NSF循环分析PSRAM申请失败：本次仅使用静音结束");
        }
    }
    if (config->enable_visual_capture) {
        impl->visual_ticks = static_cast<NsfSynthVisualTick *>(heap_caps_calloc(
            kVisualTickCapacity,
            sizeof(NsfSynthVisualTick),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (impl->visual_ticks == nullptr) {
            ESP_LOGW(TAG, "NSF播放瀑布状态PSRAM申请失败：不影响音频播放");
        }
    }

    impl->prg = owned_prg;
    impl->prg_size = prg_size;
    impl->config = *config;
    impl->sample_rate_hz = sample_rate_hz;
    const float dt = 1.0f / static_cast<float>(sample_rate_hz);
    const float hp_rc = 1.0f / (kTwoPi * 90.0f);
    const float lp_rc = 1.0f / (kTwoPi * 14000.0f);
    impl->hp_alpha = hp_rc / (hp_rc + dt);
    impl->lp_alpha = dt / (lp_rc + dt);
    impl->load_padding = static_cast<uint16_t>(config->load_address & 0x0FFFU);
    for (uint8_t bank : config->banks) {
        if (bank != 0U) {
            impl->uses_banking = true;
            break;
        }
    }

    synth->impl = impl;
    synth->sample_rate_hz = sample_rate_hz;
    synth->track_count = config->track_count;
    synth->track = config->track;
    synth->open = true;
    synth->failed = false;

    const esp_err_t ret = reset_track(synth, config->track);
    if (ret != ESP_OK) {
        // open_owned 失败时所有权仍属于调用方，因此先解除 impl 对 PRG 的引用再 close。
        impl->prg = nullptr;
        nsf_synth_close(synth);
        return ret;
    }

    ESP_LOGI(TAG,
        "NSF 2A03已打开：track=%u/%u prg=%uB bank=%u speed=%uus rate=%luHz",
        static_cast<unsigned>(synth->track + 1U),
        static_cast<unsigned>(synth->track_count),
        static_cast<unsigned>(prg_size),
        static_cast<unsigned>(impl->uses_banking),
        static_cast<unsigned>(config->ntsc_speed_us != 0U ? config->ntsc_speed_us : kDefaultNtscSpeedUs),
        static_cast<unsigned long>(sample_rate_hz));
    return ESP_OK;
}

void nsf_synth_close(NsfSynth *synth)
{
    if (synth == nullptr) return;
    NsfSynthImpl *impl = static_cast<NsfSynthImpl *>(synth->impl);
    if (impl != nullptr) {
        if (impl->prg != nullptr) heap_caps_free(impl->prg);
        if (impl->work_ram != nullptr) heap_caps_free(impl->work_ram);
        if (impl->loop_history != nullptr) heap_caps_free(impl->loop_history);
        if (impl->loop_structure_history != nullptr) heap_caps_free(impl->loop_structure_history);
        if (impl->visual_ticks != nullptr) heap_caps_free(impl->visual_ticks);
        free(impl);
    }
    *synth = {};
}

esp_err_t nsf_synth_set_track(NsfSynth *synth, uint8_t track)
{
    if (synth == nullptr || !synth->open || synth->impl == nullptr) return ESP_ERR_INVALID_STATE;
    const esp_err_t ret = reset_track(synth, track);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NSF Subsong已重置：track=%u/%u",
            static_cast<unsigned>(track + 1U),
            static_cast<unsigned>(synth->track_count));
    }
    return ret;
}

esp_err_t nsf_synth_copy_source(
    const NsfSynth *synth,
    uint8_t **out_prg,
    size_t *out_prg_size,
    NsfSynthConfig *out_config)
{
    if (out_prg == nullptr || out_prg_size == nullptr || out_config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_prg = nullptr;
    *out_prg_size = 0U;
    *out_config = {};
    if (synth == nullptr || !synth->open || synth->impl == nullptr) return ESP_ERR_INVALID_STATE;

    const NsfSynthImpl *impl = static_cast<const NsfSynthImpl *>(synth->impl);
    if (impl->prg == nullptr || impl->prg_size == 0U) return ESP_ERR_INVALID_STATE;
    uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(
        impl->prg_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) return ESP_ERR_NO_MEM;
    memcpy(copy, impl->prg, impl->prg_size);

    *out_prg = copy;
    *out_prg_size = impl->prg_size;
    *out_config = impl->config;
    out_config->track = synth->track;
    return ESP_OK;
}

esp_err_t nsf_synth_render_pcm32(
    NsfSynth *synth,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames)
{
    if (out_frames != nullptr) *out_frames = 0U;
    if (synth == nullptr || out_interleaved_stereo == nullptr || out_frames == nullptr ||
        max_frames == 0U || !synth->open || synth->impl == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (synth->failed) return ESP_ERR_INVALID_STATE;

    NsfSynthImpl *impl = static_cast<NsfSynthImpl *>(synth->impl);
    for (size_t frame = 0U; frame < max_frames; ++frame) {
        const esp_err_t play_ret = run_due_play_calls(synth);
        if (play_ret != ESP_OK) return play_ret;
        const int32_t sample = float_to_pcm32(apu_sample(impl));
        if (impl->config.enable_loop_detection) {
            if (apu_has_audible_activity(impl->apu)) {
                impl->analysis_seen_audible = true;
                impl->analysis_silent_frames = 0ULL;
            } else if (impl->analysis_seen_audible) {
                ++impl->analysis_silent_frames;
            }
        }
        out_interleaved_stereo[frame * 2U] = sample;
        out_interleaved_stereo[frame * 2U + 1U] = sample;
        ++synth->position_frames;
    }
    *out_frames = max_frames;
    return ESP_OK;
}

esp_err_t nsf_synth_analyze_play_calls(
    NsfSynth *synth,
    size_t max_calls,
    size_t *out_calls)
{
    if (out_calls != nullptr) *out_calls = 0U;
    if (synth == nullptr || out_calls == nullptr || max_calls == 0U ||
        !synth->open || synth->impl == nullptr || synth->failed) {
        return ESP_ERR_INVALID_ARG;
    }

    NsfSynthImpl *impl = static_cast<NsfSynthImpl *>(synth->impl);
    if (!impl->config.enable_loop_detection) return ESP_ERR_INVALID_STATE;

    size_t calls = 0U;
    while (calls < max_calls) {
        // 真实渲染路径是在约16.6ms PLAY 周期到点后才调用 PLAY。快速分析也先推进
        // Envelope/Length/DMC 控制状态，再执行下一次 PLAY，避免 INIT 后首帧状态偏移。
        const bool active_interval_start = apu_has_audible_activity(impl->apu);
        apu_fast_advance_analysis_interval(impl);
        const bool active_before_play = apu_has_audible_activity(impl->apu);

        const uint64_t previous_frames = impl->analysis_position_q32 >> 32U;
        impl->analysis_position_q32 += impl->play_interval_q32;
        const uint64_t current_frames = impl->analysis_position_q32 >> 32U;
        synth->position_frames = current_frames;
        const uint64_t advanced_frames = current_frames >= previous_frames
            ? current_frames - previous_frames
            : 0ULL;

        const esp_err_t ret = execute_one_play_call(synth);
        if (ret != ESP_OK) return ret;
        const bool active_after_play = apu_has_audible_activity(impl->apu);

        // 不生成 Pulse/Noise 波形、不跑滤波器、不转换 PCM；自然静音只按 PLAY 周期
        // 累计，误差上限约一个 NSF PLAY 周期。
        if (active_interval_start || active_before_play || active_after_play) {
            impl->analysis_seen_audible = true;
            impl->analysis_silent_frames = 0ULL;
        } else if (impl->analysis_seen_audible) {
            impl->analysis_silent_frames += advanced_frames;
        }

        ++calls;
        if (impl->loop_detected) break;
    }
    *out_calls = calls;
    return ESP_OK;
}

bool nsf_synth_is_open(const NsfSynth *synth)
{
    return synth != nullptr && synth->open && synth->impl != nullptr;
}

bool nsf_synth_has_failed(const NsfSynth *synth)
{
    return synth != nullptr && synth->failed;
}

uint64_t nsf_synth_position_frames(const NsfSynth *synth)
{
    return synth != nullptr && synth->open ? synth->position_frames : 0ULL;
}

uint8_t nsf_synth_track(const NsfSynth *synth)
{
    return synth != nullptr && synth->open ? synth->track : 0U;
}

bool nsf_synth_get_loop_info(const NsfSynth *synth, NsfSynthLoopInfo *out_info)
{
    if (out_info == nullptr) return false;
    *out_info = {};
    if (synth == nullptr || !synth->open || synth->impl == nullptr) return false;
    const NsfSynthImpl *impl = static_cast<const NsfSynthImpl *>(synth->impl);
    out_info->detected = impl->loop_detected;
    out_info->start_frame = impl->loop_start_frame;
    out_info->length_frames = impl->loop_length_frames;
    out_info->hint_available = impl->loop_hint_available;
    out_info->hint_start_frame = impl->loop_hint_start_frame;
    out_info->hint_length_frames = impl->loop_hint_length_frames;
    return true;
}

bool nsf_synth_get_activity_info(const NsfSynth *synth, NsfSynthActivityInfo *out_info)
{
    if (out_info == nullptr) return false;
    *out_info = {};
    if (synth == nullptr || !synth->open || synth->impl == nullptr) return false;
    const NsfSynthImpl *impl = static_cast<const NsfSynthImpl *>(synth->impl);
    out_info->seen_audible = impl->analysis_seen_audible;
    out_info->silent_frames = impl->analysis_silent_frames;
    return true;
}

size_t nsf_synth_take_visual_ticks(
    NsfSynth *synth,
    NsfSynthVisualTick *out_ticks,
    size_t capacity)
{
    if (synth == nullptr || !synth->open || synth->impl == nullptr ||
        out_ticks == nullptr || capacity == 0U) {
        return 0U;
    }
    NsfSynthImpl *impl = static_cast<NsfSynthImpl *>(synth->impl);
    size_t count = impl->visual_tick_count < capacity ? impl->visual_tick_count : capacity;
    for (size_t i = 0U; i < count; ++i) {
        out_ticks[i] = impl->visual_ticks[impl->visual_tick_read];
        impl->visual_tick_read = (impl->visual_tick_read + 1U) % kVisualTickCapacity;
    }
    impl->visual_tick_count -= count;
    return count;
}
