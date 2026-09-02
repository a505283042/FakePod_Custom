#include "audio_service.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cs43131.h"
#include "i2s_output.h"
#include "pcm_decoder.h"
#include "audio_decode_workspace.h"
#include "audio_playback_clock.h"
#include "audio_spectrum_snapshot.h"
#include "nsf_synth.h"
#include "sources/avi_mp3_audio_source.h"
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

static constexpr uint32_t AUDIO_TASK_STACK_BYTES = 8192;
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
// R.39.6.4.4：FLAC 压缩 ring 瞬时耗空不等于真实 I/O 故障。只要 PrefetchTask 仍存活、
// 未报告 io_error 且尚未 EOF，就用短静音块维持 I2S 时钟，并给预取一个有硬上限的恢复窗口；
// 不再把第一个 20ms 等待超时直接升级成 AUDIO_FAULT。
static constexpr uint32_t AUDIO_FLAC_STARVE_GRACE_MAX_MS = 1000;
static constexpr uint32_t AUDIO_FLAC_STARVE_GRACE_MAX_ATTEMPTS = 32;
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
// SRAM.2 压力审计记录峰值栈使用 5588B；收敛到 8192B，仍保留 2604B 观测余量。


enum class AudioCommandType : uint8_t
{
    Play = 1,
    Stop,
    Pause,
    Resume,
    Seek,
    SetVolume,
    SetMute,
    VideoMp3Start,
    VideoMp3ReleaseStart,
    VideoMp3Stop,
    NsfStart,
    NsfPause,
    NsfResume,
    NsfSetTrack,
    NsfStop
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
    uint32_t video_sample_rate_hz = 0U;
    uint8_t video_channels = 0U;
    uint8_t video_bits_per_sample = 0U;
    bool video_start_barrier_armed = false;
    uint8_t *nsf_prg = nullptr;
    size_t nsf_prg_size = 0U;
    NsfSynthConfig nsf_config = {};
    bool nsf_restore_music_hardware = true;
    uint8_t nsf_track = 0U;
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
static StaticSemaphore_t g_start_done_storage = {};
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
static uint32_t g_flac_starve_grace_attempts = 0;
static int64_t g_flac_starve_grace_started_us = 0;

// 正式播放资源也只属于 AudioTask。
// WAV/FLAC/MP3 都通过统一 PcmDecoder 产出 32bit stereo PCM，I2S/DAC 不关心源格式。
static PcmDecoder g_decoder = {};
static AudioDecodeWorkspace g_decode_workspace = {};

// R.40.4.1：AVI MP3 是 AudioTask 内的临时第二 decoder。Music decoder/source 保持暂停原位，
// Video 结束后重新配置硬件并恢复原 Music pipeline，不重新打开文件、不丢播放位置。
static Mp3Decoder g_video_mp3_decoder = {};
static AudioSource g_video_mp3_source = {};
static AviMp3AudioSource g_video_mp3_source_storage = {};
static AudioPlaybackClock g_video_playback_clock = {};
static bool g_video_mp3_active = false;
static bool g_video_mp3_eof = false;
static bool g_video_mp3_start_released = true;
static bool g_video_restore_paused_music_hardware = false;
static uint32_t g_video_mp3_sample_rate_hz = 0U;
static uint16_t g_video_mp3_channels = 0U;
static uint16_t g_video_mp3_bits_per_sample = 0U;

// VM10：NSF 6502/2A03 由 AudioTask 独占输出硬件；
// 传统 NSF v1 没有 Subsong 时长：后台确认可靠循环后，在3分钟附近的完整循环边界结束；
// 无循环时后台预读确认自然静音起点并直接作为结束点；实时6秒静音仅保留为分析失败兜底。
static constexpr uint32_t NSF_SILENCE_END_MS = 6000U;
static constexpr uint32_t NSF_LOOP_TARGET_MS = 180000U;
static constexpr uint32_t NSF_TRACK_FADE_MS = 5000U;
// 普通 NSF 没有 Track 时长。后台在该虚拟时间内仍无法确认 Loop/自然静音时，
// 必须采用硬上限结束，不能让持续发声或完全静音的异常 Track 永久播放。
static constexpr uint32_t NSF_UNKNOWN_HARD_END_MS = 360000U;
static constexpr int32_t NSF_SILENCE_PEAK_PCM16 = 32;

enum class NsfEndPlanKind : uint8_t
{
    None = 0U,
    VerifiedLoop,
    NaturalSilence,
    HardLimit,
};

static NsfSynth g_nsf_synth = {};
static AudioPlaybackClock g_nsf_playback_clock = {};
static bool g_nsf_active = false;
static bool g_nsf_paused = false;
static bool g_nsf_eof = false;
static bool g_nsf_failed = false;
static bool g_nsf_seen_audible = false;
static uint64_t g_nsf_silence_frames = 0ULL;
static NsfEndPlanKind g_nsf_end_plan_kind = NsfEndPlanKind::None;
static uint64_t g_nsf_end_frame = 0ULL;
static bool g_nsf_restore_paused_music_hardware = false;

// NSF 时长分析和未来音符预读拆成两个独立低优先级任务，避免任一功能拖住另一条链路。
static constexpr uint32_t NSF_ANALYSIS_SAMPLE_RATE_HZ = 500U;
static constexpr uint32_t NSF_ANALYSIS_MAX_MS = NSF_UNKNOWN_HARD_END_MS;
static constexpr uint32_t NSF_ANALYSIS_SILENCE_VERIFY_MS = 10000U;
static constexpr size_t NSF_ANALYSIS_RENDER_FRAMES = 64U;
static constexpr uint32_t NSF_ANALYSIS_TASK_STACK_BYTES = 8192U;
// CPU0 的 AudioTask(priority=5) 持续出声，后台放在 CPU0 会长期拿不到足够时间片。
// 分析任务放回 CPU1、priority=1；每个小块后固定休眠2 tick，让 LVGL 和 IDLE1 都有明确运行窗口。
static constexpr UBaseType_t NSF_ANALYSIS_TASK_PRIORITY = 1U;
static constexpr BaseType_t NSF_ANALYSIS_TASK_CORE = 1;
static constexpr uint32_t NSF_ANALYSIS_YIELD_TICKS = 4U;
// 启动阶段先让未来音符预读建立约4秒窗口，避免 Preview/Analysis 两个6502实例同时抢CPU1。
// 预读失败时最多等待5秒，之后时长分析仍独立继续。
static constexpr uint32_t NSF_ANALYSIS_PREVIEW_WAIT_MS = 5000U;
static constexpr uint32_t NSF_ANALYSIS_PREVIEW_POLL_MS = 20U;

static constexpr uint32_t NSF_PREVIEW_SAMPLE_RATE_HZ = 500U;
static constexpr uint32_t NSF_PREVIEW_LEAD_MS = 5000U;
static constexpr uint32_t NSF_PREVIEW_PUBLISH_STEP_MS = 80U;
// 瀑布播放线下方保留的已演奏历史窗：已结束音符在快照中多停留该时长，供 UI 画历史轨迹。
static constexpr uint32_t NSF_VISUAL_HISTORY_MS = 2000U;
static constexpr size_t NSF_PREVIEW_RENDER_FRAMES = 128U;
static constexpr uint32_t NSF_PREVIEW_TASK_STACK_BYTES = 6144U;
static constexpr UBaseType_t NSF_PREVIEW_TASK_PRIORITY = 3U;
static constexpr BaseType_t NSF_PREVIEW_TASK_CORE = 1;
static constexpr uint32_t NSF_PREVIEW_RENDER_YIELD_TICKS = 1U;
static constexpr uint32_t NSF_PREVIEW_IDLE_DELAY_MS = 20U;

struct NsfAnalysisTaskArgs
{
    uint8_t *prg = nullptr;
    size_t prg_size = 0U;
    NsfSynthConfig config = {};
    uint32_t generation = 0U;
};

struct NsfPreviewTaskArgs
{
    uint8_t *prg = nullptr;
    size_t prg_size = 0U;
    NsfSynthConfig config = {};
    uint32_t generation = 0U;
};

struct NsfAnalysisResult
{
    bool complete = false;
    NsfEndPlanKind end_kind = NsfEndPlanKind::None;
    uint32_t generation = 0U;
    uint8_t track = 0U;
    uint64_t loop_start_ms = 0ULL;
    uint64_t loop_length_ms = 0ULL;
    uint64_t duration_ms = 0ULL;
    uint64_t duration_hint_ms = 0ULL;
};

static portMUX_TYPE g_nsf_analysis_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_nsf_analysis_generation = 1U;
static bool g_nsf_analysis_pending = false;
static NsfAnalysisResult g_nsf_analysis_result = {};

static portMUX_TYPE g_nsf_preview_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_nsf_preview_generation = 1U;

static constexpr size_t NSF_VISUAL_EVENT_CAPACITY = 512U;
static constexpr size_t NSF_LOOKAHEAD_MAX_EVENTS = 2048U;
static constexpr uint32_t NSF_VISUAL_PERCUSSION_MS = 90U;

struct NsfPlaybackVisualVoice
{
    bool active = false;
    uint8_t note = 0U;
    uint8_t level = 0U;
    uint32_t start_ms = 0U;
};

struct NsfPlaybackVisualState
{
    AudioNsfVisualEvent *events = nullptr;
    size_t head = 0U;
    size_t count = 0U;
    NsfPlaybackVisualVoice melodic[3] = {};
    bool percussion_active[2] = {};
    uint8_t track = 0U;
    bool available = false;
};

struct NsfLookaheadBuilder
{
    AudioNsfVisualEvent *events = nullptr;
    size_t count = 0U;
    size_t capacity = 0U;
    NsfPlaybackVisualVoice melodic[3] = {};
    bool percussion_active[2] = {};
    bool truncated = false;
};

struct NsfLookaheadInfo
{
    bool available = false;
    uint8_t track = 0U;
};

static SemaphoreHandle_t g_nsf_visual_mutex = nullptr;
static NsfPlaybackVisualState g_nsf_visual = {};
static SemaphoreHandle_t g_nsf_lookahead_mutex = nullptr;
static AudioNsfVisualEvent *g_nsf_lookahead_events = nullptr;
static size_t g_nsf_lookahead_event_count = 0U;
static NsfLookaheadInfo g_nsf_lookahead_info = {};

// R.40.4.2：AudioTask 单写、Video Presenter/Decode 多读的 PCM 主时钟发布槽。
// 不让 Video 直接读取 AudioTask 内部 decoder/clock，避免跨核撕裂 64-bit 计数。
static portMUX_TYPE g_video_clock_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static AudioVideoClockSnapshot g_video_clock_snapshot = {};
static uint32_t g_video_clock_revision = 0U;

static portMUX_TYPE g_nsf_clock_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static AudioNsfClockSnapshot g_nsf_clock_snapshot = {};
static uint32_t g_nsf_clock_revision = 0U;

static void audio_task_sync_nsf_analysis_end_plan();
static uint64_t audio_task_get_nsf_analysis_duration_hint(uint8_t track);

static void audio_task_publish_nsf_clock_snapshot()
{
    audio_task_sync_nsf_analysis_end_plan();
    AudioNsfClockSnapshot snapshot = {};
    snapshot.active = g_nsf_active;
    snapshot.paused = g_nsf_paused;
    snapshot.eof = g_nsf_eof;
    snapshot.failed = g_nsf_failed;
    snapshot.revision = g_nsf_clock_revision;
    snapshot.sample_rate_hz = g_nsf_playback_clock.sample_rate_hz;
    snapshot.submitted_frames = g_nsf_playback_clock.submitted_frames;
    snapshot.position_ms = audio_playback_clock_position_ms(&g_nsf_playback_clock);
    if (g_nsf_end_plan_kind != NsfEndPlanKind::None &&
        g_nsf_synth.sample_rate_hz != 0U) {
        snapshot.duration_ms =
            g_nsf_end_frame * 1000ULL / g_nsf_synth.sample_rate_hz;
        snapshot.duration_state = AudioNsfDurationState::Final;
    } else {
        snapshot.duration_ms = audio_task_get_nsf_analysis_duration_hint(
            nsf_synth_track(&g_nsf_synth));
        snapshot.duration_state = snapshot.duration_ms > 0ULL
            ? AudioNsfDurationState::Estimated
            : AudioNsfDurationState::Unknown;
    }
    snapshot.track = nsf_synth_track(&g_nsf_synth);
    snapshot.track_count = g_nsf_synth.track_count;

    portENTER_CRITICAL(&g_nsf_clock_snapshot_mux);
    g_nsf_clock_snapshot = snapshot;
    portEXIT_CRITICAL(&g_nsf_clock_snapshot_mux);
}

static void audio_task_publish_video_clock_snapshot()
{
    AudioVideoClockSnapshot snapshot = {};
    snapshot.active = g_video_mp3_active;
    snapshot.eof = g_video_mp3_eof;
    snapshot.start_released = g_video_mp3_start_released;
    snapshot.revision = g_video_clock_revision;
    snapshot.sample_rate_hz = g_video_playback_clock.sample_rate_hz;
    snapshot.submitted_frames = g_video_playback_clock.submitted_frames;
    snapshot.decoder_frames = g_video_playback_clock.decoder_frames;
    if (snapshot.sample_rate_hz != 0U) {
        snapshot.position_us =
            (snapshot.submitted_frames * 1000000ULL) / snapshot.sample_rate_hz;
    }

    portENTER_CRITICAL(&g_video_clock_snapshot_mux);
    g_video_clock_snapshot = snapshot;
    portEXIT_CRITICAL(&g_video_clock_snapshot_mux);
}

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
    if (request->nsf_prg != nullptr) {
        heap_caps_free(request->nsf_prg);
        request->nsf_prg = nullptr;
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

static void audio_task_reset_flac_starve_grace()
{
    g_flac_starve_grace_attempts = 0;
    g_flac_starve_grace_started_us = 0;
}

static void audio_task_log_flac_starve_recovered()
{
    if (g_flac_starve_grace_attempts == 0) {
        return;
    }

    FlacPrefetchRuntimeSnapshot flac = {};
    (void)flac_decoder_get_prefetch_runtime(&g_decoder.flac, &flac);
    const int64_t now_us = esp_timer_get_time();
    const uint32_t elapsed_ms = g_flac_starve_grace_started_us > 0 && now_us > g_flac_starve_grace_started_us
        ? static_cast<uint32_t>((now_us - g_flac_starve_grace_started_us) / 1000LL)
        : 0U;
    ESP_LOGI(TAG,
        "FLAC欠载已恢复：attempts=%u elapsed=%ums ring=%u/%uB",
        static_cast<unsigned>(g_flac_starve_grace_attempts),
        static_cast<unsigned>(elapsed_ms),
        static_cast<unsigned>(flac.buffered_bytes),
        static_cast<unsigned>(flac.capacity_bytes));
    audio_task_reset_flac_starve_grace();
}

static bool audio_task_try_recover_flac_starvation(esp_err_t decoder_error)
{
    if (decoder_error != ESP_ERR_TIMEOUT || g_task_format != MediaFormat::FLAC ||
        !flac_decoder_is_open(&g_decoder.flac)) {
        return false;
    }

    FlacPrefetchRuntimeSnapshot flac = {};
    if (!flac_decoder_get_prefetch_runtime(&g_decoder.flac, &flac) ||
        !flac.active || flac.io_error || flac.eof) {
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    if (g_flac_starve_grace_attempts == 0) {
        g_flac_starve_grace_started_us = now_us;
        ESP_LOGW(TAG,
            "FLAC欠载进入有界恢复：ring=%u/%uB ioerr=0 eof=0，最多%ums/%u次；期间以静音维持I2S",
            static_cast<unsigned>(flac.buffered_bytes),
            static_cast<unsigned>(flac.capacity_bytes),
            static_cast<unsigned>(AUDIO_FLAC_STARVE_GRACE_MAX_MS),
            static_cast<unsigned>(AUDIO_FLAC_STARVE_GRACE_MAX_ATTEMPTS));
    }

    const uint32_t elapsed_ms = g_flac_starve_grace_started_us > 0 && now_us > g_flac_starve_grace_started_us
        ? static_cast<uint32_t>((now_us - g_flac_starve_grace_started_us) / 1000LL)
        : 0U;
    if (elapsed_ms >= AUDIO_FLAC_STARVE_GRACE_MAX_MS ||
        g_flac_starve_grace_attempts >= AUDIO_FLAC_STARVE_GRACE_MAX_ATTEMPTS) {
        ESP_LOGE(TAG,
            "FLAC欠载恢复超限：attempts=%u elapsed=%ums ring=%u/%uB；升级为真实播放故障",
            static_cast<unsigned>(g_flac_starve_grace_attempts),
            static_cast<unsigned>(elapsed_ms),
            static_cast<unsigned>(flac.buffered_bytes),
            static_cast<unsigned>(flac.capacity_bytes));
        audio_task_reset_flac_starve_grace();
        return false;
    }

    ++g_flac_starve_grace_attempts;
    const esp_err_t silence_ret = i2s_output_stream_write_silence(
        AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (silence_ret != ESP_OK) {
        ESP_LOGE(TAG, "FLAC欠载恢复期间I2S静音写入失败：%s", esp_err_to_name(silence_ret));
        audio_task_reset_flac_starve_grace();
        return false;
    }

    // 静音只负责维持硬件时钟，不属于媒体 PCM，因此绝不能推进正式播放时钟。
    return true;
}

static void audio_task_reset_media_fields()
{
    g_task_sample_rate_hz = 0;
    g_task_channels = 0;
    g_task_bits_per_sample = 0;
    audio_playback_clock_reset(&g_playback_clock);
    g_task_total_frames = 0;
    g_last_progress_publish_frame = 0;
    audio_task_reset_flac_starve_grace();
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
        case AudioCommandType::VideoMp3Start: return "VIDEO_MP3_START";
        case AudioCommandType::VideoMp3ReleaseStart: return "VIDEO_MP3_RELEASE_START";
        case AudioCommandType::VideoMp3Stop: return "VIDEO_MP3_STOP";
        case AudioCommandType::NsfStart: return "NSF_START";
        case AudioCommandType::NsfPause: return "NSF_PAUSE";
        case AudioCommandType::NsfResume: return "NSF_RESUME";
        case AudioCommandType::NsfSetTrack: return "NSF_SET_TRACK";
        case AudioCommandType::NsfStop: return "NSF_STOP";
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

static void audio_task_begin_pcm_fade_in(const char *reason, uint32_t sample_rate_hz = 0U)
{
    const uint32_t sample_rate = sample_rate_hz > 0U
        ? sample_rate_hz
        : (g_task_sample_rate_hz > 0U ? g_task_sample_rate_hz : 48000U);
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

static esp_err_t audio_task_shutdown_output_hardware(uint32_t active_rate_hz, const char *owner)
{
    esp_err_t first_error = ESP_OK;
    const uint32_t settle_rate = active_rate_hz > 0U ? active_rate_hz : 48000U;

    AUDIO_POP_TRACE_LOG(
        "OUTPUT_SHUTDOWN_BEGIN owner=%s rate=%lu hp=%u asp=%u i2s=%u",
        owner != nullptr ? owner : "unknown",
        static_cast<unsigned long>(settle_rate),
        static_cast<unsigned>(g_pipeline_headphone_enabled),
        static_cast<unsigned>(g_pipeline_asp_enabled),
        static_cast<unsigned>(g_pipeline_i2s_started));

    if (g_pipeline_headphone_enabled) {
        const esp_err_t mute_ret = cs43131_set_pcm_mute(true);
        audio_task_remember_first_error(mute_ret, &first_error);

        if (mute_ret == ESP_OK) {
            const size_t settle_frames = static_cast<size_t>(
                (static_cast<uint64_t>(settle_rate) * AUDIO_PCM_MUTE_SETTLE_MS + 999ULL) / 1000ULL
            );
            if (g_pipeline_i2s_started && i2s_output_is_started()) {
#if APP_DIAG_AUDIO_POP
                ESP_LOGI(TAG, "PCM软静音收敛：owner=%s 保持%lums全零PCM，帧=%u",
                    owner != nullptr ? owner : "unknown",
                    static_cast<unsigned long>(AUDIO_PCM_MUTE_SETTLE_MS),
                    static_cast<unsigned>(settle_frames));
#endif
                audio_task_remember_first_error(
                    i2s_output_stream_write_silence(settle_frames, AUDIO_I2S_WRITE_TIMEOUT_MS),
                    &first_error);
            } else {
                vTaskDelay(pdMS_TO_TICKS(AUDIO_PCM_MUTE_SETTLE_MS));
            }
        }

        audio_task_remember_first_error(cs43131_power_down_headphone_playback(), &first_error);
        g_pipeline_headphone_enabled = false;
        g_pipeline_asp_enabled = false;
    }

    if (g_pipeline_clock_prepared || g_pipeline_asp_enabled) {
        audio_task_remember_first_error(cs43131_finish_pcm_playback(), &first_error);
        g_pipeline_clock_prepared = false;
        g_pipeline_asp_enabled = false;
    }

    if (g_pipeline_i2s_started || i2s_output_is_started()) {
        audio_task_remember_first_error(i2s_output_stop(), &first_error);
        g_pipeline_i2s_started = false;
    }

    g_pcm_unmute_pending = false;
    audio_task_reset_pcm_fade_in();
    AUDIO_POP_TRACE_LOG("OUTPUT_SHUTDOWN_END owner=%s ret=%s",
        owner != nullptr ? owner : "unknown", esp_err_to_name(first_error));
    return first_error;
}

static esp_err_t audio_task_shutdown_pipeline()
{
    audio_task_reset_flac_starve_grace();
    audio_task_log_ram("shutdown_begin");

    esp_err_t first_error = audio_task_shutdown_output_hardware(g_task_sample_rate_hz, "Music");

    if (pcm_decoder_is_open(&g_decoder)) {
        pcm_decoder_close(&g_decoder);
    }
    audio_decode_workspace_trim(
        &g_decode_workspace,
        AUDIO_DECODE_WORKSPACE_RETAIN_INPUT_BYTES,
        AUDIO_DECODE_WORKSPACE_RETAIN_PCM_BYTES);

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

static esp_err_t audio_task_start_output_hardware(
    uint32_t sample_rate_hz,
    uint16_t bits_per_sample,
    uint16_t channels,
    const char *owner,
    const AudioRequest *transport_request,
    bool arm_unmute,
    const char *fade_reason)
{
    if (sample_rate_hz == 0U || channels == 0U) return ESP_ERR_INVALID_ARG;
    const char *label = owner != nullptr ? owner : "PCM";

    AUDIO_POP_TRACE_LOG(
        "START_BEGIN format=%s rate=%lu bits=%u channels=%u",
        label,
        static_cast<unsigned long>(sample_rate_hz),
        static_cast<unsigned>(bits_per_sample),
        static_cast<unsigned>(channels));

    g_pipeline_clock_prepared = true;
    esp_err_t ret = cs43131_prepare_pcm_playback_32bit(sample_rate_hz);
    if (ret != ESP_OK) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ret;
    }
    if (transport_request != nullptr &&
        audio_task_transport_request_superseded(transport_request, "after_dac_prepare")) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ESP_ERR_INVALID_STATE;
    }

    ret = i2s_output_stream_start_32bit(sample_rate_hz);
    if (ret != ESP_OK) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ret;
    }
    g_pipeline_i2s_started = true;
    AUDIO_POP_TRACE_LOG(
        "I2S_START rate=%lu bclk=%lu",
        static_cast<unsigned long>(sample_rate_hz),
        static_cast<unsigned long>(sample_rate_hz * 64UL));
    if (transport_request != nullptr &&
        audio_task_transport_request_superseded(transport_request, "after_i2s_start")) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ESP_ERR_INVALID_STATE;
    }

    ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ret;
    }

    ret = cs43131_enable_asp_input();
    if (ret != ESP_OK) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ret;
    }
    g_pipeline_asp_enabled = true;

    for (int i = 0; i < 3; ++i) {
        ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            audio_task_shutdown_output_hardware(sample_rate_hz, label);
            return ret;
        }
    }

    uint8_t asp_status = 0;
    ret = cs43131_read_asp_status(&asp_status);
    if (ret != ESP_OK) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ret;
    }

    for (int i = 0; i < 2; ++i) {
        ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            audio_task_shutdown_output_hardware(sample_rate_hz, label);
            return ret;
        }
    }

    asp_status = 0;
    ret = cs43131_read_asp_status(&asp_status);
    if (ret != ESP_OK) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ret;
    }
#if APP_DIAG_AUDIO_POP
    ESP_LOGI(TAG, "%s起播前 ASP 稳定状态：0x%02X", label, asp_status);
#endif
    AUDIO_POP_TRACE_LOG("ASP_STABLE status=0x%02X", asp_status);
    if ((asp_status & 0xF8U) != 0) {
        ESP_LOGE(TAG, "ASP 时序异常，拒绝开启耳放：INT_STATUS2=0x%02X", asp_status);
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ESP_FAIL;
    }
    if (transport_request != nullptr &&
        audio_task_transport_request_superseded(transport_request, "before_headphone_enable")) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ESP_ERR_INVALID_STATE;
    }

    g_pipeline_headphone_enabled = true;
    ret = cs43131_prepare_headphone_playback_low_volume();
    if (ret != ESP_OK) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ret;
    }
    ret = audio_task_apply_user_volume();
    if (ret != ESP_OK) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ret;
    }
    AUDIO_POP_TRACE_LOG("HP_ENABLE volume=%u muted=%u",
        static_cast<unsigned>(g_task_volume_percent),
        static_cast<unsigned>(g_task_user_muted));

    for (int i = 0; i < 4; ++i) {
        ret = i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            audio_task_shutdown_output_hardware(sample_rate_hz, label);
            return ret;
        }
    }

    if (transport_request != nullptr &&
        audio_task_transport_request_superseded(transport_request, "before_pipeline_commit")) {
        audio_task_shutdown_output_hardware(sample_rate_hz, label);
        return ESP_ERR_INVALID_STATE;
    }

    if (arm_unmute) {
        audio_task_begin_pcm_fade_in(fade_reason != nullptr ? fade_reason : "start", sample_rate_hz);
        g_pcm_unmute_pending = true;
        AUDIO_POP_TRACE_LOG("PCM_UNMUTE_ARMED reason=%s",
            fade_reason != nullptr ? fade_reason : "start");
    } else {
        g_pcm_unmute_pending = false;
        audio_task_reset_pcm_fade_in();
    }
    return ESP_OK;
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
    PcmSeekResult seek_result = {};
    esp_err_t ret = apply_seek
        ? pcm_decoder_open_for_seek(
            &g_decoder,
            decoder_type,
            path,
            &g_decode_workspace,
            seek_target_ms,
            request != nullptr && request->has_technical_info ? &request->technical_info : nullptr,
            &seek_result)
        : pcm_decoder_open(&g_decoder, decoder_type, path, &g_decode_workspace);
    if (ret != ESP_OK) {
        audio_task_log_ram("decoder_open_failed");
        if (apply_seek) {
            ESP_LOGE(TAG, "SEEK_TRACE: codec定位失败 format=%s target=%llums ret=%s",
                pcm_decoder_type_name(decoder_type),
                static_cast<unsigned long long>(seek_target_ms),
                esp_err_to_name(ret));
        }
        return ret;
    }
    audio_task_verify_index_snapshot(request, decoder_type);

    if (apply_seek) {
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

    // 格式解析和可选 Seek 均已完成；从这里开始才进入连续播放阶段。
    // MP3/WAV 切换到 Core1 顺序预读，避免 AudioTask 在 I2S 运行期间直接等待 FATFS。
    ret = pcm_decoder_enable_runtime_read_ahead(&g_decoder, path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启用运行期音频预读失败：format=%s ret=%s",
            pcm_decoder_type_name(decoder_type), esp_err_to_name(ret));
        pcm_decoder_close(&g_decoder);
        return ret;
    }
    if (request != nullptr && audio_task_transport_request_superseded(request, "after_read_ahead")) {
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
        "WORKSPACE_TRACE: codec=%s input=%uB pcm=%uB total=%uB（PSRAM共享，压缩预读ring独立）",
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

    ret = audio_task_start_output_hardware(
        g_task_sample_rate_hz,
        g_task_bits_per_sample,
        g_task_channels,
        pcm_decoder_type_name(decoder_type),
        request,
        true,
        "start");
    if (ret != ESP_OK) {
        audio_task_shutdown_pipeline();
        return ret;
    }
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
        "AUDIO_FAULT：count=%lu stage=%s err=%s track=%lu rev=%lu rate=%luHz pos=%lluf ring=%lu/%luB min=%luB qos=%u pressure=%u ioerr=%u emergency=%lu/%lu RAM(internal/dma/psram)=%lu/%lu/%lu",
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
        if (audio_task_try_recover_flac_starvation(ret)) {
            return;
        }
        audio_task_reset_flac_starve_grace();
        audio_task_fail_stream(ret, "读取PCM");
        return;
    }
    audio_task_log_flac_starve_recovered();

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

static void audio_task_video_mp3_close_decoder()
{
    if (mp3_decoder_is_open(&g_video_mp3_decoder)) {
        mp3_decoder_close(&g_video_mp3_decoder);
    }
    if (audio_source_is_open(&g_video_mp3_source)) {
        (void)audio_source_close(&g_video_mp3_source);
    }
    g_video_mp3_source_storage = {};
}

static esp_err_t audio_task_restore_paused_music_hardware(const char *reason)
{
    if (!g_video_restore_paused_music_hardware) return ESP_OK;
    if (g_task_state != AudioPlaybackState::Paused || !pcm_decoder_is_open(&g_decoder) ||
        g_task_sample_rate_hz == 0U) {
        ESP_LOGE(TAG, "AVI MP3恢复Music硬件失败：Music pipeline不再处于Paused reason=%s",
            reason != nullptr ? reason : "unknown");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = audio_task_start_output_hardware(
        g_task_sample_rate_hz,
        g_task_bits_per_sample,
        g_task_channels,
        "Music-Restore",
        nullptr,
        false,
        nullptr);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "AVI MP3已恢复Paused Music硬件：%luHz/%ubit/%uch reason=%s；保持mute等待Video退出后resume",
            static_cast<unsigned long>(g_task_sample_rate_hz),
            static_cast<unsigned>(g_task_bits_per_sample),
            static_cast<unsigned>(g_task_channels),
            reason != nullptr ? reason : "unknown");
    }
    return ret;
}

static esp_err_t audio_task_stop_video_mp3_internal(bool restore_music_hardware, const char *reason)
{
    if (!g_video_mp3_active && !mp3_decoder_is_open(&g_video_mp3_decoder) &&
        !audio_source_is_open(&g_video_mp3_source)) {
        g_video_restore_paused_music_hardware = false;
        return ESP_OK;
    }

    esp_err_t first_error = ESP_OK;
    const uint32_t video_rate = g_video_mp3_sample_rate_hz > 0U
        ? g_video_mp3_sample_rate_hz : 44100U;
    audio_task_remember_first_error(
        audio_task_shutdown_output_hardware(video_rate, "AVI-MP3"), &first_error);

#if APP_DIAG_MP3_PERFORMANCE
    AviMp3BridgeSnapshot bridge_perf = {};
    if (avi_mp3_bridge_get_snapshot(&bridge_perf)) {
        const uint32_t read_wait_avg_us = bridge_perf.read_wait_count != 0U
            ? static_cast<uint32_t>(bridge_perf.read_wait_us_total / bridge_perf.read_wait_count)
            : 0U;
        ESP_LOGI(TAG,
            "AVI MP3 Bridge汇总：high=%u/%uB pushed=%lluB read=%lluB push_wait=%lu；read_wait avg=%luus max=%luus >1/5/10ms=%lu/%lu/%lu timeout=%lu calls=%lu",
            static_cast<unsigned>(bridge_perf.high_water_bytes),
            static_cast<unsigned>(bridge_perf.capacity_bytes),
            static_cast<unsigned long long>(bridge_perf.bytes_pushed),
            static_cast<unsigned long long>(bridge_perf.bytes_read),
            static_cast<unsigned long>(bridge_perf.push_wait_count),
            static_cast<unsigned long>(read_wait_avg_us),
            static_cast<unsigned long>(bridge_perf.read_wait_us_max),
            static_cast<unsigned long>(bridge_perf.read_wait_over_1ms),
            static_cast<unsigned long>(bridge_perf.read_wait_over_5ms),
            static_cast<unsigned long>(bridge_perf.read_wait_over_10ms),
            static_cast<unsigned long>(bridge_perf.read_wait_timeout_count),
            static_cast<unsigned long>(bridge_perf.read_wait_count));
    }
#endif

    audio_task_video_mp3_close_decoder();
    const bool should_restore = restore_music_hardware && g_video_restore_paused_music_hardware;
    g_video_mp3_active = false;
    g_video_mp3_eof = false;
    g_video_mp3_start_released = true;
    g_video_mp3_sample_rate_hz = 0U;
    g_video_mp3_channels = 0U;
    g_video_mp3_bits_per_sample = 0U;
    audio_playback_clock_reset(&g_video_playback_clock, 0U);
    audio_task_publish_video_clock_snapshot();

    if (should_restore) {
        audio_task_remember_first_error(
            audio_task_restore_paused_music_hardware(reason), &first_error);
    }
    g_video_restore_paused_music_hardware = false;

    ESP_LOGI(TAG, "视频音频已停止：恢复Music硬件=%u 结果=%s",
        static_cast<unsigned>(should_restore), esp_err_to_name(first_error));
    return first_error;
}

static void audio_task_handle_video_mp3_start(AudioRequest *request)
{
    if (request == nullptr) return;
    g_task_last_request_id = request->request_id;

    if (g_video_mp3_active || mp3_decoder_is_open(&g_video_mp3_decoder)) {
        ESP_LOGW(TAG, "AVI MP3启动被拒绝：已有Video Audio pipeline");
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }
    if (g_nsf_active || nsf_synth_is_open(&g_nsf_synth)) {
        ESP_LOGE(TAG, "AVI MP3启动被拒绝：电子音流仍占用AudioTask输出");
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }
    if (g_task_state == AudioPlaybackState::Playing ||
        g_task_state == AudioPlaybackState::Preparing ||
        g_task_state == AudioPlaybackState::Seeking) {
        ESP_LOGE(TAG, "AVI MP3启动要求Music先暂停：当前state=%s",
            audio_playback_state_name_cn(g_task_state));
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    g_video_restore_paused_music_hardware =
        g_task_state == AudioPlaybackState::Paused && pcm_decoder_is_open(&g_decoder);

    if (g_pipeline_headphone_enabled || g_pipeline_clock_prepared ||
        g_pipeline_i2s_started || i2s_output_is_started()) {
        const uint32_t active_rate = g_task_sample_rate_hz > 0U ? g_task_sample_rate_hz : 48000U;
        const esp_err_t shutdown_ret = audio_task_shutdown_output_hardware(active_rate, "Music-Paused");
        if (shutdown_ret != ESP_OK) {
            g_video_restore_paused_music_hardware = false;
            audio_request_complete(request, false, shutdown_ret);
            return;
        }
    }

    esp_err_t ret = avi_mp3_audio_source_open(&g_video_mp3_source, &g_video_mp3_source_storage);
    if (ret == ESP_OK) {
        ret = mp3_decoder_open(&g_video_mp3_decoder, &g_video_mp3_source, nullptr, true);
    }
    if (ret != ESP_OK) {
        audio_task_video_mp3_close_decoder();
        if (g_video_restore_paused_music_hardware) {
            (void)audio_task_restore_paused_music_hardware("video_open_failed");
        }
        g_video_restore_paused_music_hardware = false;
        ESP_LOGE(TAG, "AVI MP3 streaming decoder启动失败：%s", esp_err_to_name(ret));
        audio_request_complete(request, false, ret);
        return;
    }

    if ((request->video_sample_rate_hz != 0U &&
         request->video_sample_rate_hz != g_video_mp3_decoder.sample_rate_hz) ||
        (request->video_channels != 0U && request->video_channels != g_video_mp3_decoder.channels) ||
        (request->video_bits_per_sample != 0U &&
         request->video_bits_per_sample != g_video_mp3_decoder.bits_per_sample)) {
        ESP_LOGE(TAG,
            "AVI MP3格式与Extractor不一致：expected=%luHz/%ubit/%uch decoder=%luHz/%ubit/%uch",
            static_cast<unsigned long>(request->video_sample_rate_hz),
            static_cast<unsigned>(request->video_bits_per_sample),
            static_cast<unsigned>(request->video_channels),
            static_cast<unsigned long>(g_video_mp3_decoder.sample_rate_hz),
            static_cast<unsigned>(g_video_mp3_decoder.bits_per_sample),
            static_cast<unsigned>(g_video_mp3_decoder.channels));
        audio_task_video_mp3_close_decoder();
        if (g_video_restore_paused_music_hardware) {
            (void)audio_task_restore_paused_music_hardware("video_format_mismatch");
        }
        g_video_restore_paused_music_hardware = false;
        audio_request_complete(request, false, ESP_ERR_INVALID_RESPONSE);
        return;
    }

    g_video_mp3_sample_rate_hz = g_video_mp3_decoder.sample_rate_hz;
    g_video_mp3_channels = g_video_mp3_decoder.channels;
    g_video_mp3_bits_per_sample = g_video_mp3_decoder.bits_per_sample;
    audio_playback_clock_reset(&g_video_playback_clock, g_video_mp3_sample_rate_hz);
    g_video_mp3_eof = false;

    ret = audio_task_start_output_hardware(
        g_video_mp3_sample_rate_hz,
        g_video_mp3_bits_per_sample,
        g_video_mp3_channels,
        "AVI-MP3",
        nullptr,
        true,
        "video");
    if (ret != ESP_OK) {
        audio_task_video_mp3_close_decoder();
        if (g_video_restore_paused_music_hardware) {
            (void)audio_task_restore_paused_music_hardware("video_hw_failed");
        }
        g_video_restore_paused_music_hardware = false;
        audio_request_complete(request, false, ret);
        return;
    }

    ++g_video_clock_revision;
    if (g_video_clock_revision == 0U) ++g_video_clock_revision;
    g_video_mp3_active = true;
    g_video_mp3_start_released = !request->video_start_barrier_armed;
    audio_task_publish_video_clock_snapshot();
    ESP_LOGI(TAG, "视频音频已%s：%luHz/%uch",
        g_video_mp3_start_released ? "启动" : "预备",
        static_cast<unsigned long>(g_video_mp3_sample_rate_hz),
        static_cast<unsigned>(g_video_mp3_channels));
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_video_mp3_release_start(AudioRequest *request)
{
    if (request == nullptr) return;
    g_task_last_request_id = request->request_id;

    if (!g_video_mp3_active || !mp3_decoder_is_open(&g_video_mp3_decoder)) {
        ESP_LOGE(TAG, "AVI MP3 A/V Start Barrier解除失败：Video Audio pipeline未预备");
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    if (!g_video_mp3_start_released) {
        g_video_mp3_start_released = true;
        audio_task_publish_video_clock_snapshot();
        ESP_LOGI(TAG, "视频音频同步闸门已解除");
    }
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_video_mp3_stop(AudioRequest *request)
{
    if (request == nullptr) return;
    g_task_last_request_id = request->request_id;
    const esp_err_t ret = audio_task_stop_video_mp3_internal(true, "video_stop_command");
    if (ret != ESP_OK) {
        audio_task_set_state(AudioPlaybackState::Error, ret);
        audio_request_complete(request, false, ret);
        return;
    }
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_service_video_mp3()
{
    if (!g_video_mp3_active || !mp3_decoder_is_open(&g_video_mp3_decoder)) return;

    if (!g_video_mp3_start_released) {
        // Start Barrier期间只维持稳定的静音I2S时钟，不读取Bridge、不推进decoder/submitted clock。
        // 这样Video可以先准备首张RGB和BoundedSPI窗口，同时避免AudioTask零等待自旋。
        const esp_err_t silence_ret =
            i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (silence_ret != ESP_OK) {
            ESP_LOGE(TAG, "AVI MP3 Start Barrier静音维持失败：%s", esp_err_to_name(silence_ret));
        }
        return;
    }

    if (g_video_mp3_eof) {
        const esp_err_t silence_ret =
            i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (silence_ret != ESP_OK) {
            ESP_LOGE(TAG, "AVI MP3 EOF静音维持失败：%s", esp_err_to_name(silence_ret));
        }
        return;
    }

    size_t frames = 0U;
    esp_err_t ret = mp3_decoder_read_pcm32(
        &g_video_mp3_decoder,
        g_pcm_block,
        AUDIO_STREAM_FRAMES,
        &frames);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVI MP3读取PCM失败：%s；本段Video后续保持静音", esp_err_to_name(ret));
        (void)cs43131_set_pcm_mute(true);
        g_video_mp3_eof = true;
        audio_task_publish_video_clock_snapshot();
        return;
    }

    audio_playback_clock_note_decoder(&g_video_playback_clock, g_video_mp3_decoder.frames_read);
    if (frames == 0U) {
        if (mp3_decoder_is_eof(&g_video_mp3_decoder)) {
            g_video_mp3_eof = true;
            audio_task_publish_video_clock_snapshot();
            ESP_LOGI(TAG, "AVI MP3压缩流EOS：PCM=%lluf/%llums；等待Video结束后恢复Music",
                static_cast<unsigned long long>(g_video_playback_clock.submitted_frames),
                static_cast<unsigned long long>(audio_playback_clock_position_ms(&g_video_playback_clock)));
        }
        return;
    }

    audio_task_apply_pcm_fade_in(g_pcm_block, frames);
    ret = audio_task_unmute_when_pcm_ready();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVI MP3首PCM解除静音失败：%s", esp_err_to_name(ret));
        g_video_mp3_eof = true;
        audio_task_publish_video_clock_snapshot();
        return;
    }
    ret = i2s_output_stream_write_pcm32(g_pcm_block, frames, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVI MP3 I2S发送失败：%s", esp_err_to_name(ret));
        g_video_mp3_eof = true;
        audio_task_publish_video_clock_snapshot();
        return;
    }
    audio_playback_clock_commit_pcm(&g_video_playback_clock, frames);
    audio_task_publish_video_clock_snapshot();
}

static esp_err_t audio_task_restore_paused_music_hardware_for_nsf(const char *reason)
{
    if (!g_nsf_restore_paused_music_hardware) return ESP_OK;
    if (g_task_state != AudioPlaybackState::Paused || !pcm_decoder_is_open(&g_decoder) ||
        g_task_sample_rate_hz == 0U) {
        ESP_LOGE(TAG, "NSF恢复Music硬件失败：Music pipeline不再处于Paused reason=%s",
            reason != nullptr ? reason : "unknown");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = audio_task_start_output_hardware(
        g_task_sample_rate_hz,
        g_task_bits_per_sample,
        g_task_channels,
        "Music-Restore",
        nullptr,
        false,
        nullptr);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NSF已恢复Paused Music硬件：%luHz/%ubit/%uch reason=%s；等待电子音流决定是否resume",
            static_cast<unsigned long>(g_task_sample_rate_hz),
            static_cast<unsigned>(g_task_bits_per_sample),
            static_cast<unsigned>(g_task_channels),
            reason != nullptr ? reason : "unknown");
    }
    return ret;
}

static uint64_t nsf_loop_boundary_at_or_after(
    uint64_t loop_start,
    uint64_t loop_length,
    uint64_t minimum)
{
    if (loop_length == 0ULL) return 0ULL;
    if (minimum <= loop_start) return loop_start;
    const uint64_t delta = minimum - loop_start;
    const uint64_t loops = (delta + loop_length - 1ULL) / loop_length;
    return loop_start + loops * loop_length;
}

static uint64_t nsf_loop_boundary_nearest(
    uint64_t loop_start,
    uint64_t loop_length,
    uint64_t target)
{
    if (loop_length == 0ULL) return 0ULL;
    if (target <= loop_start) return loop_start;

    const uint64_t delta = target - loop_start;
    const uint64_t loops_before = delta / loop_length;
    const uint64_t before = loop_start + loops_before * loop_length;
    if (before == target) return before;

    const uint64_t after = before + loop_length;
    return target - before <= after - target ? before : after;
}

static bool nsf_analysis_generation_current(uint32_t generation)
{
    bool current = false;
    portENTER_CRITICAL(&g_nsf_analysis_mux);
    current = generation == g_nsf_analysis_generation;
    portEXIT_CRITICAL(&g_nsf_analysis_mux);
    return current;
}

static bool nsf_preview_generation_current(uint32_t generation)
{
    bool current = false;
    portENTER_CRITICAL(&g_nsf_preview_mux);
    current = generation == g_nsf_preview_generation;
    portEXIT_CRITICAL(&g_nsf_preview_mux);
    return current;
}

static bool nsf_lookahead_ready_for_track(uint8_t track)
{
    if (g_nsf_lookahead_mutex == nullptr ||
        xSemaphoreTake(g_nsf_lookahead_mutex, 0) != pdTRUE) {
        return false;
    }
    const bool ready = g_nsf_lookahead_info.available &&
        g_nsf_lookahead_info.track == track &&
        g_nsf_lookahead_events != nullptr &&
        g_nsf_lookahead_event_count > 0U;
    xSemaphoreGive(g_nsf_lookahead_mutex);
    return ready;
}

static void nsf_analysis_publish(
    uint32_t generation,
    uint8_t track,
    NsfEndPlanKind end_kind,
    uint64_t loop_start_ms,
    uint64_t loop_length_ms,
    uint64_t duration_ms)
{
    portENTER_CRITICAL(&g_nsf_analysis_mux);
    if (generation == g_nsf_analysis_generation) {
        g_nsf_analysis_pending = false;
        g_nsf_analysis_result.complete = true;
        g_nsf_analysis_result.end_kind = end_kind;
        g_nsf_analysis_result.generation = generation;
        g_nsf_analysis_result.track = track;
        g_nsf_analysis_result.loop_start_ms = loop_start_ms;
        g_nsf_analysis_result.loop_length_ms = loop_length_ms;
        g_nsf_analysis_result.duration_ms = duration_ms;
        g_nsf_analysis_result.duration_hint_ms = duration_ms;
    }
    portEXIT_CRITICAL(&g_nsf_analysis_mux);
}

static void nsf_analysis_publish_hint(
    uint32_t generation,
    uint8_t track,
    uint64_t duration_hint_ms)
{
    if (duration_hint_ms == 0ULL) return;
    portENTER_CRITICAL(&g_nsf_analysis_mux);
    if (generation == g_nsf_analysis_generation && !g_nsf_analysis_result.complete) {
        g_nsf_analysis_result.generation = generation;
        g_nsf_analysis_result.track = track;
        g_nsf_analysis_result.duration_hint_ms = duration_hint_ms;
    }
    portEXIT_CRITICAL(&g_nsf_analysis_mux);
}

static void nsf_analysis_mark_unavailable(uint32_t generation)
{
    portENTER_CRITICAL(&g_nsf_analysis_mux);
    if (generation == g_nsf_analysis_generation) {
        g_nsf_analysis_pending = false;
        g_nsf_analysis_result = {};
    }
    portEXIT_CRITICAL(&g_nsf_analysis_mux);
}

static void nsf_visual_append_locked(
    uint32_t start_ms,
    uint32_t end_ms,
    uint8_t note,
    uint8_t level,
    AudioNsfVisualVoice voice)
{
    if (g_nsf_visual.events == nullptr || end_ms <= start_ms) return;
    if (g_nsf_visual.count == NSF_VISUAL_EVENT_CAPACITY) {
        g_nsf_visual.head = (g_nsf_visual.head + 1U) % NSF_VISUAL_EVENT_CAPACITY;
        --g_nsf_visual.count;
    }
    const size_t index =
        (g_nsf_visual.head + g_nsf_visual.count) % NSF_VISUAL_EVENT_CAPACITY;
    AudioNsfVisualEvent &event = g_nsf_visual.events[index];
    event.start_ms = start_ms;
    event.end_ms = end_ms;
    event.note = note;
    event.level = level;
    event.voice = voice;
    event.reserved = 0U;
    ++g_nsf_visual.count;
}

static void nsf_visual_close_melodic_locked(uint8_t voice, uint32_t end_ms)
{
    if (voice >= 3U) return;
    NsfPlaybackVisualVoice &state = g_nsf_visual.melodic[voice];
    if (!state.active) return;
    nsf_visual_append_locked(
        state.start_ms,
        end_ms,
        state.note,
        state.level,
        static_cast<AudioNsfVisualVoice>(voice));
    state = {};
}

static void nsf_visual_push_tick_locked(
    const NsfSynthVisualTick &tick,
    uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0U) return;
    const uint32_t tick_ms = static_cast<uint32_t>(
        static_cast<uint64_t>(tick.frame) * 1000ULL / sample_rate_hz);

    for (uint8_t voice = 0U; voice < 3U; ++voice) {
        const bool active = (tick.active_mask & (1U << voice)) != 0U;
        const bool trigger = (tick.trigger_mask & (1U << voice)) != 0U;
        NsfPlaybackVisualVoice &state = g_nsf_visual.melodic[voice];
        if (!active) {
            nsf_visual_close_melodic_locked(voice, tick_ms);
            continue;
        }
        if (!state.active || state.note != tick.pitch[voice] || trigger) {
            nsf_visual_close_melodic_locked(voice, tick_ms);
            state.active = true;
            state.note = tick.pitch[voice];
            state.level = tick.level[voice];
            state.start_ms = tick_ms;
        } else if (tick.level[voice] > state.level) {
            state.level = tick.level[voice];
        }
    }

    for (uint8_t index = 0U; index < 2U; ++index) {
        const uint8_t voice = static_cast<uint8_t>(index + 3U);
        const bool active = (tick.active_mask & (1U << voice)) != 0U;
        const bool trigger = (tick.trigger_mask & (1U << voice)) != 0U;
        if (active && (trigger || !g_nsf_visual.percussion_active[index])) {
            nsf_visual_append_locked(
                tick_ms,
                tick_ms + NSF_VISUAL_PERCUSSION_MS,
                0U,
                tick.level[voice],
                static_cast<AudioNsfVisualVoice>(voice));
        }
        g_nsf_visual.percussion_active[index] = active;
    }
}

static void nsf_visual_reset_playback(uint8_t track)
{
    if (g_nsf_visual_mutex == nullptr) return;

    AudioNsfVisualEvent *new_events = nullptr;
    if (xSemaphoreTake(g_nsf_visual_mutex, portMAX_DELAY) != pdTRUE) return;
    if (g_nsf_visual.events == nullptr) {
        xSemaphoreGive(g_nsf_visual_mutex);
        new_events = static_cast<AudioNsfVisualEvent *>(heap_caps_calloc(
            NSF_VISUAL_EVENT_CAPACITY,
            sizeof(AudioNsfVisualEvent),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (new_events == nullptr) {
            ESP_LOGW(TAG, "NSF播放瀑布事件PSRAM申请失败：不影响音频播放");
            return;
        }
        if (xSemaphoreTake(g_nsf_visual_mutex, portMAX_DELAY) != pdTRUE) {
            heap_caps_free(new_events);
            return;
        }
        if (g_nsf_visual.events == nullptr) {
            g_nsf_visual.events = new_events;
            new_events = nullptr;
        }
    }

    AudioNsfVisualEvent *events = g_nsf_visual.events;
    g_nsf_visual = {};
    g_nsf_visual.events = events;
    g_nsf_visual.track = track;
    g_nsf_visual.available = events != nullptr;
    xSemaphoreGive(g_nsf_visual_mutex);
    if (new_events != nullptr) heap_caps_free(new_events);

    ESP_LOGI(TAG, "NSF播放瀑布已绑定真实播放路：track=%u", static_cast<unsigned>(track + 1U));
}

static void nsf_visual_release_playback()
{
    if (g_nsf_visual_mutex == nullptr) return;
    if (xSemaphoreTake(g_nsf_visual_mutex, portMAX_DELAY) != pdTRUE) return;
    AudioNsfVisualEvent *events = g_nsf_visual.events;
    g_nsf_visual = {};
    xSemaphoreGive(g_nsf_visual_mutex);
    if (events != nullptr) heap_caps_free(events);
}

static void audio_task_update_nsf_visual_from_playback()
{
    if (!g_nsf_active || !nsf_synth_is_open(&g_nsf_synth) ||
        g_nsf_visual_mutex == nullptr || g_nsf_synth.sample_rate_hz == 0U) {
        return;
    }
    // AudioTask 不等待 UI；若 UI 正在复制事件，本轮先保留 Synth 内的小队列，下一块再取。
    if (xSemaphoreTake(g_nsf_visual_mutex, 0) != pdTRUE) return;

    NsfSynthVisualTick ticks[32] = {};
    const size_t count = nsf_synth_take_visual_ticks(
        &g_nsf_synth, ticks, sizeof(ticks) / sizeof(ticks[0]));
    for (size_t i = 0U; i < count; ++i) {
        nsf_visual_push_tick_locked(ticks[i], g_nsf_synth.sample_rate_hz);
    }
    xSemaphoreGive(g_nsf_visual_mutex);
}

static void nsf_lookahead_builder_append(
    NsfLookaheadBuilder *builder,
    uint32_t start_ms,
    uint32_t end_ms,
    uint8_t note,
    uint8_t level,
    AudioNsfVisualVoice voice)
{
    if (builder == nullptr || builder->events == nullptr || end_ms <= start_ms) return;
    if (builder->count >= builder->capacity) {
        builder->truncated = true;
        return;
    }
    AudioNsfVisualEvent &event = builder->events[builder->count++];
    event.start_ms = start_ms;
    event.end_ms = end_ms;
    event.note = note;
    event.level = level;
    event.voice = voice;
    event.reserved = 0U;
}

static void nsf_lookahead_builder_close_melodic(
    NsfLookaheadBuilder *builder,
    uint8_t voice,
    uint32_t end_ms)
{
    if (builder == nullptr || voice >= 3U) return;
    NsfPlaybackVisualVoice &state = builder->melodic[voice];
    if (!state.active) return;
    nsf_lookahead_builder_append(
        builder,
        state.start_ms,
        end_ms,
        state.note,
        state.level,
        static_cast<AudioNsfVisualVoice>(voice));
    state = {};
}

static void nsf_lookahead_builder_push_tick(
    NsfLookaheadBuilder *builder,
    const NsfSynthVisualTick &tick)
{
    if (builder == nullptr || builder->events == nullptr) return;
    const uint32_t tick_ms = static_cast<uint32_t>(
        static_cast<uint64_t>(tick.frame) * 1000ULL / NSF_PREVIEW_SAMPLE_RATE_HZ);

    for (uint8_t voice = 0U; voice < 3U; ++voice) {
        const bool active = (tick.active_mask & (1U << voice)) != 0U;
        const bool trigger = (tick.trigger_mask & (1U << voice)) != 0U;
        NsfPlaybackVisualVoice &state = builder->melodic[voice];
        if (!active) {
            nsf_lookahead_builder_close_melodic(builder, voice, tick_ms);
            continue;
        }
        if (!state.active || state.note != tick.pitch[voice] || trigger) {
            nsf_lookahead_builder_close_melodic(builder, voice, tick_ms);
            state.active = true;
            state.note = tick.pitch[voice];
            state.level = tick.level[voice];
            state.start_ms = tick_ms;
        } else if (tick.level[voice] > state.level) {
            state.level = tick.level[voice];
        }
    }

    for (uint8_t index = 0U; index < 2U; ++index) {
        const uint8_t voice = static_cast<uint8_t>(index + 3U);
        const bool active = (tick.active_mask & (1U << voice)) != 0U;
        const bool trigger = (tick.trigger_mask & (1U << voice)) != 0U;
        if (active && (trigger || !builder->percussion_active[index])) {
            nsf_lookahead_builder_append(
                builder,
                tick_ms,
                tick_ms + NSF_VISUAL_PERCUSSION_MS,
                0U,
                tick.level[voice],
                static_cast<AudioNsfVisualVoice>(voice));
        }
        builder->percussion_active[index] = active;
    }
}


static void nsf_lookahead_builder_prune_before(
    NsfLookaheadBuilder *builder,
    uint32_t cutoff_ms)
{
    if (builder == nullptr || builder->events == nullptr || builder->count == 0U) return;
    size_t write = 0U;
    for (size_t read = 0U; read < builder->count; ++read) {
        if (builder->events[read].end_ms <= cutoff_ms) continue;
        if (write != read) builder->events[write] = builder->events[read];
        ++write;
    }
    builder->count = write;
    builder->truncated = false;
    for (uint8_t voice = 0U; voice < 3U; ++voice) {
        if (builder->melodic[voice].active && builder->melodic[voice].start_ms < cutoff_ms) {
            builder->melodic[voice].start_ms = cutoff_ms;
        }
    }
}

static void nsf_lookahead_clear_published()
{
    AudioNsfVisualEvent *old_events = nullptr;
    if (g_nsf_lookahead_mutex == nullptr ||
        xSemaphoreTake(g_nsf_lookahead_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    old_events = g_nsf_lookahead_events;
    g_nsf_lookahead_events = nullptr;
    g_nsf_lookahead_event_count = 0U;
    g_nsf_lookahead_info = {};
    xSemaphoreGive(g_nsf_lookahead_mutex);
    if (old_events != nullptr) heap_caps_free(old_events);
}

static size_t nsf_lookahead_publish_snapshot(
    uint32_t generation,
    uint8_t track,
    const NsfLookaheadBuilder *builder,
    uint32_t preview_end_ms)
{
    if (builder == nullptr || builder->events == nullptr) return 0U;

    size_t active_count = 0U;
    for (uint8_t voice = 0U; voice < 3U; ++voice) {
        if (builder->melodic[voice].active &&
            builder->melodic[voice].start_ms < preview_end_ms) {
            ++active_count;
        }
    }
    const size_t total_count = builder->count + active_count;
    if (total_count == 0U) return 0U;

    AudioNsfVisualEvent *snapshot = static_cast<AudioNsfVisualEvent *>(heap_caps_malloc(
        total_count * sizeof(AudioNsfVisualEvent),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (snapshot == nullptr) return 0U;

    // 事件时间戳为预览时钟（超前真实播放约 LEAD）。UI 按真实 position 显示，
    // 音符会在未来窗口中自上而下移动；预览已渲染完的音符其真实结束时刻必然 ≤ 当前
    // 真实时间，因此这里不做时钟换算（换算会把快照清空）。播放线附近的历史由
    // 实时播放路（audio_service_nsf_copy_visual_events）在 UI 侧兜底补充。
    size_t out_count = 0U;
    for (size_t i = 0U; i < builder->count; ++i) snapshot[out_count++] = builder->events[i];
    for (uint8_t voice = 0U; voice < 3U; ++voice) {
        const NsfPlaybackVisualVoice &state = builder->melodic[voice];
        if (!state.active || state.start_ms >= preview_end_ms) continue;
        AudioNsfVisualEvent &event = snapshot[out_count++];
        event.start_ms = state.start_ms;
        event.end_ms = preview_end_ms;
        event.note = state.note;
        event.level = state.level;
        event.voice = static_cast<AudioNsfVisualVoice>(voice);
        event.reserved = 0U;
    }

    if (g_nsf_lookahead_mutex == nullptr ||
        xSemaphoreTake(g_nsf_lookahead_mutex, portMAX_DELAY) != pdTRUE) {
        heap_caps_free(snapshot);
        return 0U;
    }

    bool current = false;
    portENTER_CRITICAL(&g_nsf_preview_mux);
    current = generation == g_nsf_preview_generation;
    portEXIT_CRITICAL(&g_nsf_preview_mux);
    if (!current) {
        xSemaphoreGive(g_nsf_lookahead_mutex);
        heap_caps_free(snapshot);
        return 0U;
    }

    AudioNsfVisualEvent *old_events = g_nsf_lookahead_events;
    g_nsf_lookahead_events = snapshot;
    g_nsf_lookahead_event_count = out_count;
    g_nsf_lookahead_info.available = true;
    g_nsf_lookahead_info.track = track;
    xSemaphoreGive(g_nsf_lookahead_mutex);
    if (old_events != nullptr) heap_caps_free(old_events);
    return out_count;
}

static void nsf_analysis_task(void *arg)
{
    NsfAnalysisTaskArgs *args = static_cast<NsfAnalysisTaskArgs *>(arg);
    if (args == nullptr || args->prg == nullptr || args->prg_size == 0U) {
        if (args != nullptr) heap_caps_free(args);
        vTaskDelete(nullptr);
        return;
    }

    const uint32_t generation = args->generation;
    const uint8_t track = args->config.track;
    uint8_t *owned_prg = args->prg;
    const size_t prg_size = args->prg_size;
    NsfSynthConfig config = args->config;
    heap_caps_free(args);

    NsfSynth synth = {};
    esp_err_t ret = nsf_synth_open_owned(
        &synth, owned_prg, prg_size, &config, NSF_ANALYSIS_SAMPLE_RATE_HZ);
    if (ret != ESP_OK) {
        heap_caps_free(owned_prg); // open_owned 失败时所有权仍属于调用方
        nsf_analysis_mark_unavailable(generation);
        ESP_LOGW(TAG, "NSF后台分析启动失败：track=%u ret=%s",
            static_cast<unsigned>(track + 1U), esp_err_to_name(ret));
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "NSF后台时长分析运行：track=%u core=%d priority=%u",
        static_cast<unsigned>(track + 1U),
        static_cast<int>(NSF_ANALYSIS_TASK_CORE),
        static_cast<unsigned>(NSF_ANALYSIS_TASK_PRIORITY));

    // Preview先追到真实播放前方约5秒，再启动长时间Loop分析；
    // 只协调启动负载，两个任务的数据和Synth仍完全独立。
    uint32_t preview_waited_ms = 0U;
    while (nsf_analysis_generation_current(generation) &&
           preview_waited_ms < NSF_ANALYSIS_PREVIEW_WAIT_MS &&
           !nsf_lookahead_ready_for_track(track)) {
        vTaskDelay(pdMS_TO_TICKS(NSF_ANALYSIS_PREVIEW_POLL_MS));
        preview_waited_ms += NSF_ANALYSIS_PREVIEW_POLL_MS;
    }

    int32_t scratch[NSF_ANALYSIS_RENDER_FRAMES * 2U] = {};
    uint64_t published_hint_ms = 0ULL;
    while (nsf_analysis_generation_current(generation)) {
        size_t frames = 0U;
        ret = nsf_synth_render_pcm32(
            &synth, scratch, NSF_ANALYSIS_RENDER_FRAMES, &frames);
        if (ret != ESP_OK || frames == 0U) {
            nsf_analysis_mark_unavailable(generation);
            ESP_LOGW(TAG, "NSF后台分析失败：track=%u ret=%s",
                static_cast<unsigned>(track + 1U), esp_err_to_name(ret));
            break;
        }

        const uint64_t current_virtual_ms =
            nsf_synth_position_frames(&synth) * 1000ULL / NSF_ANALYSIS_SAMPLE_RATE_HZ;

        NsfSynthLoopInfo loop = {};
        const bool has_loop_info = nsf_synth_get_loop_info(&synth, &loop);
        if (has_loop_info && loop.hint_available && loop.hint_length_frames > 0ULL &&
            !loop.detected) {
            const uint64_t hint_start_ms =
                loop.hint_start_frame * 1000ULL / NSF_ANALYSIS_SAMPLE_RATE_HZ;
            const uint64_t hint_length_ms =
                loop.hint_length_frames * 1000ULL / NSF_ANALYSIS_SAMPLE_RATE_HZ;
            const uint64_t hint_duration_ms = nsf_loop_boundary_nearest(
                hint_start_ms, hint_length_ms, NSF_LOOP_TARGET_MS);
            if (hint_duration_ms != 0ULL && hint_duration_ms != published_hint_ms) {
                published_hint_ms = hint_duration_ms;
                nsf_analysis_publish_hint(generation, track, hint_duration_ms);
                ESP_LOGI(TAG,
                    "NSF后台Loop时长提示：track=%u start=%llums loop=%llums duration=%llums verified=2_cycles",
                    static_cast<unsigned>(track + 1U),
                    static_cast<unsigned long long>(hint_start_ms),
                    static_cast<unsigned long long>(hint_length_ms),
                    static_cast<unsigned long long>(hint_duration_ms));
            }
        }
        if (has_loop_info && loop.detected && loop.length_frames > 0ULL) {
            const uint64_t loop_start_ms =
                loop.start_frame * 1000ULL / NSF_ANALYSIS_SAMPLE_RATE_HZ;
            const uint64_t loop_length_ms =
                loop.length_frames * 1000ULL / NSF_ANALYSIS_SAMPLE_RATE_HZ;
            const uint64_t duration_ms = nsf_loop_boundary_nearest(
                loop_start_ms, loop_length_ms, NSF_LOOP_TARGET_MS);
            nsf_analysis_publish(
                generation,
                track,
                NsfEndPlanKind::VerifiedLoop,
                loop_start_ms,
                loop_length_ms,
                duration_ms);
            ESP_LOGI(TAG,
                "NSF后台Loop分析完成：track=%u start=%llums loop=%llums duration=%llums virtual=%llums",
                static_cast<unsigned>(track + 1U),
                static_cast<unsigned long long>(loop_start_ms),
                static_cast<unsigned long long>(loop_length_ms),
                static_cast<unsigned long long>(duration_ms),
                static_cast<unsigned long long>(current_virtual_ms));
            break;
        }

        NsfSynthActivityInfo activity = {};
        if (nsf_synth_get_activity_info(&synth, &activity) && activity.seen_audible) {
            const uint64_t silence_confirm_frames =
                static_cast<uint64_t>(NSF_ANALYSIS_SAMPLE_RATE_HZ) *
                (NSF_SILENCE_END_MS + NSF_ANALYSIS_SILENCE_VERIFY_MS) / 1000ULL;
            if (activity.silent_frames >= silence_confirm_frames) {
                const uint64_t position_frames = nsf_synth_position_frames(&synth);
                const uint64_t silence_start_frame =
                    position_frames >= activity.silent_frames
                        ? position_frames - activity.silent_frames
                        : 0ULL;
                const uint64_t duration_ms =
                    silence_start_frame * 1000ULL / NSF_ANALYSIS_SAMPLE_RATE_HZ;
                nsf_analysis_publish(
                    generation,
                    track,
                    NsfEndPlanKind::NaturalSilence,
                    0ULL,
                    0ULL,
                    duration_ms);
                ESP_LOGI(TAG,
                    "NSF后台静音分析完成：track=%u silence_start=%llums duration=%llums verified=%lums",
                    static_cast<unsigned>(track + 1U),
                    static_cast<unsigned long long>(duration_ms),
                    static_cast<unsigned long long>(duration_ms),
                    static_cast<unsigned long>(
                        NSF_SILENCE_END_MS + NSF_ANALYSIS_SILENCE_VERIFY_MS));
                break;
            }
        }

        if (current_virtual_ms >= NSF_ANALYSIS_MAX_MS) {
            nsf_analysis_publish(
                generation,
                track,
                NsfEndPlanKind::HardLimit,
                0ULL,
                0ULL,
                NSF_UNKNOWN_HARD_END_MS);
            ESP_LOGI(TAG, "NSF后台分析完成：track=%u %lus内未发现可靠Loop或自然静音，采用%lus硬上限",
                static_cast<unsigned>(track + 1U),
                static_cast<unsigned long>(NSF_ANALYSIS_MAX_MS / 1000U),
                static_cast<unsigned long>(NSF_UNKNOWN_HARD_END_MS / 1000U));
            break;
        }

        // 每个小块后固定休眠，避免后台模拟持续占满 CPU1；让 LVGL 和 IDLE1 都能持续获得时间片。
        vTaskDelay(NSF_ANALYSIS_YIELD_TICKS);
    }

    nsf_synth_close(&synth);
    vTaskDelete(nullptr);
}

static void nsf_preview_task(void *arg)
{
    NsfPreviewTaskArgs *args = static_cast<NsfPreviewTaskArgs *>(arg);
    if (args == nullptr || args->prg == nullptr || args->prg_size == 0U) {
        if (args != nullptr) heap_caps_free(args);
        vTaskDelete(nullptr);
        return;
    }

    const uint32_t generation = args->generation;
    const uint8_t track = args->config.track;
    uint8_t *owned_prg = args->prg;
    const size_t prg_size = args->prg_size;
    NsfSynthConfig config = args->config;
    heap_caps_free(args);

    NsfSynth synth = {};
    esp_err_t ret = nsf_synth_open_owned(
        &synth, owned_prg, prg_size, &config, NSF_PREVIEW_SAMPLE_RATE_HZ);
    if (ret != ESP_OK) {
        heap_caps_free(owned_prg);
        ESP_LOGW(TAG, "NSF未来4秒预读启动失败：track=%u ret=%s",
            static_cast<unsigned>(track + 1U), esp_err_to_name(ret));
        vTaskDelete(nullptr);
        return;
    }

    NsfLookaheadBuilder builder = {};
    builder.events = static_cast<AudioNsfVisualEvent *>(heap_caps_calloc(
        NSF_LOOKAHEAD_MAX_EVENTS,
        sizeof(AudioNsfVisualEvent),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (builder.events == nullptr) {
        ESP_LOGW(TAG, "NSF未来4秒预读事件PSRAM申请失败：track=%u",
            static_cast<unsigned>(track + 1U));
        nsf_synth_close(&synth);
        vTaskDelete(nullptr);
        return;
    }
    builder.capacity = NSF_LOOKAHEAD_MAX_EVENTS;
    ESP_LOGI(TAG, "NSF未来4秒独立预读运行：track=%u core=%d priority=%u lead=%lums",
        static_cast<unsigned>(track + 1U),
        static_cast<int>(NSF_PREVIEW_TASK_CORE),
        static_cast<unsigned>(NSF_PREVIEW_TASK_PRIORITY),
        static_cast<unsigned long>(NSF_PREVIEW_LEAD_MS));

    int32_t scratch[NSF_PREVIEW_RENDER_FRAMES * 2U] = {};
    NsfSynthVisualTick visual_ticks[32] = {};
    uint64_t last_publish_position_ms = UINT64_MAX;
    bool ready_logged = false;
    uint32_t status_publish_count = 0U;

    while (nsf_preview_generation_current(generation)) {
        AudioNsfClockSnapshot clock = {};
        if (!audio_service_nsf_get_clock(&clock) || !clock.active || clock.failed ||
            clock.eof || clock.track != track) {
            vTaskDelay(pdMS_TO_TICKS(NSF_PREVIEW_IDLE_DELAY_MS) + 1U);
            continue;
        }

        // 发布按真实位置步长独立进行，不依赖是否追平领先：即使预读暂时落后，
        // 也持续小步发布，避免积压整段后一次性灌入造成瀑布"整块刷新"。
        const bool should_publish =
            last_publish_position_ms == UINT64_MAX ||
            clock.position_ms >= last_publish_position_ms + NSF_PREVIEW_PUBLISH_STEP_MS;
        if (should_publish) {
            const uint32_t now_ms = clock.position_ms > UINT32_MAX
                ? UINT32_MAX
                : static_cast<uint32_t>(clock.position_ms);
            // 事件时间戳是歌曲时间轴（=真实播放时间轴）。prune 阈值延后一个历史窗，
            // 让刚结束的音符多停留 2s，供 UI 在播放线下方绘制历史轨迹。
            const uint32_t history_cutoff_ms = now_ms > NSF_VISUAL_HISTORY_MS
                ? now_ms - NSF_VISUAL_HISTORY_MS
                : 0U;
            nsf_lookahead_builder_prune_before(&builder, history_cutoff_ms);
            uint64_t virtual_now =
                nsf_synth_position_frames(&synth) * 1000ULL / NSF_PREVIEW_SAMPLE_RATE_HZ;
            const uint32_t preview_end_ms = virtual_now > UINT32_MAX
                ? UINT32_MAX
                : static_cast<uint32_t>(virtual_now);
            const size_t published_count = nsf_lookahead_publish_snapshot(
                generation, track, &builder, preview_end_ms);
            if (published_count > 0U && !ready_logged) {
                ready_logged = true;
                ESP_LOGI(TAG,
                    "NSF独立未来4秒预读已就绪：track=%u now=%llums ahead=%llums events=%u",
                    static_cast<unsigned>(track + 1U),
                    static_cast<unsigned long long>(clock.position_ms),
                    static_cast<unsigned long long>(
                        virtual_now > clock.position_ms ? virtual_now - clock.position_ms : 0ULL),
                    static_cast<unsigned>(published_count));
            }
            // 周期性打印持续领先量，用于核对预读是否跟得上播放。
            ++status_publish_count;
            if ((status_publish_count % 60U) == 0U) {
                ESP_LOGI(TAG,
                    "NSF预读状态：track=%u ahead=%llums events=%u",
                    static_cast<unsigned>(track + 1U),
                    static_cast<unsigned long long>(
                        virtual_now > clock.position_ms ? virtual_now - clock.position_ms : 0ULL),
                    static_cast<unsigned>(published_count));
            }
            last_publish_position_ms = clock.position_ms;
        }

        const uint64_t target_ms = clock.position_ms + NSF_PREVIEW_LEAD_MS;
        uint64_t virtual_ms =
            nsf_synth_position_frames(&synth) * 1000ULL / NSF_PREVIEW_SAMPLE_RATE_HZ;

        if (virtual_ms < target_ms) {
            size_t frames = 0U;
            ret = nsf_synth_render_pcm32(
                &synth, scratch, NSF_PREVIEW_RENDER_FRAMES, &frames);
            if (ret != ESP_OK || frames == 0U) {
                ESP_LOGW(TAG, "NSF未来4秒预读失败：track=%u ret=%s",
                    static_cast<unsigned>(track + 1U), esp_err_to_name(ret));
                break;
            }

            const size_t visual_tick_count = nsf_synth_take_visual_ticks(
                &synth, visual_ticks, sizeof(visual_ticks) / sizeof(visual_ticks[0]));
            for (size_t i = 0U; i < visual_tick_count; ++i) {
                nsf_lookahead_builder_push_tick(&builder, visual_ticks[i]);
            }

            // 只追到真实播放前方约5秒；每个小块后固定休眠，避免与 LVGL/IDLE1 争满 CPU1。
            vTaskDelay(NSF_PREVIEW_RENDER_YIELD_TICKS);
        } else {
            vTaskDelay(pdMS_TO_TICKS(NSF_PREVIEW_IDLE_DELAY_MS) + 1U);
        }
    }

    if (builder.truncated) {
        ESP_LOGW(TAG, "NSF未来4秒预读事件达到上限：track=%u max=%u",
            static_cast<unsigned>(track + 1U),
            static_cast<unsigned>(NSF_LOOKAHEAD_MAX_EVENTS));
    }
    heap_caps_free(builder.events);
    nsf_synth_close(&synth);
    vTaskDelete(nullptr);
}

static void audio_task_cancel_nsf_preview()
{
    portENTER_CRITICAL(&g_nsf_preview_mux);
    ++g_nsf_preview_generation;
    if (g_nsf_preview_generation == 0U) ++g_nsf_preview_generation;
    portEXIT_CRITICAL(&g_nsf_preview_mux);
    nsf_lookahead_clear_published();
}

static bool audio_task_start_nsf_preview()
{
    audio_task_cancel_nsf_preview();
    if (!nsf_synth_is_open(&g_nsf_synth)) return false;

    uint8_t *prg = nullptr;
    size_t prg_size = 0U;
    NsfSynthConfig config = {};
    const esp_err_t copy_ret = nsf_synth_copy_source(
        &g_nsf_synth, &prg, &prg_size, &config);
    if (copy_ret != ESP_OK) {
        ESP_LOGW(TAG, "NSF未来4秒预读复制源失败：%s；不影响播放和时长分析",
            esp_err_to_name(copy_ret));
        return false;
    }

    NsfPreviewTaskArgs *args = static_cast<NsfPreviewTaskArgs *>(heap_caps_calloc(
        1U, sizeof(NsfPreviewTaskArgs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (args == nullptr) {
        heap_caps_free(prg);
        return false;
    }

    portENTER_CRITICAL(&g_nsf_preview_mux);
    const uint32_t generation = g_nsf_preview_generation;
    portEXIT_CRITICAL(&g_nsf_preview_mux);

    config.enable_loop_detection = false;
    config.enable_visual_capture = true;
    args->prg = prg;
    args->prg_size = prg_size;
    args->config = config;
    args->generation = generation;

    const BaseType_t created = xTaskCreatePinnedToCore(
        nsf_preview_task,
        "NsfPreviewTask",
        NSF_PREVIEW_TASK_STACK_BYTES,
        args,
        NSF_PREVIEW_TASK_PRIORITY,
        nullptr,
        NSF_PREVIEW_TASK_CORE);
    if (created != pdPASS) {
        heap_caps_free(prg);
        heap_caps_free(args);
        ESP_LOGW(TAG, "创建 NSF 未来4秒预读任务失败：不影响播放和时长分析");
        return false;
    }
    return true;
}

static void audio_task_cancel_nsf_analysis()
{
    portENTER_CRITICAL(&g_nsf_analysis_mux);
    ++g_nsf_analysis_generation;
    if (g_nsf_analysis_generation == 0U) ++g_nsf_analysis_generation;
    g_nsf_analysis_pending = false;
    g_nsf_analysis_result = {};
    portEXIT_CRITICAL(&g_nsf_analysis_mux);
}

static bool audio_task_start_nsf_analysis()
{
    audio_task_cancel_nsf_analysis();
    if (!nsf_synth_is_open(&g_nsf_synth)) return false;

    uint8_t *prg = nullptr;
    size_t prg_size = 0U;
    NsfSynthConfig config = {};
    const esp_err_t copy_ret = nsf_synth_copy_source(
        &g_nsf_synth, &prg, &prg_size, &config);
    if (copy_ret != ESP_OK) {
        ESP_LOGW(TAG, "NSF后台分析复制源失败：%s；本次使用静音结束",
            esp_err_to_name(copy_ret));
        return false;
    }

    NsfAnalysisTaskArgs *args = static_cast<NsfAnalysisTaskArgs *>(heap_caps_calloc(
        1U, sizeof(NsfAnalysisTaskArgs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (args == nullptr) {
        heap_caps_free(prg);
        return false;
    }

    portENTER_CRITICAL(&g_nsf_analysis_mux);
    const uint32_t generation = g_nsf_analysis_generation;
    g_nsf_analysis_pending = true;
    portEXIT_CRITICAL(&g_nsf_analysis_mux);

    config.enable_loop_detection = true;
    config.enable_visual_capture = false;
    args->prg = prg;
    args->prg_size = prg_size;
    args->config = config;
    args->generation = generation;

    const BaseType_t created = xTaskCreatePinnedToCore(
        nsf_analysis_task,
        "NsfAnalysisTask",
        NSF_ANALYSIS_TASK_STACK_BYTES,
        args,
        NSF_ANALYSIS_TASK_PRIORITY,
        nullptr,
        NSF_ANALYSIS_TASK_CORE);
    if (created != pdPASS) {
        nsf_analysis_mark_unavailable(generation);
        heap_caps_free(prg);
        heap_caps_free(args);
        ESP_LOGW(TAG, "创建 NSF 后台分析任务失败：本次使用静音结束");
        return false;
    }
    return true;
}

static bool audio_task_nsf_analysis_is_pending()
{
    bool pending = false;
    portENTER_CRITICAL(&g_nsf_analysis_mux);
    pending = g_nsf_analysis_pending;
    portEXIT_CRITICAL(&g_nsf_analysis_mux);
    return pending;
}

static uint64_t audio_task_get_nsf_analysis_duration_hint(uint8_t track)
{
    uint64_t duration_ms = 0ULL;
    portENTER_CRITICAL(&g_nsf_analysis_mux);
    if (g_nsf_analysis_result.generation == g_nsf_analysis_generation &&
        g_nsf_analysis_result.track == track) {
        duration_ms = g_nsf_analysis_result.duration_hint_ms;
    }
    portEXIT_CRITICAL(&g_nsf_analysis_mux);
    return duration_ms;
}

static bool audio_task_get_nsf_analysis_result(NsfAnalysisResult *out_result)
{
    if (out_result == nullptr) return false;
    bool available = false;
    portENTER_CRITICAL(&g_nsf_analysis_mux);
    if (g_nsf_analysis_result.complete &&
        g_nsf_analysis_result.generation == g_nsf_analysis_generation) {
        *out_result = g_nsf_analysis_result;
        available = true;
    }
    portEXIT_CRITICAL(&g_nsf_analysis_mux);
    return available;
}

static void audio_task_sync_nsf_analysis_end_plan()
{
    if (g_nsf_end_plan_kind != NsfEndPlanKind::None || !g_nsf_active ||
        g_nsf_synth.sample_rate_hz == 0U) {
        return;
    }

    NsfAnalysisResult result = {};
    if (!audio_task_get_nsf_analysis_result(&result) || result.duration_ms == 0ULL ||
        result.track != nsf_synth_track(&g_nsf_synth)) {
        return;
    }

    const uint64_t rate = g_nsf_synth.sample_rate_hz;
    if (result.end_kind == NsfEndPlanKind::NaturalSilence) {
        const uint64_t end_frame = result.duration_ms * rate / 1000ULL;
        if (end_frame == 0ULL) return;

        // 后台通常远快于实时播放；若结果到达时已经错过自然结束点，不倒切时间轴，改用实时静音兜底。
        const uint64_t current_frame = nsf_synth_position_frames(&g_nsf_synth);
        if (end_frame <= current_frame) {
            ESP_LOGW(TAG,
                "NSF后台自然结束结果到达过晚：end=%llums current=%llums，改用实时静音兜底",
                static_cast<unsigned long long>(result.duration_ms),
                static_cast<unsigned long long>(current_frame * 1000ULL / rate));
            return;
        }

        g_nsf_end_frame = end_frame;
        g_nsf_end_plan_kind = NsfEndPlanKind::NaturalSilence;
        ESP_LOGI(TAG, "NSF自然结束计划已采用后台分析：end=%llums",
            static_cast<unsigned long long>(result.duration_ms));
        return;
    }

    if (result.end_kind == NsfEndPlanKind::HardLimit) {
        const uint64_t fade_frames = rate * NSF_TRACK_FADE_MS / 1000ULL;
        uint64_t end_frame = result.duration_ms * rate / 1000ULL;
        if (end_frame == 0ULL) return;

        // 若后台在低负载异常下过晚返回，至少从当前点保留完整淡出，避免突然截断。
        const uint64_t earliest_end =
            nsf_synth_position_frames(&g_nsf_synth) + fade_frames;
        if (end_frame < earliest_end) end_frame = earliest_end;

        g_nsf_end_frame = end_frame;
        g_nsf_end_plan_kind = NsfEndPlanKind::HardLimit;
        ESP_LOGI(TAG, "NSF结束计划采用硬上限：target=%lums end=%llums fade=%lums",
            static_cast<unsigned long>(NSF_UNKNOWN_HARD_END_MS),
            static_cast<unsigned long long>(g_nsf_end_frame * 1000ULL / rate),
            static_cast<unsigned long>(NSF_TRACK_FADE_MS));
        return;
    }

    if (result.end_kind != NsfEndPlanKind::VerifiedLoop) return;
    if (result.loop_length_ms == 0ULL) return;
    const uint64_t fade_frames = rate * NSF_TRACK_FADE_MS / 1000ULL;
    const uint64_t loop_start_frame = result.loop_start_ms * rate / 1000ULL;
    const uint64_t loop_length_frame = result.loop_length_ms * rate / 1000ULL;
    uint64_t end_frame = result.duration_ms * rate / 1000ULL;
    if (loop_length_frame == 0ULL || end_frame == 0ULL) return;

    // 后台正常会远早于播放到达目标；若极端负载导致结果过晚，就顺延一个完整 Loop 保留淡出。
    const uint64_t earliest_end =
        nsf_synth_position_frames(&g_nsf_synth) + fade_frames;
    if (end_frame < earliest_end) {
        end_frame = nsf_loop_boundary_at_or_after(
            loop_start_frame, loop_length_frame, earliest_end);
    }

    g_nsf_end_frame = end_frame;
    g_nsf_end_plan_kind = NsfEndPlanKind::VerifiedLoop;
    ESP_LOGI(TAG,
        "NSF结束计划已采用后台分析：start=%llums loop=%llums target=%lums end=%llums",
        static_cast<unsigned long long>(result.loop_start_ms),
        static_cast<unsigned long long>(result.loop_length_ms),
        static_cast<unsigned long>(NSF_LOOP_TARGET_MS),
        static_cast<unsigned long long>(g_nsf_end_frame * 1000ULL / rate));
}

static void audio_task_reset_nsf_end_policy()
{
    g_nsf_eof = false;
    g_nsf_seen_audible = false;
    g_nsf_silence_frames = 0ULL;
    g_nsf_end_plan_kind = NsfEndPlanKind::None;
    g_nsf_end_frame = 0ULL;
}

static int32_t audio_task_nsf_block_peak_pcm16(const int32_t *pcm, size_t frames)
{
    int32_t peak = 0;
    for (size_t i = 0U; i < frames; ++i) {
        int32_t sample = pcm[i * 2U] >> 16;
        if (sample < 0) sample = -sample;
        if (sample > peak) peak = sample;
    }
    return peak;
}

static bool audio_task_update_nsf_silence_end(const int32_t *pcm, size_t frames)
{
    if (g_nsf_synth.sample_rate_hz == 0U || frames == 0U) return false;
    const int32_t peak = audio_task_nsf_block_peak_pcm16(pcm, frames);
    if (peak > NSF_SILENCE_PEAK_PCM16) {
        g_nsf_seen_audible = true;
        g_nsf_silence_frames = 0ULL;
        return false;
    }
    if (!g_nsf_seen_audible) return false;

    g_nsf_silence_frames += frames;
    const uint64_t silence_limit =
        static_cast<uint64_t>(g_nsf_synth.sample_rate_hz) * NSF_SILENCE_END_MS / 1000ULL;
    return g_nsf_silence_frames >= silence_limit;
}

static bool audio_task_reached_nsf_natural_end()
{
    if (g_nsf_synth.sample_rate_hz == 0U) return false;
    audio_task_sync_nsf_analysis_end_plan();
    return g_nsf_end_plan_kind == NsfEndPlanKind::NaturalSilence &&
        nsf_synth_position_frames(&g_nsf_synth) >= g_nsf_end_frame;
}

static bool audio_task_apply_nsf_fade_end(int32_t *pcm, size_t frames)
{
    if (g_nsf_synth.sample_rate_hz == 0U || frames == 0U) return false;
    audio_task_sync_nsf_analysis_end_plan();
    if (g_nsf_end_plan_kind != NsfEndPlanKind::VerifiedLoop &&
        g_nsf_end_plan_kind != NsfEndPlanKind::HardLimit) {
        return false;
    }

    const uint64_t rate = g_nsf_synth.sample_rate_hz;
    const uint64_t current_frame = nsf_synth_position_frames(&g_nsf_synth);
    const uint64_t block_start = current_frame >= frames ? current_frame - frames : 0ULL;
    const uint64_t fade_frames = rate * NSF_TRACK_FADE_MS / 1000ULL;

    const uint64_t fade_start =
        g_nsf_end_frame > fade_frames ? g_nsf_end_frame - fade_frames : 0ULL;
    if (current_frame <= fade_start) return false;

    for (size_t frame = 0U; frame < frames; ++frame) {
        const uint64_t absolute_frame = block_start + frame;
        int32_t gain_q15 = 32767;
        if (absolute_frame >= fade_start) {
            const uint64_t remaining = absolute_frame < g_nsf_end_frame
                ? g_nsf_end_frame - absolute_frame
                : 0ULL;
            gain_q15 = fade_frames != 0ULL
                ? static_cast<int32_t>((remaining * 32767ULL) / fade_frames)
                : 0;
        }
        pcm[frame * 2U] = static_cast<int32_t>(
            (static_cast<int64_t>(pcm[frame * 2U]) * gain_q15) >> 15);
        pcm[frame * 2U + 1U] = static_cast<int32_t>(
            (static_cast<int64_t>(pcm[frame * 2U + 1U]) * gain_q15) >> 15);
    }
    return current_frame >= g_nsf_end_frame;
}

static void audio_task_finish_nsf_track(const char *reason)
{
    if (g_nsf_eof) return;
    g_nsf_eof = true;
    g_pcm_unmute_pending = false;
    audio_task_reset_pcm_fade_in();
    (void)cs43131_set_pcm_mute(true);
    audio_task_publish_nsf_clock_snapshot();
    ESP_LOGI(TAG, "NSF Track结束：reason=%s track=%u/%u position=%llums",
        reason != nullptr ? reason : "unknown",
        static_cast<unsigned>(g_nsf_synth.track + 1U),
        static_cast<unsigned>(g_nsf_synth.track_count),
        static_cast<unsigned long long>(audio_playback_clock_position_ms(&g_nsf_playback_clock)));
}

static esp_err_t audio_task_stop_nsf_internal(bool restore_music_hardware, const char *reason)
{
    const bool should_restore = restore_music_hardware && g_nsf_restore_paused_music_hardware;
    audio_task_cancel_nsf_analysis();
    audio_task_cancel_nsf_preview();
    if (!g_nsf_active && !nsf_synth_is_open(&g_nsf_synth)) {
        nsf_visual_release_playback();
        esp_err_t ret = ESP_OK;
        if (should_restore) ret = audio_task_restore_paused_music_hardware_for_nsf(reason);
        if (restore_music_hardware && ret == ESP_OK) {
            g_nsf_restore_paused_music_hardware = false;
        }
        g_nsf_paused = false;
        g_nsf_failed = false;
        audio_task_reset_nsf_end_policy();
        audio_task_publish_nsf_clock_snapshot();
        return ret;
    }

    esp_err_t first_error = ESP_OK;
    const uint32_t rate = g_nsf_synth.sample_rate_hz > 0U ? g_nsf_synth.sample_rate_hz : 48000U;
    audio_task_remember_first_error(
        audio_task_shutdown_output_hardware(rate, "NSF-2A03"), &first_error);

    nsf_synth_close(&g_nsf_synth);
    nsf_visual_release_playback();
    g_nsf_active = false;
    g_nsf_paused = false;
    g_nsf_failed = false;
    audio_task_reset_nsf_end_policy();
    audio_playback_clock_reset(&g_nsf_playback_clock, 0U);
    audio_task_publish_nsf_clock_snapshot();

    if (should_restore) {
        audio_task_remember_first_error(
            audio_task_restore_paused_music_hardware_for_nsf(reason), &first_error);
    }
    if (restore_music_hardware && first_error == ESP_OK) {
        g_nsf_restore_paused_music_hardware = false;
    }

    ESP_LOGI(TAG, "NSF 2A03已停止：恢复Music硬件=%u 结果=%s",
        static_cast<unsigned>(should_restore), esp_err_to_name(first_error));
    return first_error;
}

static void audio_task_handle_nsf_start(AudioRequest *request)
{
    if (request == nullptr) return;
    g_task_last_request_id = request->request_id;

    if (g_nsf_active || nsf_synth_is_open(&g_nsf_synth) || request->nsf_prg == nullptr ||
        request->nsf_prg_size == 0U || request->nsf_config.track_count == 0U) {
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }
    if (g_video_mp3_active || mp3_decoder_is_open(&g_video_mp3_decoder)) {
        ESP_LOGE(TAG, "NSF启动被拒绝：其它临时音频仍在活动");
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }
    if (g_task_state == AudioPlaybackState::Playing ||
        g_task_state == AudioPlaybackState::Preparing ||
        g_task_state == AudioPlaybackState::Seeking) {
        ESP_LOGE(TAG, "NSF启动要求Music先暂停：当前state=%s",
            audio_playback_state_name_cn(g_task_state));
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    g_nsf_restore_paused_music_hardware =
        g_task_state == AudioPlaybackState::Paused && pcm_decoder_is_open(&g_decoder);

    if (g_pipeline_headphone_enabled || g_pipeline_clock_prepared ||
        g_pipeline_i2s_started || i2s_output_is_started()) {
        const uint32_t active_rate = g_task_sample_rate_hz > 0U ? g_task_sample_rate_hz : 48000U;
        const esp_err_t shutdown_ret = audio_task_shutdown_output_hardware(active_rate, "Music-Paused");
        if (shutdown_ret != ESP_OK) {
            g_nsf_restore_paused_music_hardware = false;
            audio_request_complete(request, false, shutdown_ret);
            return;
        }
    }

    uint8_t *owned_prg = request->nsf_prg;
    NsfSynthConfig playback_config = request->nsf_config;
    playback_config.enable_loop_detection = false;
    playback_config.enable_visual_capture = true;
    esp_err_t ret = nsf_synth_open_owned(
        &g_nsf_synth,
        owned_prg,
        request->nsf_prg_size,
        &playback_config,
        48000U);
    if (ret != ESP_OK) {
        if (g_nsf_restore_paused_music_hardware) {
            const esp_err_t restore_ret =
                audio_task_restore_paused_music_hardware_for_nsf("nsf_open_failed");
            if (restore_ret == ESP_OK) g_nsf_restore_paused_music_hardware = false;
        }
        audio_request_complete(request, false, ret);
        return;
    }
    request->nsf_prg = nullptr; // 所有权已转移给 NsfSynth
    nsf_visual_reset_playback(g_nsf_synth.track);

    audio_playback_clock_reset(&g_nsf_playback_clock, g_nsf_synth.sample_rate_hz);
    ret = audio_task_start_output_hardware(
        g_nsf_synth.sample_rate_hz,
        16U,
        2U,
        "NSF-2A03",
        nullptr,
        true,
        "nsf");
    if (ret != ESP_OK) {
        nsf_synth_close(&g_nsf_synth);
        nsf_visual_release_playback();
        if (g_nsf_restore_paused_music_hardware) {
            const esp_err_t restore_ret =
                audio_task_restore_paused_music_hardware_for_nsf("nsf_hw_failed");
            if (restore_ret == ESP_OK) g_nsf_restore_paused_music_hardware = false;
        }
        audio_request_complete(request, false, ret);
        return;
    }

    ++g_nsf_clock_revision;
    if (g_nsf_clock_revision == 0U) ++g_nsf_clock_revision;
    g_nsf_active = true;
    g_nsf_paused = false;
    g_nsf_failed = false;
    audio_task_reset_nsf_end_policy();
    (void)audio_task_start_nsf_preview();
    (void)audio_task_start_nsf_analysis();
    audio_task_publish_nsf_clock_snapshot();
    ESP_LOGI(TAG, "NSF 2A03已启动：48000Hz/16bit/2ch track=%u/%u 基础5通道",
        static_cast<unsigned>(g_nsf_synth.track + 1U),
        static_cast<unsigned>(g_nsf_synth.track_count));
    audio_request_complete(request, true, ESP_OK);
}

static void audio_task_handle_nsf_pause(AudioRequest *request)
{
    if (request == nullptr) return;
    g_task_last_request_id = request->request_id;
    if (!g_nsf_active || g_nsf_paused || g_nsf_eof || g_nsf_failed) {
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }
    const esp_err_t ret = cs43131_set_pcm_mute(true);
    if (ret == ESP_OK) {
        g_pcm_unmute_pending = false;
        audio_task_reset_pcm_fade_in();
        g_nsf_paused = true;
        audio_task_publish_nsf_clock_snapshot();
    }
    audio_request_complete(request, ret == ESP_OK, ret);
}

static void audio_task_handle_nsf_resume(AudioRequest *request)
{
    if (request == nullptr) return;
    g_task_last_request_id = request->request_id;
    if (!g_nsf_active || !g_nsf_paused || g_nsf_eof || g_nsf_failed) {
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }
    esp_err_t ret = i2s_output_stream_write_silence(
        AUDIO_STREAM_FRAMES * 2U, AUDIO_I2S_WRITE_TIMEOUT_MS);
    if (ret == ESP_OK) {
        audio_task_begin_pcm_fade_in("nsf_resume", g_nsf_synth.sample_rate_hz);
        g_pcm_unmute_pending = true;
        g_nsf_paused = false;
        audio_task_publish_nsf_clock_snapshot();
    }
    audio_request_complete(request, ret == ESP_OK, ret);
}

static void audio_task_handle_nsf_set_track(AudioRequest *request)
{
    if (request == nullptr) return;
    g_task_last_request_id = request->request_id;
    if (!g_nsf_active || !nsf_synth_is_open(&g_nsf_synth) ||
        request->nsf_track >= g_nsf_synth.track_count) {
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }

    const bool was_paused = g_nsf_paused;
    esp_err_t ret = cs43131_set_pcm_mute(true);
    if (ret == ESP_OK) ret = nsf_synth_set_track(&g_nsf_synth, request->nsf_track);
    if (ret == ESP_OK) {
        nsf_visual_reset_playback(request->nsf_track);
        audio_playback_clock_reset(&g_nsf_playback_clock, g_nsf_synth.sample_rate_hz);
        g_nsf_failed = false;
        audio_task_reset_nsf_end_policy();
        g_nsf_paused = was_paused;
        (void)audio_task_start_nsf_preview();
        (void)audio_task_start_nsf_analysis();
        if (!was_paused) {
            ret = i2s_output_stream_write_silence(
                AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
            if (ret == ESP_OK) {
                audio_task_begin_pcm_fade_in("nsf_track", g_nsf_synth.sample_rate_hz);
                g_pcm_unmute_pending = true;
            }
        } else {
            g_pcm_unmute_pending = false;
            audio_task_reset_pcm_fade_in();
        }
        audio_task_publish_nsf_clock_snapshot();
    }
    audio_request_complete(request, ret == ESP_OK, ret);
}

static void audio_task_handle_nsf_stop(AudioRequest *request)
{
    if (request == nullptr) return;
    g_task_last_request_id = request->request_id;
    const esp_err_t ret = audio_task_stop_nsf_internal(
        request->nsf_restore_music_hardware,
        "nsf_stop_command");
    audio_request_complete(request, ret == ESP_OK, ret);
}

static void audio_task_service_nsf()
{
    if (!g_nsf_active || !nsf_synth_is_open(&g_nsf_synth)) return;

    if (g_nsf_paused || g_nsf_eof || g_nsf_failed) {
        const esp_err_t silence_ret =
            i2s_output_stream_write_silence(AUDIO_STREAM_FRAMES, AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (silence_ret != ESP_OK) {
            ESP_LOGE(TAG, "NSF静音时钟维持失败：%s", esp_err_to_name(silence_ret));
        }
        return;
    }

    size_t frames = 0U;
    esp_err_t ret = nsf_synth_render_pcm32(
        &g_nsf_synth,
        g_pcm_block,
        AUDIO_STREAM_FRAMES,
        &frames);
    if (ret != ESP_OK || frames == 0U) {
        ESP_LOGE(TAG, "NSF 2A03渲染失败：%s；后续保持静音", esp_err_to_name(ret));
        (void)cs43131_set_pcm_mute(true);
        g_nsf_failed = true;
        audio_task_publish_nsf_clock_snapshot();
        return;
    }

    audio_task_update_nsf_visual_from_playback();

    const bool silence_eof = audio_task_update_nsf_silence_end(g_pcm_block, frames);
    const bool fade_eof = audio_task_apply_nsf_fade_end(g_pcm_block, frames);
    const bool natural_eof = audio_task_reached_nsf_natural_end();

    audio_playback_clock_note_decoder(
        &g_nsf_playback_clock, nsf_synth_position_frames(&g_nsf_synth));
    audio_task_apply_pcm_fade_in(g_pcm_block, frames);
    ret = audio_task_unmute_when_pcm_ready();
    if (ret == ESP_OK) {
        ret = i2s_output_stream_write_pcm32(g_pcm_block, frames, AUDIO_I2S_WRITE_TIMEOUT_MS);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NSF I2S发送失败：%s；后续保持静音", esp_err_to_name(ret));
        (void)cs43131_set_pcm_mute(true);
        g_nsf_failed = true;
        audio_task_publish_nsf_clock_snapshot();
        return;
    }

    audio_playback_clock_commit_pcm(&g_nsf_playback_clock, frames);
    if (fade_eof) {
        audio_task_finish_nsf_track(
            g_nsf_end_plan_kind == NsfEndPlanKind::HardLimit
                ? "unresolved_360s_hard_limit"
                : "loop_boundary_near_3m");
    } else if (natural_eof) {
        audio_task_finish_nsf_track("silence_start_analyzed");
    } else if (silence_eof && g_nsf_end_plan_kind == NsfEndPlanKind::None &&
               !audio_task_nsf_analysis_is_pending()) {
        // 只有后台未得到可靠自然结束点时，才保留实时6秒静音作为异常兜底。
        audio_task_finish_nsf_track("silence_6s_fallback");
    } else {
        audio_task_publish_nsf_clock_snapshot();
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
        pcm_decoder_is_open(&g_decoder) ||
        nsf_synth_is_open(&g_nsf_synth);
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
    if (g_video_mp3_active || mp3_decoder_is_open(&g_video_mp3_decoder)) {
        (void)audio_task_stop_video_mp3_internal(false, "music_play_override");
    }
    if (g_nsf_active || nsf_synth_is_open(&g_nsf_synth)) {
        (void)audio_task_stop_nsf_internal(false, "music_play_override");
    }
    // 新 Music Play 会完整重建自己的 pipeline，不再需要临时音源恢复旧 Paused 硬件。
    g_nsf_restore_paused_music_hardware = false;
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
        ESP_LOGW(TAG, "统一 PCM Core 已接入 WAV/FLAC/MP3；%s 解码器尚未接入",
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
    if (g_video_mp3_active || mp3_decoder_is_open(&g_video_mp3_decoder)) {
        (void)audio_task_stop_video_mp3_internal(false, "global_stop");
    }
    if (g_nsf_active || nsf_synth_is_open(&g_nsf_synth)) {
        (void)audio_task_stop_nsf_internal(false, "global_stop");
    }
    g_nsf_restore_paused_music_hardware = false;
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
    if (g_video_mp3_active || g_nsf_active) {
        ESP_LOGW(TAG, "临时音频活动期间拒绝Music Pause重复请求");
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }
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
    if (g_video_mp3_active || g_nsf_active) {
        ESP_LOGW(TAG, "临时音频活动期间拒绝Music Resume；必须先停止当前临时音频");
        audio_request_complete(request, false, ESP_ERR_INVALID_STATE);
        return;
    }
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
        case AudioCommandType::VideoMp3Start:
            audio_task_handle_video_mp3_start(request);
            break;
        case AudioCommandType::VideoMp3ReleaseStart:
            audio_task_handle_video_mp3_release_start(request);
            break;
        case AudioCommandType::VideoMp3Stop:
            audio_task_handle_video_mp3_stop(request);
            break;
        case AudioCommandType::NsfStart:
            audio_task_handle_nsf_start(request);
            break;
        case AudioCommandType::NsfPause:
            audio_task_handle_nsf_pause(request);
            break;
        case AudioCommandType::NsfResume:
            audio_task_handle_nsf_resume(request);
            break;
        case AudioCommandType::NsfSetTrack:
            audio_task_handle_nsf_set_track(request);
            break;
        case AudioCommandType::NsfStop:
            audio_task_handle_nsf_stop(request);
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
#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(TAG, "AudioTask 已启动：核心=%d，优先级=%u，栈=%u字节",
        static_cast<int>(current_core),
        static_cast<unsigned>(uxTaskPriorityGet(nullptr)),
        static_cast<unsigned>(AUDIO_TASK_STACK_BYTES));
#endif

    // 音频播放依赖双核流水线：AudioTask 固定 Core 0，SD 顺序预取固定 Core 1。
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
        // 失败任务在通知启动方之前先清空全局 handle，避免调用方重试时误认为旧任务仍在运行。
        g_audio_task = nullptr;
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

    if (g_start_result != ESP_OK) {
        // 与启动握手保持同一生命周期顺序：失败结果发布前先宣布任务不再可用。
        g_audio_task = nullptr;
    }
    if (g_start_done != nullptr) {
        xSemaphoreGive(g_start_done);
    }

    if (g_start_result != ESP_OK) {
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        AudioRequest *request = nullptr;
        const bool stream_needs_service =
            g_video_mp3_active ||
            g_nsf_active ||
            g_task_state == AudioPlaybackState::Playing ||
            g_task_state == AudioPlaybackState::Paused;
        const TickType_t wait_ticks = stream_needs_service ? 0 : portMAX_DELAY;

        if (xQueueReceive(g_command_queue, &request, wait_ticks) == pdTRUE && request != nullptr) {
            audio_task_process_queue_request(request);
            audio_request_release(request); // 释放 queue 引用
        }

        if (g_video_mp3_active) {
            audio_task_service_video_mp3();
        } else if (g_nsf_active) {
            audio_task_service_nsf();
        } else if (g_task_state == AudioPlaybackState::Playing) {
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

    if (g_nsf_visual_mutex == nullptr) {
        g_nsf_visual_mutex = xSemaphoreCreateMutex();
        if (g_nsf_visual_mutex == nullptr) {
            ESP_LOGE(TAG, "创建 NSF 播放瀑布事件锁失败");
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_nsf_lookahead_mutex == nullptr) {
        g_nsf_lookahead_mutex = xSemaphoreCreateMutex();
        if (g_nsf_lookahead_mutex == nullptr) {
            ESP_LOGE(TAG, "创建 NSF 未来预读事件锁失败");
            return ESP_ERR_NO_MEM;
        }
    }

    // 启动握手使用静态信号量并贯穿服务整个生命周期。若调用方等待超时，AudioTask 仍可能
    // 稍后完成初始化并 give；此时绝不能删除信号量，否则任务会访问已经释放的内核对象。
    if (g_start_done == nullptr) {
        g_start_done = xSemaphoreCreateBinaryStatic(&g_start_done_storage);
        if (g_start_done == nullptr) {
            ESP_LOGE(TAG, "创建 AudioTask 启动信号量失败");
            return ESP_ERR_NO_MEM;
        }
    }

    // 上一次调用可能只是等待超时，AudioTask 本身仍在初始化。复用同一个握手等待它结束，
    // 禁止在旧任务尚未明确退出时再创建第二个 AudioTask。
    if (g_audio_task != nullptr) {
        if (xSemaphoreTake(g_start_done, AUDIO_START_WAIT_TIMEOUT) != pdTRUE) {
            ESP_LOGE(TAG, "等待已存在的 AudioTask 初始化超时");
            return ESP_ERR_TIMEOUT;
        }
        return g_start_result;
    }

    // 失败任务可能在上一次调用超时后才发出完成信号；创建新任务前清掉这枚陈旧 token。
    while (xSemaphoreTake(g_start_done, 0) == pdTRUE) {
    }

    g_start_result = ESP_ERR_INVALID_STATE;
    const BaseType_t task_ret = xTaskCreatePinnedToCore(
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
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(g_start_done, AUDIO_START_WAIT_TIMEOUT) != pdTRUE) {
        ESP_LOGE(TAG, "等待 AudioTask 初始化超时；任务仍在初始化，不释放启动握手对象");
        return ESP_ERR_TIMEOUT;
    }

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
        "AUDIO_FAULT_SNAPSHOT：count=%lu stage=%s err=%s track=%lu rev=%lu rate=%luHz pos=%lluf ring=%lu/%luB min=%luB qos=%u pressure=%u ioerr=%u eof=%u emergency=%lu recovered=%lu maxread=%lu RAM(internal/min/largest/dma/psram)=%lu/%lu/%lu/%lu/%lu",
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

static bool audio_service_video_mp3_submit_start(
    uint32_t sample_rate_hz,
    uint8_t channels,
    uint8_t bits_per_sample,
    bool start_barrier_armed,
    bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::VideoMp3Start, wait);
    if (request == nullptr) return false;
    request->video_sample_rate_hz = sample_rate_hz;
    request->video_channels = channels;
    request->video_bits_per_sample = bits_per_sample;
    request->video_start_barrier_armed = start_barrier_armed;
    return audio_service_submit(request, wait);
}

bool audio_service_video_mp3_start(
    uint32_t sample_rate_hz,
    uint8_t channels,
    uint8_t bits_per_sample,
    bool wait)
{
    return audio_service_video_mp3_submit_start(
        sample_rate_hz, channels, bits_per_sample, false, wait);
}

bool audio_service_video_mp3_prepare(
    uint32_t sample_rate_hz,
    uint8_t channels,
    uint8_t bits_per_sample,
    bool wait)
{
    return audio_service_video_mp3_submit_start(
        sample_rate_hz, channels, bits_per_sample, true, wait);
}

bool audio_service_video_mp3_release_start(bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::VideoMp3ReleaseStart, wait);
    return audio_service_submit(request, wait);
}

bool audio_service_video_mp3_stop(bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::VideoMp3Stop, wait);
    return audio_service_submit(request, wait);
}

bool audio_service_video_mp3_get_clock(AudioVideoClockSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    portENTER_CRITICAL(&g_video_clock_snapshot_mux);
    *out_snapshot = g_video_clock_snapshot;
    portEXIT_CRITICAL(&g_video_clock_snapshot_mux);
    return true;
}

bool audio_service_nsf_start(
    const uint8_t *prg,
    size_t prg_size,
    const NsfSynthConfig *config,
    bool wait)
{
    if (prg == nullptr || prg_size == 0U || config == nullptr ||
        config->track_count == 0U || config->track >= config->track_count) {
        return false;
    }

    AudioRequest *request = audio_request_create(AudioCommandType::NsfStart, wait);
    if (request == nullptr) return false;
    request->nsf_prg = static_cast<uint8_t *>(heap_caps_malloc(
        prg_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (request->nsf_prg == nullptr) {
        audio_request_release(request);
        return false;
    }
    memcpy(request->nsf_prg, prg, prg_size);
    request->nsf_prg_size = prg_size;
    request->nsf_config = *config;
    return audio_service_submit(request, wait);
}

bool audio_service_nsf_pause(bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::NsfPause, wait);
    return audio_service_submit(request, wait);
}

bool audio_service_nsf_resume(bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::NsfResume, wait);
    return audio_service_submit(request, wait);
}

bool audio_service_nsf_set_track(uint8_t track, bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::NsfSetTrack, wait);
    if (request == nullptr) return false;
    request->nsf_track = track;
    return audio_service_submit(request, wait);
}

bool audio_service_nsf_stop(bool restore_music_hardware, bool wait)
{
    AudioRequest *request = audio_request_create(AudioCommandType::NsfStop, wait);
    if (request == nullptr) return false;
    request->nsf_restore_music_hardware = restore_music_hardware;
    return audio_service_submit(request, wait);
}

bool audio_service_nsf_get_clock(AudioNsfClockSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    portENTER_CRITICAL(&g_nsf_clock_snapshot_mux);
    *out_snapshot = g_nsf_clock_snapshot;
    portEXIT_CRITICAL(&g_nsf_clock_snapshot_mux);
    return true;
}

size_t audio_service_nsf_copy_visual_events(
    uint8_t track,
    uint32_t window_start_ms,
    uint32_t window_end_ms,
    AudioNsfVisualEvent *out_events,
    size_t capacity)
{
    if (out_events == nullptr || capacity == 0U || window_end_ms < window_start_ms) return 0U;

    if (g_nsf_visual_mutex == nullptr ||
        xSemaphoreTake(g_nsf_visual_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0U;
    }

    size_t out_count = 0U;
    if (g_nsf_visual.available && g_nsf_visual.track == track &&
        g_nsf_visual.events != nullptr) {
        const size_t active_reserve = capacity < 3U ? 0U : 3U;
        const size_t completed_limit = capacity - active_reserve;
        // 环形缓存可能比UI窗口容量大；从最新事件向前取，保证当前听到的音符优先显示。
        for (size_t offset = 0U; offset < g_nsf_visual.count && out_count < completed_limit; ++offset) {
            const size_t reverse = g_nsf_visual.count - 1U - offset;
            const size_t index =
                (g_nsf_visual.head + reverse) % NSF_VISUAL_EVENT_CAPACITY;
            const AudioNsfVisualEvent &event = g_nsf_visual.events[index];
            if (event.end_ms < window_start_ms || event.start_ms > window_end_ms) continue;
            out_events[out_count++] = event;
        }

        // 尚在发声的长音还没有 completed event；以 UI 的真实播放时钟补到当前窗口末端。
        for (uint8_t voice = 0U; voice < 3U && out_count < capacity; ++voice) {
            const NsfPlaybackVisualVoice &state = g_nsf_visual.melodic[voice];
            if (!state.active || state.start_ms > window_end_ms) continue;
            AudioNsfVisualEvent event = {};
            event.start_ms = state.start_ms;
            event.end_ms = window_end_ms;
            event.note = state.note;
            event.level = state.level;
            event.voice = static_cast<AudioNsfVisualVoice>(voice);
            if (event.end_ms >= window_start_ms) out_events[out_count++] = event;
        }
    }

    xSemaphoreGive(g_nsf_visual_mutex);
    return out_count;
}

size_t audio_service_nsf_copy_lookahead_events(
    uint8_t track,
    uint32_t window_start_ms,
    uint32_t window_end_ms,
    AudioNsfVisualEvent *out_events,
    size_t capacity)
{
    if (out_events == nullptr || capacity == 0U || window_end_ms < window_start_ms) return 0U;
    if (g_nsf_lookahead_mutex == nullptr ||
        xSemaphoreTake(g_nsf_lookahead_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0U;
    }

    size_t out_count = 0U;
    if (!g_nsf_lookahead_info.available || g_nsf_lookahead_info.track != track ||
        g_nsf_lookahead_events == nullptr || g_nsf_lookahead_event_count == 0U) {
        xSemaphoreGive(g_nsf_lookahead_mutex);
        return 0U;
    }

    // 预读实例始终跟随真实播放向前约5秒，因此这里只复制当前绝对时间窗口，不再复用时长分析的Loop结果。
    // 从最新事件往回拷贝：容量不足时优先保住未来窗口顶部（最新音符），
    // 最早的历史先被截掉，避免密集段落时顶部空白。
    for (size_t i = g_nsf_lookahead_event_count; i > 0U && out_count < capacity; --i) {
        const AudioNsfVisualEvent &event = g_nsf_lookahead_events[i - 1U];
        if (event.end_ms < window_start_ms || event.start_ms > window_end_ms) continue;
        out_events[out_count++] = event;
    }

    xSemaphoreGive(g_nsf_lookahead_mutex);
    return out_count;
}

uint32_t audio_service_playback_revision()
{
    AudioStateSnapshot snapshot = {};
    return audio_service_get_snapshot(&snapshot) ? snapshot.playback_revision : 0;
}
