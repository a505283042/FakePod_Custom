#include "audio_service.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cs43131.h"
#include "i2s_output.h"
#include "pcm_decoder.h"
#include "audio_decode_workspace.h"
#include "audio_playback_clock.h"
#include "audio_spectrum_snapshot.h"
#include "app_diag_config.h"

static const char *TAG = "音频服务";

#if APP_DIAG_AUDIO_POP
#define AUDIO_POP_TRACE_LOG(...) \
    ESP_LOGI(TAG, "POP_TRACE: " __VA_ARGS__)
#else
#define AUDIO_POP_TRACE_LOG(...) APP_DIAG_DISCARDED_LOGI(TAG, "POP_TRACE: " __VA_ARGS__)
#endif

// 仅在控制路径关键节点采样，不进入 PCM 热循环。
// 用于判断 AudioTask 栈是否可以后续从 24KB 安全下调，以及播放链路是否侵蚀内部 RAM。
#if APP_DIAG_AUDIO_RAM
static void audio_task_log_ram(const char *stage)
{
    const size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_min =
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t dma_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t psram_free =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "RAM_TRACE: %s internal=%u min=%u largest=%u dma=%u psram=%u stack_hwm=%u",
        stage ? stage : "unknown",
        static_cast<unsigned>(internal_free),
        static_cast<unsigned>(internal_min),
        static_cast<unsigned>(internal_largest),
        static_cast<unsigned>(dma_free),
        static_cast<unsigned>(psram_free),
        static_cast<unsigned>(stack_hwm));
}
#else
static inline void audio_task_log_ram(const char *) {}
#endif

static constexpr uint32_t AUDIO_TASK_STACK_BYTES = 12288;
static constexpr UBaseType_t AUDIO_TASK_PRIORITY = 5;
static constexpr BaseType_t AUDIO_TASK_CORE = 0;
static constexpr UBaseType_t AUDIO_COMMAND_QUEUE_LENGTH = 8;
static constexpr size_t AUDIO_INLINE_PATH_SIZE = 384;
static constexpr size_t AUDIO_MAX_PATH_SIZE = 4096;
static constexpr size_t AUDIO_STREAM_FRAMES = 256;
static constexpr uint32_t AUDIO_PCM_FADE_IN_MS = 30;
static constexpr uint8_t AUDIO_PCM_UNMUTE_PRIME_BLOCKS = 4;
static constexpr uint32_t AUDIO_I2S_WRITE_TIMEOUT_MS = 100;
static constexpr uint32_t AUDIO_PCM_MUTE_SETTLE_MS = 150;
// 常规 codec workspace 跨曲保留复用；若异常文件把 PCM 工作区推到超大尺寸，
// 关闭该曲后释放，避免一次特殊文件永久占住大量 PSRAM。
static constexpr size_t AUDIO_DECODE_WORKSPACE_RETAIN_INPUT_BYTES = 32 * 1024;
static constexpr size_t AUDIO_DECODE_WORKSPACE_RETAIN_PCM_BYTES = 128 * 1024;
static constexpr TickType_t AUDIO_QUEUE_SEND_TIMEOUT = pdMS_TO_TICKS(50);
static constexpr TickType_t AUDIO_SYNC_WAIT_TIMEOUT = pdMS_TO_TICKS(1500);
static constexpr TickType_t AUDIO_START_WAIT_TIMEOUT = pdMS_TO_TICKS(1500);

// 用户音量继续保持此前实机验证的安全模拟基线：CS43131 0.5Vrms 满量程。
// P1.5.3.2R.13 重新分配逻辑音量曲线后，默认逻辑音量改为 50%（约 -18dB），
// 保持接近旧版 80%=-20dB 的启动实际响度；运行期音量仍只能由 AudioTask 写 DAC。
// 192kHz 长时间实测显示 AudioTask 峰值栈使用约 5.3KB；先保守收敛到 12KB，仍保留超过一倍的观测余量。


enum class AudioCommandType : uint8_t
{
    Play = 1,
    Stop,
    Pause,
    Resume,
    Seek,
    SetVolume,
    SetMute
};

struct AudioRequest
{
    AudioCommandType type = AudioCommandType::Stop;
    uint32_t request_id = 0;
    uint32_t transport_intent_revision = 0;
    uint32_t track_index = UINT32_MAX;
    MediaFormat format = MediaFormat::Unknown;
    bool has_technical_info = false;
    MediaTechnicalInfo technical_info = {};
    uint8_t volume_percent = 50;
    bool mute = false;
    uint32_t expected_playback_revision = 0;
    uint64_t seek_target_ms = 0;
    char path[AUDIO_INLINE_PATH_SIZE] = {};
    char *extended_path = nullptr;
    SemaphoreHandle_t done = nullptr;
    bool success = false;
    esp_err_t result = ESP_FAIL;
    bool completed = false;
    uint8_t refs = 1;
};

static QueueHandle_t g_command_queue = nullptr;
static TaskHandle_t g_audio_task = nullptr;
static SemaphoreHandle_t g_start_done = nullptr;
static esp_err_t g_start_result = ESP_ERR_INVALID_STATE;

static portMUX_TYPE g_request_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_next_request_id = 1;

// Stage 11.0.1：Play/Seek 属于“目标型 transport intent”。
// 待处理 intent 只保留最新一份；队列里最多挂一个 transport 唤醒请求，避免快速连续 NEXT/Seek
// 把 AudioTask 队列塞满。正在执行的旧 intent 则通过 revision 在安全检查点主动退出。
static SemaphoreHandle_t g_transport_submit_mutex = nullptr;
static AudioRequest *g_pending_transport_request = nullptr;
static bool g_transport_wake_pending = false;
static uint32_t g_next_transport_intent_revision = 1;
static uint32_t g_latest_transport_intent_revision = 0;

static portMUX_TYPE g_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static AudioStateSnapshot g_snapshot = {};

// R.36.2.2：最近一次真实播放故障保留在独立 POD 快照中。
// 即使关闭高频诊断，故障现场也不会随着 pipeline shutdown 丢失。
static portMUX_TYPE g_fault_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static AudioFaultSnapshot g_fault_snapshot = {};
static uint32_t g_fault_count = 0;

// 以下状态只允许 AudioTask 自己写。
static bool g_task_ready = false;
static AudioPlaybackState g_task_state = AudioPlaybackState::Starting;
static uint32_t g_task_state_revision = 0;
static uint32_t g_task_playback_revision = 1;
static uint32_t g_task_last_request_id = 0;
static uint32_t g_task_track_index = UINT32_MAX;
static MediaFormat g_task_format = MediaFormat::Unknown;
static esp_err_t g_task_last_error = ESP_OK;
static uint32_t g_task_sample_rate_hz = 0;
static uint16_t g_task_channels = 0;
static uint16_t g_task_bits_per_sample = 0;
static AudioPlaybackClock g_playback_clock = {};
static uint64_t g_task_total_frames = 0;
static uint32_t g_task_seek_revision = 0;
static uint32_t g_task_last_seek_request_id = 0;
static uint64_t g_task_last_seek_target_ms = 0;
static uint64_t g_last_progress_publish_frame = 0;
static bool g_ram_trace_first_pcm_done = false;
static bool g_ram_trace_steady_5s_done = false;
static bool g_pcm_unmute_pending = false;
static uint32_t g_pcm_fade_in_total_frames = 0;
static uint32_t g_pcm_fade_in_done_frames = 0;
static bool g_pcm_fade_in_logged_done = true;
static uint8_t g_task_volume_percent = 50U;
static bool g_task_user_muted = false;

// 正式播放资源也只属于 AudioTask。
// WAV/FLAC/MP3 都通过统一 PcmDecoder 产出 32bit stereo PCM，I2S/DAC 不关心源格式。
static PcmDecoder g_decoder = {};
static AudioDecodeWorkspace g_decode_workspace = {};
static bool g_pipeline_clock_prepared = false;
static bool g_pipeline_i2s_started = false;
static bool g_pipeline_asp_enabled = false;
static bool g_pipeline_headphone_enabled = false;
static int32_t g_pcm_block[AUDIO_STREAM_FRAMES * 2] = {};

static const char *audio_request_path(const AudioRequest *request)
{
    if (request == nullptr) {
        return nullptr;
    }
    return request->extended_path != nullptr ? request->extended_path : request->path;
}

static void audio_request_retain(AudioRequest *request)
{
    if (request == nullptr) {
        return;
    }
    portENTER_CRITICAL(&g_request_mux);
    request->refs++;
    portEXIT_CRITICAL(&g_request_mux);
}

static void audio_request_release(AudioRequest *request)
{
    if (request == nullptr) {
        return;
    }

    bool destroy = false;
    portENTER_CRITICAL(&g_request_mux);
    if (request->refs > 0) {
        request->refs--;
        destroy = request->refs == 0;
    }
    portEXIT_CRITICAL(&g_request_mux);

    if (!destroy) {
        return;
    }

    if (request->done != nullptr) {
        vSemaphoreDelete(request->done);
        request->done = nullptr;
    }
    if (request->extended_path != nullptr) {
        heap_caps_free(request->extended_path);
        request->extended_path = nullptr;
    }
    heap_caps_free(request);
}

static AudioRequest *audio_request_create(AudioCommandType type, bool wait)
{
    AudioRequest *request = static_cast<AudioRequest *>(
        heap_caps_calloc(1, sizeof(AudioRequest), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (request == nullptr) {
        request = static_cast<AudioRequest *>(calloc(1, sizeof(AudioRequest)));
    }
    if (request == nullptr) {
        return nullptr;
    }

    request->type = type;
    request->refs = 1;
    request->track_index = UINT32_MAX;
    request->format = MediaFormat::Unknown;
    request->result = ESP_FAIL;

    portENTER_CRITICAL(&g_request_mux);
    request->request_id = g_next_request_id++;
    if (g_next_request_id == 0) {
        g_next_request_id = 1;
    }
    portEXIT_CRITICAL(&g_request_mux);

    if (wait) {
        request->done = xSemaphoreCreateBinary();
        if (request->done == nullptr) {
            audio_request_release(request);
            return nullptr;
        }
    }
    return request;
}

static bool audio_request_set_path(AudioRequest *request, const char *path)
{
    if (request == nullptr || path == nullptr) {
        return false;
    }

    const size_t length = strlen(path);
    if (length > AUDIO_MAX_PATH_SIZE) {
        ESP_LOGE(TAG, "路径过长，拒绝入队：长度=%u，上限=%u，请求=%lu",
            static_cast<unsigned>(length),
            static_cast<unsigned>(AUDIO_MAX_PATH_SIZE),
            static_cast<unsigned long>(request->request_id));
        return false;
    }

    if (length < sizeof(request->path)) {
        memcpy(request->path, path, length + 1);
        return true;
    }

    request->extended_path = static_cast<char *>(
        heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (request->extended_path == nullptr) {
        request->extended_path = static_cast<char *>(malloc(length + 1));
    }
    if (request->extended_path == nullptr) {
        ESP_LOGE(TAG, "长路径分配失败：长度=%u，请求=%lu",
            static_cast<unsigned>(length),
            static_cast<unsigned long>(request->request_id));
        return false;
    }

    memcpy(request->extended_path, path, length + 1);
    ESP_LOGI(TAG, "长路径使用动态缓冲：长度=%u，请求=%lu",
        static_cast<unsigned>(length),
        static_cast<unsigned long>(request->request_id));
    return true;
}

static void audio_task_advance_playback_revision()
{
    g_task_playback_revision++;
    if (g_task_playback_revision == 0) {
        g_task_playback_revision = 1;
    }
}

static void audio_task_publish_snapshot()
{
    AudioStateSnapshot snapshot = {};
    snapshot.ready = g_task_ready;
    snapshot.state = g_task_state;
    snapshot.state_revision = ++g_task_state_revision;
    if (snapshot.state_revision == 0) {
        snapshot.state_revision = ++g_task_state_revision;
    }
    snapshot.playback_revision = g_task_playback_revision;
    snapshot.last_request_id = g_task_last_request_id;
    snapshot.track_index = g_task_track_index;
    snapshot.format = g_task_format;
    snapshot.last_error = g_task_last_error;
    snapshot.queue_depth = g_command_queue != nullptr
        ? static_cast<uint32_t>(uxQueueMessagesWaiting(g_command_queue))
        : 0;
    snapshot.sample_rate_hz = g_task_sample_rate_hz;
    snapshot.channels = g_task_channels;
    snapshot.bits_per_sample = g_task_bits_per_sample;
    snapshot.position_frames = g_playback_clock.submitted_frames;
    snapshot.decoder_position_frames = g_playback_clock.decoder_frames;
    snapshot.position_ms = audio_playback_clock_position_ms(&g_playback_clock);
    snapshot.total_frames = g_task_total_frames;
    snapshot.seek_supported = pcm_decoder_seek_supported(&g_decoder, 1);
    snapshot.seek_revision = g_task_seek_revision;
    snapshot.last_seek_request_id = g_task_last_seek_request_id;
    snapshot.last_seek_target_ms = g_task_last_seek_target_ms;
    snapshot.decode_workspace_input_bytes = static_cast<uint32_t>(
        g_decode_workspace.input_capacity > UINT32_MAX
            ? UINT32_MAX
            : g_decode_workspace.input_capacity);
    snapshot.decode_workspace_pcm_bytes = static_cast<uint32_t>(
        g_decode_workspace.decoded_capacity > UINT32_MAX
            ? UINT32_MAX
            : g_decode_workspace.decoded_capacity);
    snapshot.volume_percent = g_task_volume_percent;
    snapshot.user_muted = g_task_user_muted;

    portENTER_CRITICAL(&g_snapshot_mux);
    g_snapshot = snapshot;
    portEXIT_CRITICAL(&g_snapshot_mux);
}

static void audio_task_set_state(AudioPlaybackState state, esp_err_t error = ESP_OK)
{
    g_task_state = state;
    g_task_last_error = error;
    audio_task_publish_snapshot();
}

static void audio_task_reset_media_fields()
{
    g_task_sample_rate_hz = 0;
    g_task_channels = 0;
    g_task_bits_per_sample = 0;
    audio_playback_clock_reset(&g_playback_clock);
    g_task_total_frames = 0;
    g_last_progress_publish_frame = 0;
}

static void audio_request_complete(AudioRequest *request, bool success, esp_err_t result)
{
    if (request == nullptr) {
        return;
    }

    // Pending intent 被新 intent 覆盖时，提交任务和 AudioTask 可能同时尝试完成同一个请求。
    // 完成状态必须幂等，避免同步请求的 binary semaphore 被重复 give。
    bool signal_done = false;
    portENTER_CRITICAL(&g_request_mux);
    if (!request->completed) {
        request->success = success;
        request->result = result;
        request->completed = true;
        signal_done = request->done != nullptr;
    }
    portEXIT_CRITICAL(&g_request_mux);

    if (signal_done) {
        xSemaphoreGive(request->done);
    }
}

static bool audio_command_is_transport_intent(AudioCommandType type)
{
    return type == AudioCommandType::Play || type == AudioCommandType::Seek;
}

#if APP_DIAG_TRANSPORT_INTENT
static const char *audio_command_name(AudioCommandType type)
{
    switch (type) {
        case AudioCommandType::Play: return "PLAY";
        case AudioCommandType::Stop: return "STOP";
        case AudioCommandType::Pause: return "PAUSE";
        case AudioCommandType::Resume: return "RESUME";
        case AudioCommandType::Seek: return "SEEK";
        case AudioCommandType::SetVolume: return "VOLUME";
        case AudioCommandType::SetMute: return "MUTE";
    }
    return "UNKNOWN";
}
#endif

static uint32_t audio_transport_latest_revision()
{
    uint32_t revision = 0;
    portENTER_CRITICAL(&g_request_mux);
    revision = g_latest_transport_intent_revision;
    portEXIT_CRITICAL(&g_request_mux);
    return revision;
}

static bool audio_transport_request_is_latest(const AudioRequest *request)
{
    if (request == nullptr || !audio_command_is_transport_intent(request->type) ||
        request->transport_intent_revision == 0U) {
        return false;
    }
    return request->transport_intent_revision == audio_transport_latest_revision();
}

static bool audio_task_transport_request_superseded(
    const AudioRequest *request,
    const char *stage)
{
    if (audio_transport_request_is_latest(request)) {
        return false;
    }
#if APP_DIAG_TRANSPORT_INTENT
    ESP_LOGI(TAG,
        "INTENT_TRACE: SUPERSEDED request=%lu intent=%lu latest=%lu type=%s stage=%s",
        request != nullptr ? static_cast<unsigned long>(request->request_id) : 0UL,
        request != nullptr ? static_cast<unsigned long>(request->transport_intent_revision) : 0UL,
        static_cast<unsigned long>(audio_transport_latest_revision()),
        request != nullptr ? audio_command_name(request->type) : "UNKNOWN",
        stage != nullptr ? stage : "unknown");
#else
    (void)stage;
#endif
    return true;
}

static void audio_task_remember_first_error(esp_err_t candidate, esp_err_t *first_error)
{
    if (first_error != nullptr && *first_error == ESP_OK && candidate != ESP_OK) {
        *first_error = candidate;
    }
}

static void audio_task_reset_pcm_fade_in()
{
    g_pcm_fade_in_total_frames = 0;
    g_pcm_fade_in_done_frames = 0;
    g_pcm_fade_in_logged_done = true;
}

static void audio_task_begin_pcm_fade_in(const char *reason)
{
    const uint32_t sample_rate =
        g_task_sample_rate_hz > 0 ? g_task_sample_rate_hz : 48000U;
    g_pcm_fade_in_total_frames = static_cast<uint32_t>(
        (static_cast<uint64_t>(sample_rate) * AUDIO_PCM_FADE_IN_MS + 999ULL) / 1000ULL
    );
    if (g_pcm_fade_in_total_frames == 0) {
        g_pcm_fade_in_total_frames = 1;
    }
    g_pcm_fade_in_done_frames = 0;
    g_pcm_fade_in_logged_done = false;
    AUDIO_POP_TRACE_LOG(
        "PCM_FADE_IN_BEGIN reason=%s duration=%lums frames=%lu",
        reason != nullptr ? reason : "unknown",
        static_cast<unsigned long>(AUDIO_PCM_FADE_IN_MS),
        static_cast<unsigned long>(g_pcm_fade_in_total_frames));
}

static void audio_task_apply_pcm_fade_in(int32_t *pcm, size_t frames)
{
    if (
        pcm == nullptr ||
        frames == 0 ||
        g_pcm_fade_in_total_frames == 0 ||
        g_pcm_fade_in_done_frames >= g_pcm_fade_in_total_frames
    ) {
        return;
    }

    for (size_t frame = 0; frame < frames; ++frame) {
        if (g_pcm_fade_in_done_frames >= g_pcm_fade_in_total_frames) {
            break;
        }

        const uint32_t gain_q15 = static_cast<uint32_t>(
            (static_cast<uint64_t>(g_pcm_fade_in_done_frames) * 32768ULL) /
            g_pcm_fade_in_total_frames
        );
        pcm[frame * 2] = static_cast<int32_t>(
            (static_cast<int64_t>(pcm[frame * 2]) * gain_q15) >> 15);
        pcm[frame * 2 + 1] = static_cast<int32_t>(
            (static_cast<int64_t>(pcm[frame * 2 + 1]) * gain_q15) >> 15);
        ++g_pcm_fade_in_done_frames;
    }

    if (
        !g_pcm_fade_in_logged_done &&
        g_pcm_fade_in_done_frames >= g_pcm_fade_in_total_frames
    ) {
        g_pcm_fade_in_logged_done = true;
        AUDIO_POP_TRACE_LOG("PCM_FADE_IN_DONE");
    }
}

static constexpr uint8_t audio_volume_interp_half_db_steps(
    uint8_t percent,
    uint8_t p0,
    uint8_t steps0,
    uint8_t p1,
    uint8_t steps1)
{
    const uint16_t span = static_cast<uint16_t>(p1 - p0);
    if (span == 0U || percent <= p0) {
        return steps0;
    }
    if (percent >= p1) {
        return steps1;
    }

    const uint16_t offset = static_cast<uint16_t>(percent - p0);
    const uint16_t delta = static_cast<uint16_t>(steps0 - steps1);
    return static_cast<uint8_t>(
        steps0 - ((offset * delta + span / 2U) / span));
}

static constexpr uint8_t audio_volume_percent_to_half_db_steps(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }

    // P1.5.3.2R.13：重新分配整个 0~100% 听感区间。
    // 用户确认 -26dB 应落在约 30%；30% 以上不再把主要响度挤在最后 20%，
    // 而是按约 3~4dB / 10% 的节奏平滑走到 100%=0dB。低音量区同时继续抬升。
    //   0%=-100dB（近似静音，真正静音仍由独立 mute 控制）
    //   1%=-54dB, 5%=-48dB, 10%=-42dB, 20%=-33dB, 30%=-26dB
    //   40%=-22dB, 50%=-18dB, 60%=-14dB, 70%=-10dB
    //   80%=-7dB, 90%=-4dB, 100%=0dB
    if (percent == 0U) return 200U;
    if (percent <= 1U) return 108U;
    if (percent <= 5U) return audio_volume_interp_half_db_steps(percent, 1U, 108U, 5U, 96U);
    if (percent <= 10U) return audio_volume_interp_half_db_steps(percent, 5U, 96U, 10U, 84U);
    if (percent <= 20U) return audio_volume_interp_half_db_steps(percent, 10U, 84U, 20U, 66U);
    if (percent <= 30U) return audio_volume_interp_half_db_steps(percent, 20U, 66U, 30U, 52U);
    if (percent <= 40U) return audio_volume_interp_half_db_steps(percent, 30U, 52U, 40U, 44U);
    if (percent <= 50U) return audio_volume_interp_half_db_steps(percent, 40U, 44U, 50U, 36U);
    if (percent <= 60U) return audio_volume_interp_half_db_steps(percent, 50U, 36U, 60U, 28U);
    if (percent <= 70U) return audio_volume_interp_half_db_steps(percent, 60U, 28U, 70U, 20U);
    if (percent <= 80U) return audio_volume_interp_half_db_steps(percent, 70U, 20U, 80U, 14U);
    if (percent <= 90U) return audio_volume_interp_half_db_steps(percent, 80U, 14U, 90U, 8U);
    return audio_volume_interp_half_db_steps(percent, 90U, 8U, 100U, 0U);
}

static_assert(audio_volume_percent_to_half_db_steps(100U) == 0U, "100% 应保持 0dB 满幅");
static_assert(audio_volume_percent_to_half_db_steps(90U) == 8U, "90% 应对应 -4dB");
static_assert(audio_volume_percent_to_half_db_steps(80U) == 14U, "80% 应对应 -7dB");
static_assert(audio_volume_percent_to_half_db_steps(70U) == 20U, "70% 应对应 -10dB");
static_assert(audio_volume_percent_to_half_db_steps(50U) == 36U, "50% 应对应 -18dB");
static_assert(audio_volume_percent_to_half_db_steps(30U) == 52U, "30% 应对应 -26dB");
static_assert(audio_volume_percent_to_half_db_steps(20U) == 66U, "20% 应对应 -33dB");
static_assert(audio_volume_percent_to_half_db_steps(0U) == 200U, "0% 应对应 -100dB 数字衰减");

static esp_err_t audio_task_apply_user_volume()
{
    if (!g_pipeline_headphone_enabled) {
        return ESP_OK;
    }
    return cs43131_set_pcm_volume_attenuation(
        audio_volume_percent_to_half_db_steps(g_task_volume_percent));
}

static esp_err_t audio_task_unmute_when_pcm_ready()
{
    if (!g_pcm_unmute_pending) {
        return ESP_OK;
    }

    // 首次 FLAC process 可能耗时较长，期间 DMA 中原有静音已经播放完。
    // 真正解除静音前重新送一小段全零 PCM，让 BCLK/LRCK 和 ASP 先恢复稳定。
    for (uint8_t i = 0; i < AUDIO_PCM_UNMUTE_PRIME_BLOCKS; ++i) {
        esp_err_t ret =
            i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    // 用户静音与 transport pause 是独立状态。即使当前要求静音，也要完成 prime，
    // 但保持 DAC mute 位，真实 PCM 可以继续流动而不出声。
    esp_err_t ret = ESP_OK;
    if (!g_task_user_muted) {
        ret = cs43131_set_pcm_mute(false);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    g_pcm_unmute_pending = false;
    AUDIO_POP_TRACE_LOG(
        "PCM_UNMUTE_ON_READY_PCM prime_blocks=%u user_muted=%u",
        static_cast<unsigned>(AUDIO_PCM_UNMUTE_PRIME_BLOCKS),
        static_cast<unsigned>(g_task_user_muted));
    return ESP_OK;
}

static esp_err_t audio_task_shutdown_pipeline()
{
    esp_err_t first_error = ESP_OK;

    audio_task_log_ram("shutdown_begin");
    AUDIO_POP_TRACE_LOG(
        "SHUTDOWN_BEGIN rate=%lu hp=%u asp=%u i2s=%u",
        static_cast<unsigned long>(g_task_sample_rate_hz),
        static_cast<unsigned>(g_pipeline_headphone_enabled),
        static_cast<unsigned>(g_pipeline_asp_enabled),
        static_cast<unsigned>(g_pipeline_i2s_started));

    // 先触发 PCM 软斜坡静音，并在整个静音收敛窗口持续发送全零 PCM。
    // CS43131 的 PCM_SZC=soft-ramp 模式不是“写寄存器后立即静音”；官方多个切换序列
    // 都为软斜坡静音预留 150ms。必须等斜坡完成后再置 PDN_HP，否则长时间真实播放后
    // 可能出现 PDN_DONE 未在短窗口内到达，而短测试音却偶尔能立即完成的现象。
    if (g_pipeline_headphone_enabled) {
        esp_err_t mute_ret = cs43131_set_pcm_mute(true);
        audio_task_remember_first_error(mute_ret, &first_error);

        if (mute_ret == ESP_OK) {
            const uint32_t settle_rate = g_task_sample_rate_hz > 0 ? g_task_sample_rate_hz : 48000U;
            const size_t settle_frames = static_cast<size_t>(
                (static_cast<uint64_t>(settle_rate) * AUDIO_PCM_MUTE_SETTLE_MS + 999ULL) / 1000ULL
            );

            if (g_pipeline_i2s_started && i2s_output_is_started()) {
#if APP_DIAG_AUDIO_POP
                ESP_LOGI(TAG, "PCM软静音收敛：保持%lums全零PCM，帧=%u",
                    static_cast<unsigned long>(AUDIO_PCM_MUTE_SETTLE_MS),
                    static_cast<unsigned>(settle_frames));
#endif
                audio_task_remember_first_error(
                    i2s_output_stream_write_silence(settle_frames, AUDIO_I2S_WRITE_TIMEOUT_MS),
                    &first_error);
            } else {
                // 异常清理路径没有 I2S 时仍给模拟静音电路留出收敛时间。
                vTaskDelay(pdMS_TO_TICKS(AUDIO_PCM_MUTE_SETTLE_MS));
            }
        }

        audio_task_remember_first_error(cs43131_power_down_headphone_playback(), &first_error);
        g_pipeline_headphone_enabled = false;
        g_pipeline_asp_enabled = false;
    }

    // 即使 ASP/耳放启动过程中失败，也用统一 finish 将 ASP、XTAL 恢复到安全待机状态。
    if (g_pipeline_clock_prepared || g_pipeline_asp_enabled) {
        audio_task_remember_first_error(cs43131_finish_pcm_playback(), &first_error);
        g_pipeline_clock_prepared = false;
        g_pipeline_asp_enabled = false;
    }

    if (g_pipeline_i2s_started || i2s_output_is_started()) {
        audio_task_remember_first_error(i2s_output_stop(), &first_error);
        g_pipeline_i2s_started = false;
    }

    if (pcm_decoder_is_open(&g_decoder)) {
        pcm_decoder_close(&g_decoder);
    }
    audio_decode_workspace_trim(
        &g_decode_workspace,
        AUDIO_DECODE_WORKSPACE_RETAIN_INPUT_BYTES,
        AUDIO_DECODE_WORKSPACE_RETAIN_PCM_BYTES);

    g_pcm_unmute_pending = false;
    audio_task_reset_pcm_fade_in();

    AUDIO_POP_TRACE_LOG("SHUTDOWN_END ret=%s", esp_err_to_name(first_error));
    audio_task_log_ram("shutdown_end");
    return first_error;
}

static void audio_task_log_index_snapshot(const AudioRequest *request)
{
    if (request == nullptr || !request->has_technical_info) {
        ESP_LOGW(TAG, "INDEX_TRACE: 曲目=%lu 未携带技术索引快照，继续由decoder直接解析",
            request != nullptr ? static_cast<unsigned long>(request->track_index + 1U) : 0UL);
        return;
    }

#if APP_DIAG_AUDIO_INDEX
    const MediaTechnicalInfo &info = request->technical_info;
    ESP_LOGI(TAG,
        "INDEX_TRACE: RECEIVED 曲目=%lu 格式=%s parsed=%u rate=%luHz bits=%u ch=%u duration=%lums total=%llu audio_offset=%llu metadata_end=%llu artwork=%llu+%lu max_block=%u max_frame=%lu bitrate=%lu flags=0x%08lX",
        static_cast<unsigned long>(request->track_index + 1U),
        media_format_name(request->format),
        (info.flags & MEDIA_TECH_PARSED) != 0U ? 1U : 0U,
        static_cast<unsigned long>(info.sample_rate_hz),
        static_cast<unsigned>(info.bits_per_sample),
        static_cast<unsigned>(info.channels),
        static_cast<unsigned long>(info.duration_ms),
        static_cast<unsigned long long>(info.total_frames),
        static_cast<unsigned long long>(info.audio_data_offset),
        static_cast<unsigned long long>(info.metadata_end_offset),
        static_cast<unsigned long long>(info.artwork_offset),
        static_cast<unsigned long>(info.artwork_size),
        static_cast<unsigned>(info.max_block_size),
        static_cast<unsigned long>(info.max_frame_size),
        static_cast<unsigned long>(info.bitrate_kbps),
        static_cast<unsigned long>(info.flags));
#endif
}

static void audio_task_verify_index_snapshot(
    const AudioRequest *request,
    PcmDecoderType decoder_type
)
{
    if (request == nullptr || !request->has_technical_info) {
        return;
    }

    const MediaTechnicalInfo &index = request->technical_info;
    if ((index.flags & MEDIA_TECH_PARSED) == 0U) {
#if APP_DIAG_AUDIO_INDEX
        ESP_LOGI(TAG, "INDEX_TRACE: BASIC 格式=%s 尚无深度技术索引，decoder结果作为真值",
            media_format_name(request->format));
#endif
        return;
    }

    bool mismatch = false;
    if (index.sample_rate_hz != 0U && index.sample_rate_hz != g_decoder.info.sample_rate_hz) {
        mismatch = true;
    }
    if (index.channels != 0U && index.channels != g_decoder.info.channels) {
        mismatch = true;
    }
    if (index.bits_per_sample != 0U && index.bits_per_sample != g_decoder.info.bits_per_sample) {
        mismatch = true;
    }
    // MP3 Simple Decoder 当前不发布 total_frames，因此只有两边都非零时才核对总帧。
    if (
        index.total_frames != 0ULL &&
        g_decoder.info.total_frames != 0ULL &&
        index.total_frames != g_decoder.info.total_frames
    ) {
        mismatch = true;
    }

    if (decoder_type == PcmDecoderType::Flac) {
        if (index.max_block_size != 0U && index.max_block_size != g_decoder.flac.max_block_size) {
            mismatch = true;
        }
        if (index.max_frame_size != 0U && index.max_frame_size != g_decoder.flac.max_frame_size) {
            mismatch = true;
        }
        if (index.audio_data_offset != 0ULL &&
            index.audio_data_offset != g_decoder.flac.audio_data_offset_bytes) {
            mismatch = true;
        }
    } else if (decoder_type == PcmDecoderType::Mp3) {
        if (
            index.bitrate_kbps != 0U &&
            g_decoder.mp3.bitrate != 0U &&
            index.bitrate_kbps != g_decoder.mp3.bitrate
        ) {
            mismatch = true;
        }
    }

    if (mismatch) {
        ESP_LOGW(TAG,
            "INDEX_TRACE: MISMATCH 格式=%s index=%luHz/%ubit/%uch total=%llu decoder=%luHz/%ubit/%uch total=%llu；本次继续以decoder为准",
            media_format_name(request->format),
            static_cast<unsigned long>(index.sample_rate_hz),
            static_cast<unsigned>(index.bits_per_sample),
            static_cast<unsigned>(index.channels),
            static_cast<unsigned long long>(index.total_frames),
            static_cast<unsigned long>(g_decoder.info.sample_rate_hz),
            static_cast<unsigned>(g_decoder.info.bits_per_sample),
            static_cast<unsigned>(g_decoder.info.channels),
            static_cast<unsigned long long>(g_decoder.info.total_frames));
        return;
    }

#if APP_DIAG_AUDIO_INDEX
    ESP_LOGI(TAG,
        "INDEX_TRACE: VERIFIED 格式=%s rate=%luHz bits=%u ch=%u total=%llu audio_offset=%llu",
        media_format_name(request->format),
        static_cast<unsigned long>(g_decoder.info.sample_rate_hz),
        static_cast<unsigned>(g_decoder.info.bits_per_sample),
        static_cast<unsigned>(g_decoder.info.channels),
        static_cast<unsigned long long>(
            g_decoder.info.total_frames != 0ULL ? g_decoder.info.total_frames : index.total_frames),
        static_cast<unsigned long long>(index.audio_data_offset));
#endif
}

static esp_err_t audio_task_start_pcm_pipeline(
    PcmDecoderType decoder_type,
    const char *path,
    const AudioRequest *request,
    bool apply_seek = false,
    uint64_t seek_target_ms = 0,
    PcmSeekResult *out_seek_result = nullptr
)
{
    audio_task_log_ram("before_decoder_open");
    esp_err_t ret = pcm_decoder_open(&g_decoder, decoder_type, path, &g_decode_workspace);
    if (ret != ESP_OK) {
        audio_task_log_ram("decoder_open_failed");
        return ret;
    }
    audio_task_verify_index_snapshot(request, decoder_type);

    PcmSeekResult seek_result = {};
    if (apply_seek) {
        const uint64_t target_frame = g_decoder.info.sample_rate_hz > 0
            ? (seek_target_ms * static_cast<uint64_t>(g_decoder.info.sample_rate_hz)) / 1000ULL
            : 0ULL;
        seek_result.requested_frame = target_frame;
        if (target_frame == 0) {
            // Decoder open 本身已经在曲首完成首块预解码，不再做一次重复 reopen。
            seek_result.actual_frame = 0;
            seek_result.method = PcmSeekMethod::RestartFromBeginning;
        } else {
            ret = pcm_decoder_seek_frame(
                &g_decoder,
                target_frame,
                request != nullptr && request->has_technical_info ? &request->technical_info : nullptr,
                &seek_result);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "SEEK_TRACE: codec定位失败 format=%s target=%llums frame=%llu ret=%s",
                    pcm_decoder_type_name(decoder_type),
                    static_cast<unsigned long long>(seek_target_ms),
                    static_cast<unsigned long long>(target_frame),
                    esp_err_to_name(ret));
                pcm_decoder_close(&g_decoder);
                return ret;
            }
        }
        if (out_seek_result != nullptr) {
            *out_seek_result = seek_result;
        }
#if APP_DIAG_AUDIO_SEEK
        ESP_LOGI(TAG,
            "SEEK_TRACE: PREPARED format=%s method=%s target=%llums requested_frame=%llu actual_frame=%llu source_offset=%llu",
            pcm_decoder_type_name(decoder_type),
            pcm_seek_method_name(seek_result.method),
            static_cast<unsigned long long>(seek_target_ms),
            static_cast<unsigned long long>(seek_result.requested_frame),
            static_cast<unsigned long long>(seek_result.actual_frame),
            static_cast<unsigned long long>(seek_result.source_offset));
#endif
    }

    // decoder/source open 可能需要几十到数百毫秒。若用户在这期间又选择了更新目标，
    // 立即关闭旧 decoder，不再为已经过期的 Track 启动 I2S/DAC。
    if (request != nullptr && audio_task_transport_request_superseded(request, "after_decoder_open")) {
        pcm_decoder_close(&g_decoder);
        audio_decode_workspace_trim(
            &g_decode_workspace,
            AUDIO_DECODE_WORKSPACE_RETAIN_INPUT_BYTES,
            AUDIO_DECODE_WORKSPACE_RETAIN_PCM_BYTES);
        return ESP_ERR_INVALID_STATE;
    }

    audio_task_log_ram("after_decoder_open");
#if APP_DIAG_AUDIO_WORKSPACE
    ESP_LOGI(TAG,
        "WORKSPACE_TRACE: codec=%s input=%uB pcm=%uB total=%uB（PSRAM共享，FLAC ring独立）",
        pcm_decoder_type_name(decoder_type),
        static_cast<unsigned>(g_decode_workspace.input_capacity),
        static_cast<unsigned>(g_decode_workspace.decoded_capacity),
        static_cast<unsigned>(audio_decode_workspace_total_bytes(&g_decode_workspace)));
#endif

    g_task_sample_rate_hz = g_decoder.info.sample_rate_hz;
    g_task_channels = g_decoder.info.channels;
    g_task_bits_per_sample = g_decoder.info.bits_per_sample;
    audio_playback_clock_reset(&g_playback_clock, g_decoder.info.sample_rate_hz);
    if (apply_seek) {
        audio_playback_clock_seek(&g_playback_clock, seek_result.actual_frame);
    }
    g_task_total_frames = g_decoder.info.total_frames;
    if (
        g_task_total_frames == 0 &&
        request != nullptr &&
        request->has_technical_info &&
        (request->technical_info.flags & MEDIA_TECH_PARSED) != 0U &&
        request->technical_info.sample_rate_hz == g_decoder.info.sample_rate_hz &&
        request->technical_info.total_frames > 0
    ) {
        g_task_total_frames = request->technical_info.total_frames;
#if APP_DIAG_AUDIO_CLOCK
        ESP_LOGI(TAG,
            "CLOCK_TRACE: TOTAL_SOURCE=index total=%llu duration=%lums estimated=%u",
            static_cast<unsigned long long>(g_task_total_frames),
            static_cast<unsigned long>(request->technical_info.duration_ms),
            static_cast<unsigned>(
                (request->technical_info.flags & MEDIA_TECH_DURATION_ESTIMATED) != 0U));
#endif
    } else {
#if APP_DIAG_AUDIO_CLOCK
        ESP_LOGI(TAG, "CLOCK_TRACE: TOTAL_SOURCE=decoder total=%llu",
            static_cast<unsigned long long>(g_task_total_frames));
#endif
    }
    g_last_progress_publish_frame = g_playback_clock.submitted_frames;
    g_ram_trace_first_pcm_done = false;
    g_ram_trace_steady_5s_done = false;
    // P1.5.2R.3：新曲/Seek 建立新 PCM pipeline 时先清空旧 FFT 快照。
    // UI 会同时核对 playback_revision + track_index，绝不会把上一首的 PCM 当成当前频谱。
    audio_spectrum_snapshot_reset(
        g_task_playback_revision,
        g_task_track_index,
        g_task_sample_rate_hz);
    audio_task_publish_snapshot();

    AUDIO_POP_TRACE_LOG(
        "START_BEGIN format=%s rate=%lu bits=%u channels=%u",
        pcm_decoder_type_name(decoder_type),
        static_cast<unsigned long>(g_task_sample_rate_hz),
        static_cast<unsigned>(g_task_bits_per_sample),
        static_cast<unsigned>(g_task_channels));

    // 从这一刻开始即使 CS43131 准备过程中途失败，也必须执行 finish 清理。
    g_pipeline_clock_prepared = true;
    ret = cs43131_prepare_pcm_playback_32bit(g_task_sample_rate_hz);
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }
    if (request != nullptr && audio_task_transport_request_superseded(request, "after_dac_prepare")) {
        audio_task_shutdown_pipeline();
        return ESP_ERR_INVALID_STATE;
    }

    ret = i2s_output_stream_start_32bit(g_task_sample_rate_hz);
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }
    g_pipeline_i2s_started = true;
    AUDIO_POP_TRACE_LOG(
        "I2S_START rate=%lu bclk=%lu",
        static_cast<unsigned long>(g_task_sample_rate_hz),
        static_cast<unsigned long>(g_task_sample_rate_hz * 64UL));
    if (request != nullptr && audio_task_transport_request_superseded(request, "after_i2s_start")) {
        audio_task_shutdown_pipeline();
        return ESP_ERR_INVALID_STATE;
    }

    // 先给 I2S DMA 填入全零 PCM，再开启 CS43131 ASP，避免 ASP 上电瞬间面对不稳定时钟。
    ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }

    ret = cs43131_enable_asp_input();
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }
    g_pipeline_asp_enabled = true;

    // 保持约 15ms 全零 PCM，让 ASP 锁定 BCLK/LRCK。
    for (int i = 0; i < 3; ++i) {
        ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            audio_task_shutdown_pipeline();
            return ret;
        }
    }

    uint8_t asp_status = 0;
    ret = cs43131_read_asp_status(&asp_status); // 清启动阶段 sticky 状态
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }

    for (int i = 0; i < 2; ++i) {
        ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            audio_task_shutdown_pipeline();
            return ret;
        }
    }

    asp_status = 0;
    ret = cs43131_read_asp_status(&asp_status);
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }
#if APP_DIAG_AUDIO_POP
    ESP_LOGI(TAG, "%s起播前 ASP 稳定状态：0x%02X", pcm_decoder_type_name(decoder_type), asp_status);
#endif
    AUDIO_POP_TRACE_LOG("ASP_STABLE status=0x%02X", asp_status);
    if ((asp_status & 0xF8U) != 0) {
        ESP_LOGE(TAG, "ASP 时序异常，拒绝开启耳放：INT_STATUS2=0x%02X", asp_status);
        audio_task_shutdown_pipeline();
        return ESP_FAIL;
    }
    if (request != nullptr && audio_task_transport_request_superseded(request, "before_headphone_enable")) {
        audio_task_shutdown_pipeline();
        return ESP_ERR_INVALID_STATE;
    }

    // 预先标记“耳放可能已被触及”，保证寄存器写到一半失败时仍会尝试安全掉电。
    g_pipeline_headphone_enabled = true;
    ret = cs43131_prepare_headphone_playback_low_volume();
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }
    ret = audio_task_apply_user_volume();
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }
    AUDIO_POP_TRACE_LOG("HP_ENABLE volume=%u muted=%u",
        static_cast<unsigned>(g_task_volume_percent),
        static_cast<unsigned>(g_task_user_muted));

    // 耳放 pop-free 上电后继续送约 20ms 静音，再解除 PCM 手动静音。
    for (int i = 0; i < 4; ++i) {
        ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            audio_task_shutdown_pipeline();
            return ret;
        }
    }

    if (request != nullptr && audio_task_transport_request_superseded(request, "before_pipeline_commit")) {
        audio_task_shutdown_pipeline();
        return ESP_ERR_INVALID_STATE;
    }

    // 耳放保持手动静音，等第一块真实 PCM 已经解码完成后再解除。
    // 这样即使 FLAC 首次 process 发生秒级延迟，也不会出现“先开声、后等数据”的窗口。
    audio_task_begin_pcm_fade_in("start");
    g_pcm_unmute_pending = true;
    AUDIO_POP_TRACE_LOG("PCM_UNMUTE_ARMED reason=start");
    audio_task_log_ram("pipeline_playing");

    const uint64_t duration_ms = g_task_sample_rate_hz > 0 && g_task_total_frames > 0
        ? (g_task_total_frames * 1000ULL) / g_task_sample_rate_hz
        : 0;
    ESP_LOGI(TAG, "PCM硬件链路已开启：格式=%s %luHz / %ubit / %u声道，时长约=%llums，用户音量=%u%% mute=%u，等待首PCM帧解除静音",
        pcm_decoder_type_name(decoder_type),
        static_cast<unsigned long>(g_task_sample_rate_hz),
        static_cast<unsigned>(g_task_bits_per_sample),
        static_cast<unsigned>(g_task_channels),
        static_cast<unsigned long long>(duration_ms),
        static_cast<unsigned>(g_task_volume_percent),
        static_cast<unsigned>(g_task_user_muted));
    return ESP_OK;
}

static AudioFaultStage audio_fault_stage_from_name(const char *stage)
{
    if (stage == nullptr) return AudioFaultStage::Unknown;
    if (strcmp(stage, "读取PCM") == 0) return AudioFaultStage::ReadPcm;
    if (strcmp(stage, "PCM无进度") == 0) return AudioFaultStage::PcmNoProgress;
    if (strcmp(stage, "首PCM解除静音") == 0) return AudioFaultStage::FirstPcmUnmute;
    if (strcmp(stage, "I2S发送") == 0) return AudioFaultStage::I2sWrite;
    if (strcmp(stage, "暂停静音") == 0) return AudioFaultStage::PauseMute;
    if (strcmp(stage, "恢复播放") == 0) return AudioFaultStage::ResumePlayback;
    return AudioFaultStage::Unknown;
}

static const char *audio_fault_stage_name(AudioFaultStage stage)
{
    switch (stage) {
        case AudioFaultStage::ReadPcm: return "读取PCM";
        case AudioFaultStage::PcmNoProgress: return "PCM无进度";
        case AudioFaultStage::FirstPcmUnmute: return "首PCM解除静音";
        case AudioFaultStage::I2sWrite: return "I2S发送";
        case AudioFaultStage::PauseMute: return "暂停静音";
        case AudioFaultStage::ResumePlayback: return "恢复播放";
        case AudioFaultStage::EofDrain: return "EOF排空";
        case AudioFaultStage::None: return "无";
        case AudioFaultStage::Unknown:
        default: return "未知";
    }
}

static void audio_task_capture_fault(esp_err_t error, const char *stage)
{
    AudioFaultSnapshot snapshot = {};
    snapshot.valid = true;
    snapshot.stage = audio_fault_stage_from_name(stage);
    snapshot.error = error;
    snapshot.playback_revision = g_task_playback_revision;
    snapshot.track_index = g_task_track_index;
    snapshot.format = g_task_format;
    snapshot.sample_rate_hz = g_task_sample_rate_hz;
    snapshot.channels = g_task_channels;
    snapshot.bits_per_sample = g_task_bits_per_sample;
    snapshot.position_frames = g_playback_clock.submitted_frames;
    snapshot.decoder_position_frames = g_playback_clock.decoder_frames;

    if (g_task_format == MediaFormat::FLAC && flac_decoder_is_open(&g_decoder.flac)) {
        FlacPrefetchRuntimeSnapshot flac = {};
        if (flac_decoder_get_prefetch_runtime(&g_decoder.flac, &flac)) {
            snapshot.flac_prefetch_active = flac.active;
            snapshot.flac_prefetch_io_error = flac.io_error;
            snapshot.flac_prefetch_eof = flac.eof;
            snapshot.flac_prefetch_pressure = flac.pressure_active;
            snapshot.flac_qos_level = static_cast<uint8_t>(flac.qos_level);
            snapshot.flac_ring_buffered_bytes = flac.buffered_bytes;
            snapshot.flac_ring_capacity_bytes = flac.capacity_bytes;
            snapshot.flac_ring_min_buffered_bytes = flac.min_buffered_bytes;
            snapshot.flac_emergency_entries = flac.emergency_entries;
            snapshot.flac_recovered_count = flac.recovered_count;
            snapshot.flac_max_consecutive_reads = flac.max_consecutive_reads;
        }
    }

    snapshot.internal_free_bytes = static_cast<uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    snapshot.internal_min_bytes = static_cast<uint32_t>(
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    snapshot.internal_largest_bytes = static_cast<uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    snapshot.dma_free_bytes = static_cast<uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    snapshot.psram_free_bytes = static_cast<uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    portENTER_CRITICAL(&g_fault_snapshot_mux);
    snapshot.sequence = g_fault_snapshot.sequence + 1U;
    snapshot.fault_count = ++g_fault_count;
    g_fault_snapshot = snapshot;
    portEXIT_CRITICAL(&g_fault_snapshot_mux);

    // 真实故障只打印一次短快照；若当时未开串口，system_loop 会在 Error 状态低频重报。
    ESP_LOGE(TAG,
        "R.36.2.2 AUDIO_FAULT：count=%lu stage=%s err=%s track=%lu rev=%lu rate=%luHz pos=%lluf ring=%lu/%luB min=%luB qos=%u pressure=%u ioerr=%u emergency=%lu/%lu RAM(internal/dma/psram)=%lu/%lu/%lu",
        static_cast<unsigned long>(snapshot.fault_count),
        audio_fault_stage_name(snapshot.stage),
        esp_err_to_name(snapshot.error),
        static_cast<unsigned long>(snapshot.track_index),
        static_cast<unsigned long>(snapshot.playback_revision),
        static_cast<unsigned long>(snapshot.sample_rate_hz),
        static_cast<unsigned long long>(snapshot.position_frames),
        static_cast<unsigned long>(snapshot.flac_ring_buffered_bytes),
        static_cast<unsigned long>(snapshot.flac_ring_capacity_bytes),
        static_cast<unsigned long>(snapshot.flac_ring_min_buffered_bytes),
        static_cast<unsigned>(snapshot.flac_qos_level),
        static_cast<unsigned>(snapshot.flac_prefetch_pressure),
        static_cast<unsigned>(snapshot.flac_prefetch_io_error),
        static_cast<unsigned long>(snapshot.flac_emergency_entries),
        static_cast<unsigned long>(snapshot.flac_recovered_count),
        static_cast<unsigned long>(snapshot.internal_free_bytes),
        static_cast<unsigned long>(snapshot.dma_free_bytes),
        static_cast<unsigned long>(snapshot.psram_free_bytes));
}

static void audio_task_fail_stream(esp_err_t error, const char *stage)
{
    audio_task_capture_fault(error, stage);
    ESP_LOGE(TAG, "%s播放失败：阶段=%s，错误=%s",
        media_format_name(g_task_format),
        stage != nullptr ? stage : "未知",
        esp_err_to_name(error));
    esp_err_t cleanup_error = audio_task_shutdown_pipeline();
    if (error == ESP_OK) {
        error = cleanup_error != ESP_OK ? cleanup_error : ESP_FAIL;
    }
    audio_task_set_state(AudioPlaybackState::Error, error);
}

static void audio_task_finish_stream()
{
    // position_frames 保持 I2S 已提交真实 PCM 的真值，不在 EOF 时强行改成 metadata total。
    // total 未知时才用真实输出帧回填，这样进度/歌词/恢复始终建立在同一个播放时钟上。
    if (g_task_total_frames == 0) {
        g_task_total_frames = g_playback_clock.submitted_frames;
    }
    audio_task_publish_snapshot();

#if APP_DIAG_AUDIO_CLOCK
    const int64_t total_delta = static_cast<int64_t>(g_playback_clock.submitted_frames) -
        static_cast<int64_t>(g_task_total_frames);
    ESP_LOGI(TAG,
        "%s PCM 数据播放完成：CLOCK_TRACE submitted=%llu decoder=%llu total=%llu delta=%lld time=%llums",
        media_format_name(g_task_format),
        static_cast<unsigned long long>(g_playback_clock.submitted_frames),
        static_cast<unsigned long long>(g_playback_clock.decoder_frames),
        static_cast<unsigned long long>(g_task_total_frames),
        static_cast<long long>(total_delta),
        static_cast<unsigned long long>(audio_playback_clock_position_ms(&g_playback_clock)));
#else
    ESP_LOGI(TAG, "%s 播放完成：时长=%llums",
        media_format_name(g_task_format),
        static_cast<unsigned long long>(audio_playback_clock_position_ms(&g_playback_clock)));
#endif

    // i2s_channel_write() 返回代表 PCM 已复制进 DMA，不等于最后一个样本已经从引脚送出。
    // 文件尾先追加 4 个静音块，让已排队的最后 PCM 块自然播放完，再执行 DAC 软静音/掉电。
    // 这样不会因为 EOF 清理过快而截掉歌曲最后几毫秒。
    if (g_pipeline_i2s_started && i2s_output_is_started()) {
        esp_err_t drain_ret = i2s_output_stream_write_silence(
            AUDIO_STREAM_FRAMES * 4,
            AUDIO_I2S_WRITE_TIMEOUT_MS
        );
        if (drain_ret != ESP_OK) {
            ESP_LOGE(TAG, "%s 尾部静音排空失败：%s",
                media_format_name(g_task_format), esp_err_to_name(drain_ret));
            audio_task_shutdown_pipeline();
            audio_task_set_state(AudioPlaybackState::Error, drain_ret);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_err_t ret = audio_task_shutdown_pipeline();
    if (ret != ESP_OK) {
        audio_task_set_state(AudioPlaybackState::Error, ret);
        return;
    }
    audio_task_set_state(AudioPlaybackState::Finished, ESP_OK);
}

static void audio_task_service_pcm_playback()
{
    if (g_task_state != AudioPlaybackState::Playing || !pcm_decoder_is_open(&g_decoder)) {
        return;
    }

    size_t frames = 0;
    esp_err_t ret = pcm_decoder_read_pcm32(
        &g_decoder,
        g_pcm_block,
        AUDIO_STREAM_FRAMES,
        &frames
    );
    if (ret != ESP_OK) {
        audio_task_fail_stream(ret, "读取PCM");
        return;
    }

    // decoder 位置只用于诊断；正式播放时钟必须等真实 PCM 成功进入 I2S 后才推进。
    audio_playback_clock_note_decoder(
        &g_playback_clock, pcm_decoder_position_frames(&g_decoder));

    if (frames == 0) {
        if (pcm_decoder_is_eof(&g_decoder)) {
            audio_task_finish_stream();
        } else {
            audio_task_fail_stream(ESP_FAIL, "PCM无进度");
        }
        return;
    }

    audio_task_apply_pcm_fade_in(g_pcm_block, frames);

    // 第一块真实 PCM 已经在 g_pcm_block 中准备好；先恢复稳定的零 PCM 时钟，
    // 再解除 CS43131 手动静音，随后发送从 0 开始淡入的真实 PCM。
    ret = audio_task_unmute_when_pcm_ready();
    if (ret != ESP_OK) {
        audio_task_fail_stream(ret, "首PCM解除静音");
        return;
    }

    ret = i2s_output_stream_write_pcm32(g_pcm_block, frames, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        audio_task_fail_stream(ret, "I2S发送");
        return;
    }

    // i2s_output_stream_write_pcm32() 成功表示整块真实 PCM 已复制进 DMA。
    // 启动 prime、暂停保持时钟和 EOF drain 都走 silence API，因此不会污染该计数。
    audio_playback_clock_commit_pcm(&g_playback_clock, frames);

    // P1.5.2R.3：只在“真实 PCM 已成功进入 I2S DMA”之后旁路采样。
    // AudioTask 只负责约20Hz抽取/降采样并填充256点mono窗；FFT在Core1/P1任务执行。
    audio_spectrum_snapshot_publish_pcm(
        g_pcm_block,
        frames,
        g_task_playback_revision,
        g_task_track_index,
        g_task_sample_rate_hz,
        g_playback_clock.submitted_frames);

    // FLAC/simple-decoder 可能在真正 process 首帧时才完成内部工作区的延迟分配。
    // 这里只采样一次首个 PCM 块和一次 5 秒稳定态，避免 RAM 诊断日志进入实时热循环。
#if APP_DIAG_AUDIO_CLOCK || APP_DIAG_AUDIO_RAM
    if (!g_ram_trace_first_pcm_done) {
        g_ram_trace_first_pcm_done = true;
#if APP_DIAG_AUDIO_CLOCK
        ESP_LOGI(TAG,
            "CLOCK_TRACE: FIRST_PCM submitted=%llu decoder=%llu delta=%lld",
            static_cast<unsigned long long>(g_playback_clock.submitted_frames),
            static_cast<unsigned long long>(g_playback_clock.decoder_frames),
            static_cast<long long>(
                static_cast<int64_t>(g_playback_clock.decoder_frames) -
                static_cast<int64_t>(g_playback_clock.submitted_frames)));
#endif
        audio_task_log_ram("first_pcm");
    }
    if (
        !g_ram_trace_steady_5s_done &&
        audio_playback_clock_position_ms(&g_playback_clock) >= 5000ULL
    ) {
        g_ram_trace_steady_5s_done = true;
#if APP_DIAG_AUDIO_CLOCK
        ESP_LOGI(TAG,
            "CLOCK_TRACE: STEADY submitted=%llu decoder=%llu delta=%lld time=%llums",
            static_cast<unsigned long long>(g_playback_clock.submitted_frames),
            static_cast<unsigned long long>(g_playback_clock.decoder_frames),
            static_cast<long long>(
                static_cast<int64_t>(g_playback_clock.decoder_frames) -
                static_cast<int64_t>(g_playback_clock.submitted_frames)),
            static_cast<unsigned long long>(audio_playback_clock_position_ms(&g_playback_clock)));
#endif
        audio_task_log_ram("steady_5s");
    }
#endif

    const uint64_t publish_interval = g_task_sample_rate_hz >= 4
        ? g_task_sample_rate_hz / 4U
        : 1;
    if (g_playback_clock.submitted_frames - g_last_progress_publish_frame >= publish_interval) {
        g_last_progress_publish_frame = g_playback_clock.submitted_frames;
        audio_task_publish_snapshot();
    }
}

static void audio_task_service_pause_silence()
{
    if (g_task_state != AudioPlaybackState::Paused || !g_pipeline_i2s_started) {
        return;
    }

    // 暂停时不推进文件，但继续送零 PCM，让 CS43131 Slave 始终看到连续 BCLK/LRCK。
    esp_err_t ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        audio_task_fail_stream(ret, "暂停静音");
    }
}

static bool audio_task_pipeline_has_resources()
{
    return
        g_pipeline_clock_prepared ||
        g_pipeline_i2s_started ||
        g_pipeline_asp_enabled ||
        g_pipeline_headphone_enabled ||
        i2s_output_is_started() ||
        pcm_decoder_is_open(&g_decoder);
}

static PcmDecoderType audio_decoder_type_for_format(MediaFormat format)
{
    switch (format) {
        case MediaFormat::WAV: return PcmDecoderType::Wav;
        case MediaFormat::FLAC: return PcmDecoderType::Flac;
        case MediaFormat::MP3: return PcmDecoderType::Mp3;
        default: return PcmDecoderType::None;
    }
}

static void audio_task_handle_play(AudioRequest *request)
{
    const char *path = audio_request_path(request);

    // 队列中的 transport 唤醒项可能已经被更新 intent 覆盖；这种请求不能触碰当前 pipeline。
    if (audio_task_transport_request_superseded(request, "play_dequeue")) {
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    // 新播放请求只在旧 pipeline 仍持有资源时执行 shutdown。
    // EOF 已经完整收尾、或显式 Stop 后直接起播时跳过空 shutdown，减少切歌延迟和日志噪声。
    if (audio_task_pipeline_has_resources()) {
        esp_err_t cleanup_ret = audio_task_shutdown_pipeline();
        if (cleanup_ret != ESP_OK) {
            g_task_last_request_id = request->request_id;
            audio_task_set_state(AudioPlaybackState::Error, cleanup_ret);
            audio_request_complete(request, false, cleanup_ret);
            return;
        }
    } else {
        AUDIO_POP_TRACE_LOG("PLAY_REUSE_IDLE_PIPELINE no_shutdown");
    }

    // 150ms soft-ramp shutdown 本身足以让用户产生新的 NEXT/选歌意图。
    // 旧请求在关闭完旧链路后必须再次确认，否则会无意义地打开已过期文件。
    if (audio_task_transport_request_superseded(request, "after_previous_shutdown")) {
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    g_task_last_request_id = request->request_id;
    g_task_track_index = request->track_index;
    g_task_format = request->format;
    g_task_last_seek_request_id = 0;
    g_task_last_seek_target_ms = 0;
    audio_task_reset_media_fields();
    audio_task_advance_playback_revision();
    audio_task_set_state(AudioPlaybackState::Preparing);

    ESP_LOGI(TAG, "收到播放请求：请求=%lu 世代=%lu 曲目=%lu 格式=%s 路径=%s",
        static_cast<unsigned long>(request->request_id),
        static_cast<unsigned long>(g_task_playback_revision),
        static_cast<unsigned long>(request->track_index + 1),
        media_format_name(request->format),
        path != nullptr ? path : "(空)");
    audio_task_log_index_snapshot(request);

    if (path == nullptr || path[0] == '\0') {
        audio_task_set_state(AudioPlaybackState::Error, ESP_ERR_INVALID_ARG);
        audio_request_complete(request, false, ESP_ERR_INVALID_ARG);
        return;
    }

    const PcmDecoderType decoder_type = audio_decoder_type_for_format(request->format);
    if (decoder_type == PcmDecoderType::None) {
        ESP_LOGW(TAG, "Stage 9.5.1 当前统一PCM Core 已接入 WAV/FLAC/MP3；%s 解码器尚未接入",
            media_format_name(request->format));
        audio_task_set_state(AudioPlaybackState::Error, ESP_ERR_NOT_SUPPORTED);
        audio_request_complete(request, false, ESP_ERR_NOT_SUPPORTED);
        return;
    }

    esp_err_t ret = audio_task_start_pcm_pipeline(decoder_type, path, request);
    if (ret != ESP_OK) {
        if (!audio_transport_request_is_latest(request)) {
            // 最新 intent 已经在等待，旧请求失败属于主动取消而不是播放器错误。
            // 保留 Preparing 短暂过渡状态，下一条最新 Play 会立即接管并发布自己的 Track。
            audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
            return;
        }
        audio_task_set_state(AudioPlaybackState::Error, ret);
        audio_request_complete(request, false, ret);
        return;
    }

    // pipeline 完整建立后再做一次最终提交检查；若用户恰好在最后一个硬件阶段更新目标，
    // 旧链路不会进入真实 PCM 播放，而是安全关闭后交给最新 intent。
    if (audio_task_transport_request_superseded(request, "play_commit")) {
        audio_task_shutdown_pipeline();
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    audio_task_set_state(AudioPlaybackState::Playing, ESP_OK);
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_seek(AudioRequest *request)
{
    if (request == nullptr) {
        return;
    }
    g_task_last_request_id = request->request_id;

    if (audio_task_transport_request_superseded(request, "seek_dequeue")) {
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }
    if (request->expected_playback_revision == 0U ||
        request->expected_playback_revision != g_task_playback_revision ||
        request->track_index != g_task_track_index || request->format != g_task_format) {
#if APP_DIAG_AUDIO_SEEK
        ESP_LOGI(TAG,
            "SEEK_TRACE: 忽略过期 Seek request=%lu expected_rev=%lu current_rev=%lu req_track=%lu current_track=%lu",
            static_cast<unsigned long>(request->request_id),
            static_cast<unsigned long>(request->expected_playback_revision),
            static_cast<unsigned long>(g_task_playback_revision),
            static_cast<unsigned long>(request->track_index),
            static_cast<unsigned long>(g_task_track_index));
#endif
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }
    if (g_task_state != AudioPlaybackState::Playing &&
        g_task_state != AudioPlaybackState::Paused &&
        g_task_state != AudioPlaybackState::Seeking) {
        ESP_LOGW(TAG, "SEEK_TRACE: 当前状态不允许 Seek：%s", audio_playback_state_name_cn(g_task_state));
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    const PcmDecoderType decoder_type = audio_decoder_type_for_format(request->format);
    if (decoder_type == PcmDecoderType::None) {
        audio_request_complete(request, false, ESP_ERR_NOT_SUPPORTED);
        return;
    }

    uint64_t target_ms = request->seek_target_ms;
    const uint64_t known_total_frames = g_task_total_frames;
    const uint32_t known_rate = g_task_sample_rate_hz;
    if (known_total_frames > 0 && known_rate > 0) {
        const uint64_t duration_ms = (known_total_frames * 1000ULL) / known_rate;
        if (duration_ms > 0 && target_ms >= duration_ms) {
            target_ms = duration_ms - 1ULL;
        }
    }
    const uint64_t target_frame = known_rate > 0
        ? (target_ms * static_cast<uint64_t>(known_rate)) / 1000ULL : 0ULL;
    if (!pcm_decoder_seek_supported(&g_decoder, target_frame)) {
        ESP_LOGW(TAG,
            "SEEK_TRACE: 当前文件/后端不支持该 Seek format=%s target=%llums；保持原播放位置",
            pcm_decoder_type_name(decoder_type),
            static_cast<unsigned long long>(target_ms));
        audio_request_complete(request, false, ESP_ERR_NOT_SUPPORTED);
        return;
    }
    if (decoder_type == PcmDecoderType::Mp3 &&
        (!request->has_technical_info ||
         (request->technical_info.flags & MEDIA_TECH_PARSED) == 0U)) {
        ESP_LOGW(TAG, "SEEK_TRACE: MP3 缺少技术索引，拒绝无依据的字节定位");
        audio_request_complete(request, false, ESP_ERR_NOT_SUPPORTED);
        return;
    }

    const bool was_paused = g_task_state == AudioPlaybackState::Paused;
    audio_task_set_state(AudioPlaybackState::Seeking, ESP_OK);
#if APP_DIAG_AUDIO_SEEK
    ESP_LOGI(TAG,
        "SEEK_TRACE: BEGIN request=%lu rev=%lu track=%lu format=%s from=%llums target=%llums paused=%u",
        static_cast<unsigned long>(request->request_id),
        static_cast<unsigned long>(g_task_playback_revision),
        static_cast<unsigned long>(g_task_track_index),
        media_format_name(g_task_format),
        static_cast<unsigned long long>(audio_playback_clock_position_ms(&g_playback_clock)),
        static_cast<unsigned long long>(target_ms),
        static_cast<unsigned>(was_paused));
#endif

    esp_err_t ret = audio_task_shutdown_pipeline();
    if (ret != ESP_OK) {
        audio_task_set_state(AudioPlaybackState::Error, ret);
        audio_request_complete(request, false, ret);
        return;
    }

    if (audio_task_transport_request_superseded(request, "seek_after_shutdown")) {
        // 新 Seek/Play 已经成为最新意图；保持当前 Track/revision，便于后续同 Track Seek 继续接管。
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    PcmSeekResult seek_result = {};
    ret = audio_task_start_pcm_pipeline(
        decoder_type,
        audio_request_path(request),
        request,
        true,
        target_ms,
        &seek_result);
    if (ret != ESP_OK) {
        if (!audio_transport_request_is_latest(request)) {
            audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
            return;
        }
        audio_task_set_state(AudioPlaybackState::Error, ret);
        audio_request_complete(request, false, ret);
        return;
    }

    if (audio_task_transport_request_superseded(request, "seek_commit")) {
        audio_task_shutdown_pipeline();
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    ++g_task_seek_revision;
    if (g_task_seek_revision == 0U) {
        ++g_task_seek_revision;
    }
    g_task_last_seek_request_id = request->request_id;
    g_task_last_seek_target_ms = target_ms;

    if (was_paused) {
        // start pipeline 尚未发送真实 PCM，因此 DAC 仍保持手动静音；Paused 下继续送零维持时钟。
        g_pcm_unmute_pending = false;
        audio_task_reset_pcm_fade_in();
        audio_task_set_state(AudioPlaybackState::Paused, ESP_OK);
    } else {
        audio_task_set_state(AudioPlaybackState::Playing, ESP_OK);
    }

#if APP_DIAG_AUDIO_SEEK
    ESP_LOGI(TAG,
        "SEEK_TRACE: DONE request=%lu method=%s requested=%llums actual=%llums frame=%llu source_offset=%llu state=%s",
        static_cast<unsigned long>(request->request_id),
        pcm_seek_method_name(seek_result.method),
        static_cast<unsigned long long>(request->seek_target_ms),
        static_cast<unsigned long long>(audio_playback_clock_position_ms(&g_playback_clock)),
        static_cast<unsigned long long>(seek_result.actual_frame),
        static_cast<unsigned long long>(seek_result.source_offset),
        audio_playback_state_name_cn(g_task_state));
#endif
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_stop(AudioRequest *request)
{
    g_task_last_request_id = request->request_id;
    esp_err_t ret = audio_task_shutdown_pipeline();
    audio_task_advance_playback_revision();
    g_task_track_index = UINT32_MAX;
    g_task_format = MediaFormat::Unknown;
    g_task_last_seek_request_id = 0;
    g_task_last_seek_target_ms = 0;
    audio_task_reset_media_fields();
    audio_spectrum_snapshot_reset(g_task_playback_revision, UINT32_MAX, 0U);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "停止播放时清理音频链路失败：%s", esp_err_to_name(ret));
        audio_task_set_state(AudioPlaybackState::Error, ret);
        audio_request_complete(request, false, ret);
        return;
    }

    audio_task_set_state(AudioPlaybackState::Stopped, ESP_OK);
    ESP_LOGI(TAG, "停止请求完成：请求=%lu，新播放世代=%lu",
        static_cast<unsigned long>(request->request_id),
        static_cast<unsigned long>(g_task_playback_revision));
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_pause(AudioRequest *request)
{
    g_task_last_request_id = request->request_id;
    if (g_task_state != AudioPlaybackState::Playing || !g_pipeline_headphone_enabled) {
        ESP_LOGW(TAG, "暂停请求被拒绝：当前状态=%s",
            audio_playback_state_name_cn(g_task_state));
        audio_task_set_state(g_task_state, ESP_ERR_INVALID_STATE);
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    AUDIO_POP_TRACE_LOG("PAUSE_MUTE_BEGIN frame=%llu",
        static_cast<unsigned long long>(g_playback_clock.submitted_frames));
    esp_err_t ret = cs43131_set_pcm_mute(true);
    if (ret != ESP_OK) {
        audio_task_fail_stream(ret, "暂停静音");
        audio_request_complete(request, false, ret);
        return;
    }
    AUDIO_POP_TRACE_LOG("PAUSE_MUTE_DONE");
    g_pcm_unmute_pending = false;
    audio_task_reset_pcm_fade_in();

    audio_task_set_state(AudioPlaybackState::Paused, ESP_OK);
    ESP_LOGI(TAG, "播放已暂停：帧=%llu/%llu，I2S继续发送静音保持时钟",
        static_cast<unsigned long long>(g_playback_clock.submitted_frames),
        static_cast<unsigned long long>(g_task_total_frames));
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_resume(AudioRequest *request)
{
    g_task_last_request_id = request->request_id;
    if (g_task_state != AudioPlaybackState::Paused || !g_pipeline_headphone_enabled) {
        ESP_LOGW(TAG, "恢复请求被拒绝：当前状态=%s",
            audio_playback_state_name_cn(g_task_state));
        audio_task_set_state(g_task_state, ESP_ERR_INVALID_STATE);
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    AUDIO_POP_TRACE_LOG("RESUME_ZERO_PRIME_BEGIN");
    esp_err_t ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES * 2, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (ret == ESP_OK) {
        AUDIO_POP_TRACE_LOG("RESUME_ZERO_PRIME_DONE");
    }
    if (ret != ESP_OK) {
        audio_task_fail_stream(ret, "恢复播放");
        audio_request_complete(request, false, ret);
        return;
    }
    audio_task_begin_pcm_fade_in("resume");
    g_pcm_unmute_pending = true;
    AUDIO_POP_TRACE_LOG("RESUME_UNMUTE_ARMED");

    audio_task_set_state(AudioPlaybackState::Playing, ESP_OK);
    ESP_LOGI(TAG, "播放已恢复：从帧=%llu继续",
        static_cast<unsigned long long>(g_playback_clock.submitted_frames));
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_set_volume(AudioRequest *request)
{
    if (request == nullptr) {
        return;
    }
    g_task_last_request_id = request->request_id;
    const uint8_t previous = g_task_volume_percent;
    g_task_volume_percent = request->volume_percent > 100U ? 100U : request->volume_percent;

    esp_err_t ret = audio_task_apply_user_volume();
    if (ret != ESP_OK) {
        g_task_volume_percent = previous;
        if (g_pipeline_headphone_enabled) {
            // 左右声道寄存器是分两次 I2C 写入；失败时尽力恢复旧值，避免只更新单声道。
            (void)audio_task_apply_user_volume();
        }
        audio_task_set_state(g_task_state, ret);
        audio_request_complete(request, false, ret);
        return;
    }

    audio_task_publish_snapshot();
    ESP_LOGI(TAG, "用户音量已更新：%u%%，衰减steps=%u",
        static_cast<unsigned>(g_task_volume_percent),
        static_cast<unsigned>(audio_volume_percent_to_half_db_steps(g_task_volume_percent)));
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_set_mute(AudioRequest *request)
{
    if (request == nullptr) {
        return;
    }
    g_task_last_request_id = request->request_id;
    const bool previous = g_task_user_muted;
    g_task_user_muted = request->mute;

    esp_err_t ret = ESP_OK;
    if (g_pipeline_headphone_enabled) {
        if (g_task_user_muted) {
            ret = cs43131_set_pcm_mute(true);
        } else if (g_task_state == AudioPlaybackState::Playing && !g_pcm_unmute_pending) {
            ret = cs43131_set_pcm_mute(false);
        }
        // Paused/Preparing 状态解除“用户静音”时仍保持硬件 mute；真正恢复播放时由
        // resume/start 的 prime + unmute 序列统一解除，避免绕开 pop-free 时序。
    }

    if (ret != ESP_OK) {
        g_task_user_muted = previous;
        audio_task_set_state(g_task_state, ret);
        audio_request_complete(request, false, ret);
        return;
    }

    audio_task_publish_snapshot();
    ESP_LOGI(TAG, "用户静音：%s", g_task_user_muted ? "开启" : "关闭");
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_process_request(AudioRequest *request)
{
    switch (request->type) {
        case AudioCommandType::Play:
            audio_task_handle_play(request);
            break;
        case AudioCommandType::Stop:
            audio_task_handle_stop(request);
            break;
        case AudioCommandType::Pause:
            audio_task_handle_pause(request);
            break;
        case AudioCommandType::Resume:
            audio_task_handle_resume(request);
            break;
        case AudioCommandType::Seek:
            audio_task_handle_seek(request);
            break;
        case AudioCommandType::SetVolume:
            audio_task_handle_set_volume(request);
            break;
        case AudioCommandType::SetMute:
            audio_task_handle_set_mute(request);
            break;
    }
}

static AudioRequest *audio_transport_take_pending_for_wake()
{
    AudioRequest *pending = nullptr;
    portENTER_CRITICAL(&g_request_mux);
    pending = g_pending_transport_request;
    g_pending_transport_request = nullptr;
    g_transport_wake_pending = false;
    portEXIT_CRITICAL(&g_request_mux);
    return pending;
}

static void audio_task_process_queue_request(AudioRequest *queue_request)
{
    if (queue_request == nullptr) {
        return;
    }
    if (!audio_command_is_transport_intent(queue_request->type)) {
        audio_task_process_request(queue_request);
        return;
    }

    // transport 队列项只负责唤醒。真正执行的请求从 latest-intent 单槽中取出，
    // 因而连续 NEXT/Seek 不会在队列里逐条积压。返回的 pending 持有 slot 引用。
    AudioRequest *effective = audio_transport_take_pending_for_wake();
    if (effective == nullptr) {
#if APP_DIAG_TRANSPORT_INTENT
        ESP_LOGI(TAG,
            "INTENT_TRACE: WAKE_EMPTY wake_request=%lu type=%s",
            static_cast<unsigned long>(queue_request->request_id),
            audio_command_name(queue_request->type));
#endif
        return;
    }

#if APP_DIAG_TRANSPORT_INTENT
    if (effective != queue_request) {
        ESP_LOGI(TAG,
            "INTENT_TRACE: DISPATCH wake_request=%lu -> request=%lu intent=%lu type=%s track=%lu",
            static_cast<unsigned long>(queue_request->request_id),
            static_cast<unsigned long>(effective->request_id),
            static_cast<unsigned long>(effective->transport_intent_revision),
            audio_command_name(effective->type),
            static_cast<unsigned long>(effective->track_index));
    } else {
        ESP_LOGI(TAG,
            "INTENT_TRACE: DISPATCH request=%lu intent=%lu type=%s track=%lu",
            static_cast<unsigned long>(effective->request_id),
            static_cast<unsigned long>(effective->transport_intent_revision),
            audio_command_name(effective->type),
            static_cast<unsigned long>(effective->track_index));
    }
#endif

    audio_task_process_request(effective);
    audio_request_release(effective); // 释放 pending slot 引用
}

static void audio_task_main(void *arg)
{
    (void)arg;
    const BaseType_t current_core = xPortGetCoreID();
#if !APP_DIAG_AUDIO_CODEC
    // 乐鑫 Simple Decoder 内部 INFO 在每次 open 时都会重复打印 parser/stream 参数。
    // 正式固件只保留其 W/E；专项解码诊断打开时恢复 INFO，便于核对 parser 行为。
    esp_log_level_set("ESP_ES_PARSER", ESP_LOG_WARN);
    esp_log_level_set("AUD_Dec_Parse", ESP_LOG_WARN);
#endif
    ESP_LOGI(TAG, "AudioTask 已启动：核心=%d，优先级=%u，栈=%u字节",
        static_cast<int>(current_core),
        static_cast<unsigned>(uxTaskPriorityGet(nullptr)),
        static_cast<unsigned>(AUDIO_TASK_STACK_BYTES));

    // 高采样率播放依赖双核流水线：AudioTask 固定 Core 0，SD/FLAC 预取固定 Core 1。
    // 如果运行环境没有遵守绑核约束，宁可拒绝初始化，也不让音频实时任务和阻塞 I/O 混在同一核心。
    if (current_core != AUDIO_TASK_CORE) {
        g_start_result = ESP_ERR_INVALID_STATE;
        g_task_ready = false;
        g_task_state = AudioPlaybackState::Error;
        g_task_last_error = g_start_result;
        audio_task_publish_snapshot();
        ESP_LOGE(TAG, "AudioTask 核心绑定异常：当前=%d，期望=%d",
            static_cast<int>(current_core),
            static_cast<int>(AUDIO_TASK_CORE));
        if (g_start_done != nullptr) {
            xSemaphoreGive(g_start_done);
        }
        vTaskDelete(nullptr);
        return;
    }

    g_start_result = pcm_decoder_register_backends();
    if (g_start_result == ESP_OK) {
        g_start_result = cs43131_init();
    }
    if (g_start_result == ESP_OK) {
        g_task_ready = true;
        g_task_state = AudioPlaybackState::Ready;
        g_task_last_error = ESP_OK;
        audio_task_publish_snapshot();
        ESP_LOGI(TAG, "AudioTask 已取得运行期音频硬件所有权；统一 PCM Core 已就绪，DAC 当前保持安全掉电状态");
    } else {
        g_task_ready = false;
        g_task_state = AudioPlaybackState::Error;
        g_task_last_error = g_start_result;
        audio_task_publish_snapshot();
        ESP_LOGE(TAG, "AudioTask 初始化播放器后端失败：%s", esp_err_to_name(g_start_result));
    }

    if (g_start_done != nullptr) {
        xSemaphoreGive(g_start_done);
    }

    if (g_start_result != ESP_OK) {
        g_audio_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        AudioRequest *request = nullptr;
        const bool stream_needs_service =
            g_task_state == AudioPlaybackState::Playing ||
            g_task_state == AudioPlaybackState::Paused;
        const TickType_t wait_ticks = stream_needs_service ? 0 : portMAX_DELAY;

        if (xQueueReceive(g_command_queue, &request, wait_ticks) == pdTRUE && request != nullptr) {
            audio_task_process_queue_request(request);
            audio_request_release(request); // 释放 queue 引用
        }

        if (g_task_state == AudioPlaybackState::Playing) {
            audio_task_service_pcm_playback();
        } else if (g_task_state == AudioPlaybackState::Paused) {
            audio_task_service_pause_silence();
        }
    }
}

static bool audio_service_finish_submit_caller(AudioRequest *request, bool wait)
{
    if (!wait) {
        audio_request_release(request);
        return true;
    }

    if (request->done == nullptr) {
        ESP_LOGE(TAG, "同步请求缺少完成信号量：请求=%lu",
            static_cast<unsigned long>(request->request_id));
        audio_request_release(request);
        return false;
    }

    const bool completed = xSemaphoreTake(request->done, AUDIO_SYNC_WAIT_TIMEOUT) == pdTRUE;
    const bool success = completed && request->success;
    if (!completed) {
        ESP_LOGE(TAG, "等待音频命令完成超时：请求=%lu",
            static_cast<unsigned long>(request->request_id));
    }
    audio_request_release(request);
    return success;
}

static bool audio_service_submit_transport_intent(AudioRequest *request, bool wait)
{
    if (request == nullptr || g_command_queue == nullptr || g_transport_submit_mutex == nullptr ||
        !audio_service_is_ready() || !audio_command_is_transport_intent(request->type)) {
        audio_request_release(request);
        return false;
    }

    if (xSemaphoreTake(g_transport_submit_mutex, AUDIO_QUEUE_SEND_TIMEOUT) != pdTRUE) {
        ESP_LOGE(TAG, "等待 transport intent 提交锁超时：请求=%lu",
            static_cast<unsigned long>(request->request_id));
        audio_request_release(request);
        return false;
    }

    AudioRequest *replaced = nullptr;
    bool need_wake = false;
    uint32_t previous_latest_revision = 0;

    // slot 持有独立引用；即使调用方异步立即返回，请求仍能活到 AudioTask 取走。
    audio_request_retain(request);
    portENTER_CRITICAL(&g_request_mux);
    previous_latest_revision = g_latest_transport_intent_revision;
    request->transport_intent_revision = g_next_transport_intent_revision++;
    if (g_next_transport_intent_revision == 0U) {
        g_next_transport_intent_revision = 1U;
    }
    replaced = g_pending_transport_request;
    g_pending_transport_request = request;
    g_latest_transport_intent_revision = request->transport_intent_revision;
    need_wake = !g_transport_wake_pending;
    if (need_wake) {
        g_transport_wake_pending = true;
    }
    portEXIT_CRITICAL(&g_request_mux);

    if (replaced != nullptr && replaced != request) {
#if APP_DIAG_TRANSPORT_INTENT
        ESP_LOGI(TAG,
            "INTENT_TRACE: COALESCE old_request=%lu old_intent=%lu old_type=%s -> request=%lu intent=%lu type=%s",
            static_cast<unsigned long>(replaced->request_id),
            static_cast<unsigned long>(replaced->transport_intent_revision),
            audio_command_name(replaced->type),
            static_cast<unsigned long>(request->request_id),
            static_cast<unsigned long>(request->transport_intent_revision),
            audio_command_name(request->type));
#endif
        audio_request_complete(replaced, false, ESP_ERR_INVALID_STATE);
        audio_request_release(replaced); // 释放被覆盖请求的 slot 引用
    }

    if (need_wake) {
        // queue 仍持有一份引用。这个请求对象仅作为 transport wake token；真正执行目标
        // 会在 AudioTask 出队时从 pending slot 读取，因此后续替换不会继续占队列。
        audio_request_retain(request);
        if (xQueueSend(g_command_queue, &request, AUDIO_QUEUE_SEND_TIMEOUT) != pdTRUE) {
            ESP_LOGE(TAG, "transport intent 唤醒入队失败：请求=%lu intent=%lu",
                static_cast<unsigned long>(request->request_id),
                static_cast<unsigned long>(request->transport_intent_revision));

            portENTER_CRITICAL(&g_request_mux);
            if (g_pending_transport_request == request) {
                g_pending_transport_request = nullptr;
                g_transport_wake_pending = false;
                g_latest_transport_intent_revision = previous_latest_revision;
            }
            portEXIT_CRITICAL(&g_request_mux);

            audio_request_release(request); // queue 引用
            audio_request_release(request); // slot 引用
            xSemaphoreGive(g_transport_submit_mutex);
            audio_request_complete(request, false, ESP_ERR_TIMEOUT);
            return audio_service_finish_submit_caller(request, wait);
        }
    }

#if APP_DIAG_TRANSPORT_INTENT
    ESP_LOGI(TAG,
        "INTENT_TRACE: PUBLISH request=%lu intent=%lu type=%s track=%lu wake=%u",
        static_cast<unsigned long>(request->request_id),
        static_cast<unsigned long>(request->transport_intent_revision),
        audio_command_name(request->type),
        static_cast<unsigned long>(request->track_index),
        static_cast<unsigned>(need_wake));
#endif

    xSemaphoreGive(g_transport_submit_mutex);
    return audio_service_finish_submit_caller(request, wait);
}

static bool audio_service_submit(AudioRequest *request, bool wait)
{
    if (request == nullptr || g_command_queue == nullptr || !audio_service_is_ready()) {
        audio_request_release(request);
        return false;
    }

    // 队列持有独立引用。调用方无论同步还是异步，都可以安全释放自己的引用。
    audio_request_retain(request);
    if (xQueueSend(g_command_queue, &request, AUDIO_QUEUE_SEND_TIMEOUT) != pdTRUE) {
        ESP_LOGE(TAG, "音频命令队列已满：请求=%lu",
            static_cast<unsigned long>(request->request_id));
        audio_request_release(request);
        audio_request_release(request);
        return false;
    }

    return audio_service_finish_submit_caller(request, wait);
}

const char *audio_playback_state_name_cn(AudioPlaybackState state)
{
    switch (state) {
        case AudioPlaybackState::Starting: return "启动中";
        case AudioPlaybackState::Ready: return "就绪";
        case AudioPlaybackState::Preparing: return "准备中";
        case AudioPlaybackState::Prepared: return "已准备";
        case AudioPlaybackState::Playing: return "播放中";
        case AudioPlaybackState::Seeking: return "定位中";
        case AudioPlaybackState::Paused: return "已暂停";
        case AudioPlaybackState::Finished: return "播放结束";
        case AudioPlaybackState::Stopped: return "已停止";
        case AudioPlaybackState::Error: return "错误";
        default: return "未知";
    }
}

esp_err_t audio_service_start()
{
    if (g_audio_task != nullptr && audio_service_is_ready()) {
        return ESP_OK;
    }

    // P1.5.2R.3：FFT 是低优先级旁路观察者。即使创建失败也不能阻止核心音频服务启动。
    const esp_err_t spectrum_ret = audio_spectrum_snapshot_start();
    if (spectrum_ret != ESP_OK) {
        ESP_LOGW(TAG, "SpectrumFFT旁路启动失败，继续无频谱运行：%s", esp_err_to_name(spectrum_ret));
    }

    if (g_command_queue == nullptr) {
        g_command_queue = xQueueCreate(AUDIO_COMMAND_QUEUE_LENGTH, sizeof(AudioRequest *));
        if (g_command_queue == nullptr) {
            ESP_LOGE(TAG, "创建音频命令队列失败");
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_transport_submit_mutex == nullptr) {
        g_transport_submit_mutex = xSemaphoreCreateMutex();
        if (g_transport_submit_mutex == nullptr) {
            ESP_LOGE(TAG, "创建 transport intent 提交锁失败");
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_start_done != nullptr) {
        vSemaphoreDelete(g_start_done);
        g_start_done = nullptr;
    }
    g_start_done = xSemaphoreCreateBinary();
    if (g_start_done == nullptr) {
        ESP_LOGE(TAG, "创建 AudioTask 启动信号量失败");
        return ESP_ERR_NO_MEM;
    }

    g_start_result = ESP_ERR_INVALID_STATE;
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        audio_task_main,
        "AudioTask",
        AUDIO_TASK_STACK_BYTES,
        nullptr,
        AUDIO_TASK_PRIORITY,
        &g_audio_task,
        AUDIO_TASK_CORE
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "创建 AudioTask 失败");
        g_audio_task = nullptr;
        vSemaphoreDelete(g_start_done);
        g_start_done = nullptr;
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(g_start_done, AUDIO_START_WAIT_TIMEOUT) != pdTRUE) {
        ESP_LOGE(TAG, "等待 AudioTask 初始化超时");
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(g_start_done);
    g_start_done = nullptr;
    return g_start_result;
}

bool audio_service_is_ready()
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot)) {
        return false;
    }
    return snapshot.ready;
}

bool audio_service_get_snapshot(AudioStateSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }
    portENTER_CRITICAL(&g_snapshot_mux);
    *out_snapshot = g_snapshot;
    portEXIT_CRITICAL(&g_snapshot_mux);
    return true;
}

bool audio_service_get_last_fault(AudioFaultSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }
    portENTER_CRITICAL(&g_fault_snapshot_mux);
    *out_snapshot = g_fault_snapshot;
    portEXIT_CRITICAL(&g_fault_snapshot_mux);
    return out_snapshot->valid;
}

void audio_service_log_last_fault()
{
    AudioFaultSnapshot snapshot = {};
    if (!audio_service_get_last_fault(&snapshot)) {
        return;
    }
    ESP_LOGE(TAG,
        "R.36.2.2 AUDIO_FAULT_SNAPSHOT：count=%lu stage=%s err=%s track=%lu rev=%lu rate=%luHz pos=%lluf ring=%lu/%luB min=%luB qos=%u pressure=%u ioerr=%u eof=%u emergency=%lu recovered=%lu maxread=%lu RAM(internal/min/largest/dma/psram)=%lu/%lu/%lu/%lu/%lu",
        static_cast<unsigned long>(snapshot.fault_count),
        audio_fault_stage_name(snapshot.stage),
        esp_err_to_name(snapshot.error),
        static_cast<unsigned long>(snapshot.track_index),
        static_cast<unsigned long>(snapshot.playback_revision),
        static_cast<unsigned long>(snapshot.sample_rate_hz),
        static_cast<unsigned long long>(snapshot.position_frames),
        static_cast<unsigned long>(snapshot.flac_ring_buffered_bytes),
        static_cast<unsigned long>(snapshot.flac_ring_capacity_bytes),
        static_cast<unsigned long>(snapshot.flac_ring_min_buffered_bytes),
        static_cast<unsigned>(snapshot.flac_qos_level),
        static_cast<unsigned>(snapshot.flac_prefetch_pressure),
        static_cast<unsigned>(snapshot.flac_prefetch_io_error),
        static_cast<unsigned>(snapshot.flac_prefetch_eof),
        static_cast<unsigned long>(snapshot.flac_emergency_entries),
        static_cast<unsigned long>(snapshot.flac_recovered_count),
        static_cast<unsigned long>(snapshot.flac_max_consecutive_reads),
        static_cast<unsigned long>(snapshot.internal_free_bytes),
        static_cast<unsigned long>(snapshot.internal_min_bytes),
        static_cast<unsigned long>(snapshot.internal_largest_bytes),
        static_cast<unsigned long>(snapshot.dma_free_bytes),
        static_cast<unsigned long>(snapshot.psram_free_bytes));
}

bool audio_service_get_spectrum_snapshot(AudioSpectrumSnapshot *out_snapshot)
{
    return audio_spectrum_snapshot_get(out_snapshot);
}

void audio_service_set_spectrum_enabled(bool enabled)
{
    audio_spectrum_snapshot_set_enabled(enabled);
}

bool audio_service_play_track(
    uint32_t track_index,
    const char *path,
    MediaFormat format,
    const MediaTechnicalInfo *technical_info,
    bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::Play, wait);
    if (request == nullptr) {
        return false;
    }
    request->track_index = track_index;
    request->format = format;
    if (technical_info != nullptr) {
        // 跨任务只复制 POD 快照，不把 Catalog 内部指针交给 AudioTask。
        request->technical_info = *technical_info;
        request->has_technical_info = true;
    }
    if (!audio_request_set_path(request, path)) {
        audio_request_release(request);
        return false;
    }
    return audio_service_submit_transport_intent(request, wait);
}

bool audio_service_stop(bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::Stop, wait);
    return audio_service_submit(request, wait);
}

bool audio_service_pause(bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::Pause, wait);
    return audio_service_submit(request, wait);
}

bool audio_service_resume(bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::Resume, wait);
    return audio_service_submit(request, wait);
}


bool audio_service_seek_track(
    uint32_t track_index,
    const char *path,
    MediaFormat format,
    const MediaTechnicalInfo *technical_info,
    uint64_t target_ms,
    bool wait)
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready ||
        snapshot.track_index != track_index ||
        (snapshot.state != AudioPlaybackState::Playing &&
         snapshot.state != AudioPlaybackState::Paused &&
         snapshot.state != AudioPlaybackState::Seeking)) {
        return false;
    }

    AudioRequest *request = audio_request_create(AudioCommandType::Seek, wait);
    if (request == nullptr) {
        return false;
    }
    request->track_index = track_index;
    request->format = format;
    request->expected_playback_revision = snapshot.playback_revision;
    request->seek_target_ms = target_ms;
    if (technical_info != nullptr) {
        request->technical_info = *technical_info;
        request->has_technical_info = true;
    }
    if (!audio_request_set_path(request, path)) {
        audio_request_release(request);
        return false;
    }

    // Play/Seek 共用 latest-intent 单槽；快速拖动或快速切歌都只保留最后目标。
    return audio_service_submit_transport_intent(request, wait);
}

bool audio_service_set_volume(uint8_t percent, bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::SetVolume, wait);
    if (request == nullptr) {
        return false;
    }
    request->volume_percent = percent > 100U ? 100U : percent;
    return audio_service_submit(request, wait);
}

bool audio_service_set_mute(bool mute, bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::SetMute, wait);
    if (request == nullptr) {
        return false;
    }
    request->mute = mute;
    return audio_service_submit(request, wait);
}

uint32_t audio_service_playback_revision()
{
    AudioStateSnapshot snapshot = {};
    return audio_service_get_snapshot(&snapshot) ? snapshot.playback_revision : 0;
}
