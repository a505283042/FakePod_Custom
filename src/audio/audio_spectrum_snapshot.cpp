#include "audio_spectrum_snapshot.h"
#include "sdkconfig.h"
#include "spectrum_fft_math.h"
#ifndef FAKEPOD_SPECTRUM_DSP
#define FAKEPOD_SPECTRUM_DSP 1
#endif
#if FAKEPOD_SPECTRUM_DSP && CONFIG_IDF_TARGET_ESP32S3
#include "dsps_fft2r.h"
#endif

#include <atomic>
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "app_diag_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
static const char *TAG = "音频频谱";

#if APP_DIAG_BOOT_VERBOSE
#define SPECTRUM_BOOT_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define SPECTRUM_BOOT_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif

// P1.5.2R.3.1：256 点 FFT 配合约 14.7~16kHz 的分析采样率，
// 第一有效 bin 约 57~63Hz，优先保留底鼓/贝斯瞬态；有效视觉频段约覆盖到 7~8kHz。
constexpr size_t FFT_SIZE = 256U;
constexpr uint32_t ANALYSIS_TARGET_RATE_HZ = 16000U;
constexpr uint32_t ANALYSIS_CAPTURE_NORMAL_HZ = 24U;
constexpr uint32_t ANALYSIS_CAPTURE_HIGH_RATE_HZ = 12U;
constexpr uint32_t ANALYSIS_HIGH_RATE_THRESHOLD_HZ = 96000U;
constexpr uint8_t FFT_FRAME_SLOT_COUNT = 2U;
constexpr uint32_t FFT_TASK_STACK_BYTES = 4096U;
// R46.0.94：P2 仍严格低于 LVGL P3 / NasStream P4，但不再与歌词/封面等 P1 后台任务同级争抢。
constexpr UBaseType_t FFT_TASK_PRIORITY = 2U;
constexpr uint32_t FFT_GRACE_REUSE_MS = 2000U;
constexpr BaseType_t FFT_TASK_CORE = 1;

// P1.5.2R.3.1：低频端增加细分。约 16kHz / 256 点时，前几段从约 62Hz 起步，
// 重点覆盖底鼓/贝斯；后半段再逐渐扩宽到约 8kHz。14.7kHz 分析率下比例保持接近。
constexpr uint16_t FFT_BAND_EDGES[AUDIO_SPECTRUM_BAND_COUNT + 1U] = {
    1U, 2U, 3U, 4U, 5U, 6U, 8U, 10U, 13U,
    17U, 22U, 29U, 38U, 49U, 64U, 83U, 128U
};

enum class FftFrameState : uint8_t
{
    Empty = 0,
    Filling,
    Ready,
    Reading
};

struct FftFrameSlot
{
    FftFrameState state = FftFrameState::Empty;
    uint32_t enable_generation = 0U;
    uint32_t playback_revision = 0U;
    uint32_t track_index = UINT32_MAX;
    uint32_t source_sample_rate_hz = 0U;
    uint32_t analysis_sample_rate_hz = 0U;
    uint64_t position_frames = 0ULL;
    int16_t samples[FFT_SIZE] = {};
};

portMUX_TYPE g_spectrum_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_frame_mux = portMUX_INITIALIZER_UNLOCKED;
AudioSpectrumSnapshot g_spectrum_snapshot = {};
FftFrameSlot g_frame_slots[FFT_FRAME_SLOT_COUNT] = {};
enum class FftTaskLifecycle : uint8_t
{
    Off = 0,
    Running,
    Grace
};

std::atomic<TaskHandle_t> g_fft_task{nullptr};
std::atomic<FftTaskLifecycle> g_fft_lifecycle{FftTaskLifecycle::Off};
std::atomic<TickType_t> g_fft_grace_deadline_tick{0};

std::atomic<bool> g_enabled{false};
std::atomic<uint32_t> g_enable_generation{1U};
std::atomic<uint32_t> g_next_revision{1U};
std::atomic<uint32_t> g_analyzed_frames{0U};
std::atomic<uint32_t> g_dropped_frames{0U};

// 以下捕获状态只由 AudioTask 读写，不进入跨任务共享锁。
bool g_capture_enabled_last = false;
bool g_capture_active = false;
uint32_t g_capture_playback_revision = 0U;
uint32_t g_capture_track_index = UINT32_MAX;
uint32_t g_capture_source_rate_hz = 0U;
uint32_t g_capture_decimation = 1U;
uint32_t g_capture_analysis_rate_hz = 0U;
uint32_t g_capture_enable_generation = 0U;
size_t g_capture_count = 0U;
uint64_t g_last_capture_start_frame = 0ULL;
int16_t g_capture_samples[FFT_SIZE] = {};

static uint32_t spectrum_next_revision()
{
    uint32_t revision = g_next_revision.fetch_add(1U, std::memory_order_relaxed);
    if (revision == 0U) {
        revision = g_next_revision.fetch_add(1U, std::memory_order_relaxed);
    }
    return revision;
}

static void spectrum_reset_capture_state()
{
    g_capture_active = false;
    g_capture_count = 0U;
    g_capture_playback_revision = 0U;
    g_capture_track_index = UINT32_MAX;
    g_capture_source_rate_hz = 0U;
    g_capture_decimation = 1U;
    g_capture_analysis_rate_hz = 0U;
    g_capture_enable_generation = 0U;
    g_last_capture_start_frame = 0ULL;
}

static uint32_t spectrum_capture_rate_hz(uint32_t source_rate_hz)
{
    return source_rate_hz >= ANALYSIS_HIGH_RATE_THRESHOLD_HZ
        ? ANALYSIS_CAPTURE_HIGH_RATE_HZ
        : ANALYSIS_CAPTURE_NORMAL_HZ;
}

static uint32_t spectrum_choose_decimation(uint32_t source_rate_hz)
{
    if (source_rate_hz == 0U) {
        return 1U;
    }
    uint32_t decimation =
        (source_rate_hz + ANALYSIS_TARGET_RATE_HZ / 2U) / ANALYSIS_TARGET_RATE_HZ;
    if (decimation < 1U) {
        decimation = 1U;
    }
    if (decimation > 16U) {
        decimation = 16U;
    }
    return decimation;
}

static int16_t spectrum_mix_stereo_to_i16(int32_t left, int32_t right)
{
    // I2S 32bit slot 中，16bit PCM 左移16位、24bit PCM 左移8位。
    // 两声道先用 int64 求和避免溢出，再取平均后的高16位作为 FFT mono 输入。
    int64_t mixed = (static_cast<int64_t>(left) + static_cast<int64_t>(right)) >> 17;
    if (mixed > 32767) {
        mixed = 32767;
    } else if (mixed < -32768) {
        mixed = -32768;
    }
    return static_cast<int16_t>(mixed);
}

static bool spectrum_submit_fft_frame(uint64_t position_frames)
{
    TaskHandle_t fft_task = g_fft_task.load(std::memory_order_acquire);
    if (fft_task == nullptr || !g_enabled.load(std::memory_order_acquire)) {
        return false;
    }

    int slot_index = -1;
    portENTER_CRITICAL(&g_frame_mux);
    for (uint8_t i = 0U; i < FFT_FRAME_SLOT_COUNT; ++i) {
        if (g_frame_slots[i].state == FftFrameState::Empty) {
            g_frame_slots[i].state = FftFrameState::Filling;
            slot_index = static_cast<int>(i);
            break;
        }
    }
    portEXIT_CRITICAL(&g_frame_mux);

    if (slot_index < 0) {
        // 分析任务来不及时宁可丢掉一帧，也绝不阻塞 AudioTask。
        g_dropped_frames.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    FftFrameSlot &slot = g_frame_slots[slot_index];
    slot.enable_generation = g_capture_enable_generation;
    slot.playback_revision = g_capture_playback_revision;
    slot.track_index = g_capture_track_index;
    slot.source_sample_rate_hz = g_capture_source_rate_hz;
    slot.analysis_sample_rate_hz = g_capture_analysis_rate_hz;
    slot.position_frames = position_frames;
    memcpy(slot.samples, g_capture_samples, sizeof(g_capture_samples));

    portENTER_CRITICAL(&g_frame_mux);
    slot.state = FftFrameState::Ready;
    portEXIT_CRITICAL(&g_frame_mux);
    xTaskNotifyGive(fft_task);
    return true;
}

static int spectrum_claim_ready_slot()
{
    int slot_index = -1;
    portENTER_CRITICAL(&g_frame_mux);
    for (uint8_t i = 0U; i < FFT_FRAME_SLOT_COUNT; ++i) {
        if (g_frame_slots[i].state == FftFrameState::Ready) {
            g_frame_slots[i].state = FftFrameState::Reading;
            slot_index = static_cast<int>(i);
            break;
        }
    }
    portEXIT_CRITICAL(&g_frame_mux);
    return slot_index;
}

static void spectrum_release_slot(int slot_index)
{
    if (slot_index < 0 || slot_index >= FFT_FRAME_SLOT_COUNT) {
        return;
    }
    portENTER_CRITICAL(&g_frame_mux);
    g_frame_slots[slot_index].state = FftFrameState::Empty;
    portEXIT_CRITICAL(&g_frame_mux);
}

static void spectrum_fft_in_place(float *data)
{
#if FAKEPOD_SPECTRUM_DSP && CONFIG_IDF_TARGET_ESP32S3
    // Use the fixed-size raw kernel with our own immutable coefficient table.
    // No global ESP-DSP init/deinit, heap allocation, or task-lifetime table race.
    (void)dsps_fft2r_fc32_aes3_(data, static_cast<int>(FFT_SIZE),
        const_cast<float *>(fakepod_spectrum::kTwiddle));
#else
    fakepod_spectrum::portable_fft(data);
#endif
    fakepod_spectrum::bit_reverse(data);
}

static void spectrum_analyze_frame(const FftFrameSlot &frame, uint8_t *levels)
{
    // FFT 工作数组只存在于低优先级任务栈，不占 AudioTask 栈，也不会交给 UI。
    alignas(16) float data[FFT_SIZE * 2U];
    static_assert(FFT_SIZE == fakepod_spectrum::kSize);
    fakepod_spectrum::prepare(frame.samples, data);
    spectrum_fft_in_place(data);

    for (uint8_t band = 0U; band < AUDIO_SPECTRUM_BAND_COUNT; ++band) {
        const uint16_t start = FFT_BAND_EDGES[band];
        const uint16_t end = FFT_BAND_EDGES[band + 1U];
        float max_power = 0.0f;
        for (uint16_t bin = start; bin < end && bin < FFT_SIZE / 2U; ++bin) {
            const float power = data[2U * bin] * data[2U * bin] + data[2U * bin + 1U] * data[2U * bin + 1U];
            if (power > max_power) {
                max_power = power;
            }
        }
        levels[band] = fakepod_spectrum::power_level(max_power);
    }
}

static void spectrum_publish_fft_snapshot(const FftFrameSlot &frame, const uint8_t *levels)
{
    if (
        levels == nullptr ||
        !g_enabled.load(std::memory_order_acquire) ||
        frame.enable_generation != g_enable_generation.load(std::memory_order_acquire)
    ) {
        return;
    }

    AudioSpectrumSnapshot snapshot = {};
    snapshot.valid = true;
    snapshot.revision = spectrum_next_revision();
    snapshot.playback_revision = frame.playback_revision;
    snapshot.track_index = frame.track_index;
    snapshot.sample_rate_hz = frame.source_sample_rate_hz;
    snapshot.analysis_sample_rate_hz = frame.analysis_sample_rate_hz;
    snapshot.fft_size = static_cast<uint16_t>(FFT_SIZE);
    snapshot.position_frames = frame.position_frames;
    memcpy(snapshot.levels, levels, sizeof(snapshot.levels));

    portENTER_CRITICAL(&g_spectrum_mux);
    g_spectrum_snapshot = snapshot;
    portEXIT_CRITICAL(&g_spectrum_mux);
}

static void spectrum_fft_task_main(void *)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    uint32_t logged_generation = 0U;
#if FAKEPOD_SPECTRUM_DSP && CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGI(TAG, "FFT backend=ESP-DSP S3 aes3 N=256 readonly-table stack-work=2048B");
#else
    ESP_LOGI(TAG, "FFT backend=portable-table N=256 stack-work=2048B");
#endif

#if APP_DIAG_SPECTRUM_PERFORMANCE
    int64_t perf_since_us=esp_timer_get_time();
    uint64_t perf_total_us=0;uint32_t perf_frames=0,perf_max_us=0;
#endif
    while (true) {
        FftTaskLifecycle lifecycle = g_fft_lifecycle.load(std::memory_order_acquire);
        TickType_t wait_ticks = portMAX_DELAY;

        if (lifecycle == FftTaskLifecycle::Grace) {
            const TickType_t now_tick = xTaskGetTickCount();
            const TickType_t deadline_tick =
                g_fft_grace_deadline_tick.load(std::memory_order_acquire);
            const int32_t remaining_ticks =
                static_cast<int32_t>(deadline_tick - now_tick);
            if (remaining_ticks <= 0) {
                FftTaskLifecycle expected = FftTaskLifecycle::Grace;
                if (g_fft_lifecycle.compare_exchange_strong(
                        expected, FftTaskLifecycle::Off,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            wait_ticks = static_cast<TickType_t>(remaining_ticks);
        } else if (lifecycle == FftTaskLifecycle::Off) {
            break;
        }

        const uint32_t notified = ulTaskNotifyTake(pdTRUE, wait_ticks);
        lifecycle = g_fft_lifecycle.load(std::memory_order_acquire);

        if (lifecycle == FftTaskLifecycle::Grace) {
            const TickType_t now_tick = xTaskGetTickCount();
            const TickType_t deadline_tick =
                g_fft_grace_deadline_tick.load(std::memory_order_acquire);
            if (notified == 0U &&
                static_cast<int32_t>(now_tick - deadline_tick) >= 0) {
                FftTaskLifecycle expected = FftTaskLifecycle::Grace;
                if (g_fft_lifecycle.compare_exchange_strong(
                        expected, FftTaskLifecycle::Off,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    break;
                }
            }
            continue;
        }
        if (lifecycle != FftTaskLifecycle::Running) {
            continue;
        }

        while (g_fft_lifecycle.load(std::memory_order_acquire) == FftTaskLifecycle::Running) {
            const int slot_index = spectrum_claim_ready_slot();
            if (slot_index < 0) {
                break;
            }

            FftFrameSlot &frame = g_frame_slots[slot_index];
            const uint32_t generation = frame.enable_generation;
            if (
                g_enabled.load(std::memory_order_acquire) &&
                generation == g_enable_generation.load(std::memory_order_acquire)
            ) {
                uint8_t levels[AUDIO_SPECTRUM_BAND_COUNT] = {};
                const int64_t fft_begin_us = esp_timer_get_time();
                spectrum_analyze_frame(frame, levels);
                const uint32_t fft_elapsed_us = static_cast<uint32_t>(
                    esp_timer_get_time() - fft_begin_us);
#if APP_DIAG_SPECTRUM_PERFORMANCE
                perf_total_us+=fft_elapsed_us;++perf_frames;
                if(fft_elapsed_us>perf_max_us)perf_max_us=fft_elapsed_us;
                const int64_t perf_now_us=esp_timer_get_time();
                if(perf_now_us-perf_since_us>=5000000LL){
                    ESP_LOGI(TAG, "SPECTRUM_PERF: frames=%lu avg=%luus max=%luus dropped_total=%lu source=%luHz stack_hwm=%u",
                        static_cast<unsigned long>(perf_frames),
                        static_cast<unsigned long>(perf_total_us/perf_frames),
                        static_cast<unsigned long>(perf_max_us),
                        static_cast<unsigned long>(g_dropped_frames.load(std::memory_order_relaxed)),
                        static_cast<unsigned long>(frame.source_sample_rate_hz),
                        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
                    perf_since_us=perf_now_us;perf_total_us=0;perf_frames=0;perf_max_us=0;
                }
#endif
                spectrum_publish_fft_snapshot(frame, levels);
                g_analyzed_frames.fetch_add(1U, std::memory_order_relaxed);

                if (logged_generation != generation) {
                    logged_generation = generation;
                    SPECTRUM_BOOT_LOGI(
                        "FFT 首帧：source=%luHz analysis=%luHz N=%u bands=%u fft=%luus core=%d priority=%u stack_hwm=%u",
                        static_cast<unsigned long>(frame.source_sample_rate_hz),
                        static_cast<unsigned long>(frame.analysis_sample_rate_hz),
                        static_cast<unsigned>(FFT_SIZE),
                        static_cast<unsigned>(AUDIO_SPECTRUM_BAND_COUNT),
                        static_cast<unsigned long>(fft_elapsed_us),
                        static_cast<int>(FFT_TASK_CORE),
                        static_cast<unsigned>(FFT_TASK_PRIORITY),
                        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
                }
            }

            spectrum_release_slot(slot_index);
        }
    }

    TaskHandle_t expected_task = self;
    (void)g_fft_task.compare_exchange_strong(
        expected_task, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    ESP_LOGI(TAG, "SpectrumFFT 延迟回收：stack=%uB grace=%ums",
        static_cast<unsigned>(FFT_TASK_STACK_BYTES),
        static_cast<unsigned>(FFT_GRACE_REUSE_MS));
    vTaskDelete(nullptr);
}
} // namespace

esp_err_t audio_spectrum_snapshot_start()
{
    FftTaskLifecycle expected = FftTaskLifecycle::Off;
    if (!g_fft_lifecycle.compare_exchange_strong(
            expected, FftTaskLifecycle::Running,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return ESP_OK;
    }

    g_fft_grace_deadline_tick.store(0, std::memory_order_release);
    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreatePinnedToCore(
        spectrum_fft_task_main,
        "SpectrumFFT",
        FFT_TASK_STACK_BYTES,
        nullptr,
        FFT_TASK_PRIORITY,
        &task,
        FFT_TASK_CORE);
    if (created != pdPASS || task == nullptr) {
        g_fft_lifecycle.store(FftTaskLifecycle::Off, std::memory_order_release);
        ESP_LOGE(TAG, "创建 SpectrumFFT 任务失败");
        return ESP_ERR_NO_MEM;
    }
    g_fft_task.store(task, std::memory_order_release);

    SPECTRUM_BOOT_LOGI(
        "SpectrumFFT 任务已启动：core=%d priority=%u stack=%uB N=%u capture=%u/%uHz target=%uHz floor=-45dB",
        static_cast<int>(FFT_TASK_CORE),
        static_cast<unsigned>(FFT_TASK_PRIORITY),
        static_cast<unsigned>(FFT_TASK_STACK_BYTES),
        static_cast<unsigned>(FFT_SIZE),
        static_cast<unsigned>(ANALYSIS_CAPTURE_NORMAL_HZ),
        static_cast<unsigned>(ANALYSIS_CAPTURE_HIGH_RATE_HZ),
        static_cast<unsigned>(ANALYSIS_TARGET_RATE_HZ));
    return ESP_OK;
}

bool audio_spectrum_snapshot_is_ready()
{
    return g_fft_lifecycle.load(std::memory_order_acquire) != FftTaskLifecycle::Off;
}

static esp_err_t spectrum_fft_task_acquire()
{
    while (true) {
        FftTaskLifecycle lifecycle = g_fft_lifecycle.load(std::memory_order_acquire);
        if (lifecycle == FftTaskLifecycle::Running) {
            return ESP_OK;
        }
        if (lifecycle == FftTaskLifecycle::Grace) {
            FftTaskLifecycle expected = FftTaskLifecycle::Grace;
            if (g_fft_lifecycle.compare_exchange_weak(
                    expected, FftTaskLifecycle::Running,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                g_fft_grace_deadline_tick.store(0, std::memory_order_release);
                TaskHandle_t task = g_fft_task.load(std::memory_order_acquire);
                if (task != nullptr) {
                    xTaskNotifyGive(task);
                }
                ESP_LOGI(TAG, "SpectrumFFT Grace复用：无需重建任务");
                return ESP_OK;
            }
            continue;
        }
        return audio_spectrum_snapshot_start();
    }
}

static void spectrum_fft_task_begin_grace()
{
    const TickType_t deadline_tick =
        xTaskGetTickCount() + pdMS_TO_TICKS(FFT_GRACE_REUSE_MS);
    g_fft_grace_deadline_tick.store(deadline_tick, std::memory_order_release);

    FftTaskLifecycle lifecycle = g_fft_lifecycle.load(std::memory_order_acquire);
    while (lifecycle == FftTaskLifecycle::Running &&
           !g_fft_lifecycle.compare_exchange_weak(
               lifecycle, FftTaskLifecycle::Grace,
               std::memory_order_acq_rel, std::memory_order_acquire)) {
    }

    if (g_fft_lifecycle.load(std::memory_order_acquire) == FftTaskLifecycle::Grace) {
        TaskHandle_t task = g_fft_task.load(std::memory_order_acquire);
        if (task != nullptr) {
            xTaskNotifyGive(task);
        }
    }
}

void audio_spectrum_snapshot_set_enabled(bool enabled)
{
    if (enabled) {
        const esp_err_t start_ret = spectrum_fft_task_acquire();
        if (start_ret != ESP_OK) {
            ESP_LOGW(TAG, "FFT频谱按需启动失败：%s", esp_err_to_name(start_ret));
            return;
        }
    }

    const bool previous = g_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (previous == enabled) {
        if (!enabled) {
            spectrum_fft_task_begin_grace();
        }
        return;
    }

    uint32_t generation = g_enable_generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    if (generation == 0U) {
        generation = g_enable_generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    }

    if (enabled) {
        g_analyzed_frames.store(0U, std::memory_order_relaxed);
        g_dropped_frames.store(0U, std::memory_order_relaxed);
    }

    if (!enabled) {
        AudioSpectrumSnapshot snapshot = {};
        snapshot.valid = false;
        snapshot.revision = spectrum_next_revision();
        portENTER_CRITICAL(&g_spectrum_mux);
        g_spectrum_snapshot = snapshot;
        portEXIT_CRITICAL(&g_spectrum_mux);

        // Ready帧可以直接作废；Reading帧由 generation 检查保证不会重新发布。
        portENTER_CRITICAL(&g_frame_mux);
        for (uint8_t i = 0U; i < FFT_FRAME_SLOT_COUNT; ++i) {
            if (g_frame_slots[i].state == FftFrameState::Ready) {
                g_frame_slots[i].state = FftFrameState::Empty;
            }
        }
        portEXIT_CRITICAL(&g_frame_mux);
    }

    if (enabled) {
        ESP_LOGI(TAG, "FFT频谱旁路：启用 generation=%lu",
            static_cast<unsigned long>(generation));
    } else {
        ESP_LOGI(TAG,
            "FFT频谱旁路：关闭 generation=%lu analyzed=%lu dropped=%lu",
            static_cast<unsigned long>(generation),
            static_cast<unsigned long>(g_analyzed_frames.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(g_dropped_frames.load(std::memory_order_relaxed)));
        spectrum_fft_task_begin_grace();
    }
}

void audio_spectrum_snapshot_reset(
    uint32_t playback_revision,
    uint32_t track_index,
    uint32_t sample_rate_hz)
{
    AudioSpectrumSnapshot snapshot = {};
    snapshot.valid = false;
    snapshot.revision = spectrum_next_revision();
    snapshot.playback_revision = playback_revision;
    snapshot.track_index = track_index;
    snapshot.sample_rate_hz = sample_rate_hz;

    spectrum_reset_capture_state();

    portENTER_CRITICAL(&g_frame_mux);
    for (uint8_t i = 0U; i < FFT_FRAME_SLOT_COUNT; ++i) {
        if (g_frame_slots[i].state != FftFrameState::Reading) {
            g_frame_slots[i].state = FftFrameState::Empty;
        }
    }
    portEXIT_CRITICAL(&g_frame_mux);

    portENTER_CRITICAL(&g_spectrum_mux);
    g_spectrum_snapshot = snapshot;
    portEXIT_CRITICAL(&g_spectrum_mux);
}

void audio_spectrum_snapshot_publish_pcm(
    const int32_t *interleaved_stereo,
    size_t frames,
    AudioPlaybackSource source,
    uint32_t playback_revision,
    uint32_t track_index,
    uint32_t sample_rate_hz,
    uint64_t submitted_frames)
{
    if (interleaved_stereo == nullptr || frames == 0U || sample_rate_hz == 0U) {
        return;
    }

    // R46.0.99：只保护 NAS_HTTP 的 96kHz及以上流；本地高采样率仍保留12Hz FFT降载。
    if (source == AudioPlaybackSource::NasHttp &&
        sample_rate_hz >= ANALYSIS_HIGH_RATE_THRESHOLD_HZ) {
        if (g_capture_enabled_last) spectrum_reset_capture_state();
        g_capture_enabled_last = false;
        return;
    }

    const bool enabled = g_enabled.load(std::memory_order_acquire);
    if (!enabled) {
        if (g_capture_enabled_last) {
            spectrum_reset_capture_state();
        }
        g_capture_enabled_last = false;
        return;
    }

    if (!g_capture_enabled_last) {
        spectrum_reset_capture_state();
        g_capture_enabled_last = true;
    }

    const uint32_t enable_generation = g_enable_generation.load(std::memory_order_acquire);
    if (
        g_capture_active &&
        (g_capture_playback_revision != playback_revision ||
         g_capture_track_index != track_index ||
         g_capture_source_rate_hz != sample_rate_hz ||
         g_capture_enable_generation != enable_generation)
    ) {
        spectrum_reset_capture_state();
    }

    if (!g_capture_active) {
        const uint32_t capture_rate_hz = spectrum_capture_rate_hz(sample_rate_hz);
        const uint64_t interval = sample_rate_hz >= capture_rate_hz
            ? static_cast<uint64_t>(sample_rate_hz / capture_rate_hz)
            : 1ULL;
        if (
            g_last_capture_start_frame != 0ULL &&
            submitted_frames >= g_last_capture_start_frame &&
            submitted_frames - g_last_capture_start_frame < interval
        ) {
            return;
        }

        g_capture_active = true;
        g_capture_count = 0U;
        g_capture_playback_revision = playback_revision;
        g_capture_track_index = track_index;
        g_capture_source_rate_hz = sample_rate_hz;
        g_capture_decimation = spectrum_choose_decimation(sample_rate_hz);
        g_capture_analysis_rate_hz = sample_rate_hz / g_capture_decimation;
        g_capture_enable_generation = enable_generation;
        g_last_capture_start_frame = submitted_frames;
    }

    const uint64_t block_start = submitted_frames >= frames
        ? submitted_frames - static_cast<uint64_t>(frames)
        : 0ULL;
    const uint32_t remainder = static_cast<uint32_t>(block_start % g_capture_decimation);
    size_t frame_index = remainder == 0U ? 0U : (g_capture_decimation - remainder);

    for (; frame_index < frames && g_capture_count < FFT_SIZE;
         frame_index += g_capture_decimation) {
        g_capture_samples[g_capture_count++] = spectrum_mix_stereo_to_i16(
            interleaved_stereo[frame_index * 2U],
            interleaved_stereo[frame_index * 2U + 1U]);
    }

    if (g_capture_count >= FFT_SIZE) {
        spectrum_submit_fft_frame(submitted_frames);
        g_capture_active = false;
        g_capture_count = 0U;
        // P1.5.2R.3.1：节流锚定“本窗开始时间”而不是完成时间。256点/约16kHz窗口约16ms，
        // 这样24Hz配置得到约42ms start-to-start 节奏，而不是额外再叠加一个窗口时长。
        // 即使双槽满也只会丢这一帧，不做补帧循环，因此不会反向阻塞 AudioTask。
    }
}

bool audio_spectrum_snapshot_get(AudioSpectrumSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }
    portENTER_CRITICAL(&g_spectrum_mux);
    *out_snapshot = g_spectrum_snapshot;
    portEXIT_CRITICAL(&g_spectrum_mux);
    return true;
}
