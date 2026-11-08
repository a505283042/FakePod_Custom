#include "video_benchmark.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "audio/audio_service.h"
#include "audio/sources/avi_mp3_audio_source.h"
#include "avi_demuxer.h"
#include "video_media_io.h"
#include "video_seek_index.h"
#include "video_jpeg_probe.h"

static const char *TAG = "VideoBench";

namespace VideoBenchmark
{
namespace
{

// 将 AVI 文件扩展名替换为 .fvix；不再尝试旧的 .avi.fvix 命名。
static bool build_sidecar_path(const char *avi_path, char *out_path, size_t out_size)
{
    if (avi_path == nullptr || out_path == nullptr || out_size == 0U) return false;

    const char *dot = strrchr(avi_path, '.');
    const char *slash = strrchr(avi_path, '/');
    if (dot == nullptr || (slash != nullptr && dot < slash) || strlen(dot) != 4U) return false;

    const bool is_avi =
        (dot[1] == 'a' || dot[1] == 'A') &&
        (dot[2] == 'v' || dot[2] == 'V') &&
        (dot[3] == 'i' || dot[3] == 'I');
    if (!is_avi) return false;

    const size_t stem_length = static_cast<size_t>(dot - avi_path);
    if (stem_length + sizeof(".fvix") > out_size) return false;

    memcpy(out_path, avi_path, stem_length);
    memcpy(out_path + stem_length, ".fvix", sizeof(".fvix"));
    return true;
}

static constexpr size_t kFrameSlotCount = 3U;
// 24 个 PSRAM 压缩槽中，23 个供视频队列使用（约 1.5 秒@15fps），最后一个 128KB 专供 MP3 chunk。
static constexpr size_t kCompressedSlotCount = 24U;
static constexpr size_t kCompressedVideoSlotCount = kCompressedSlotCount - 1U;
static constexpr size_t kAudioScratchSlotIndex = kCompressedVideoSlotCount;
static constexpr size_t kCompressedSlotBytes = 128U * 1024U;
static constexpr uint32_t kTaskStack = 8192U;
static constexpr uint32_t kExtractTaskStack = 6144U;
static constexpr UBaseType_t kTaskPriority = 2U;
static constexpr UBaseType_t kExtractTaskPriority = 2U;
// 视频播放双核调度：JPEG Decode=P2 固定在 Core1，避免 Core0/P5 AudioTask 对持续 CPU 解码的高频抢占。
// Extractor=P2 固定在 Core0，主要等待 SD/I/O，优先级低于 AudioTask=P5，与 Presenter=P2 同级公平调度。
static constexpr BaseType_t kTaskCore = 1;
static constexpr BaseType_t kExtractTaskCore = 0;
// R46.0.26: Decode/Extractor are video-only workers. Put only their stacks in PSRAM;
// keep the 4KB Presenter stack internal because it directly drives TE/SPI timing.
#if !defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) || !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#error "R46.0.26 requires CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y; regenerate sdkconfig from defaults"
#endif
static constexpr UBaseType_t kVideoWorkerStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
static constexpr TickType_t kQueuePollTicks = pdMS_TO_TICKS(20);
static constexpr uint32_t kPublishEveryFrames = 8U;
static constexpr uint32_t kDecodeYieldEveryFrames = 16U;
static constexpr TickType_t kDecodeYieldTicks = 1U;
static constexpr int64_t kUiEmergencyLateUs = 50000LL;
static constexpr uint32_t kDeadlineSafetyBaseUs = 5000U;
static constexpr uint32_t kDeadlineSafetyMaxUs = 12000U;
static constexpr uint32_t kDecodeJitterReserveMaxUs = 10000U;
static constexpr uint8_t kDecodeTimingEwmaShift = 3U; // alpha = 1/8
// AVI ExtractTask -> AudioTask 桥容量。96KB 约等于 4 秒 192kbps MP3。
// 视频侧反压（压缩帧队列满）时 ExtractTask 会暂停读文件，32KB 只能撑约 2 秒，
// 高码率大关键帧处会被抽干并触发音频欠载，进而把音频主时钟拖慢。
static constexpr size_t kAudioBridgeBytes = 96U * 1024U;
// 预缓冲阈值：4KB约等于192kbps MP3的170ms音频，足以防止启动欠载。
// 注意：不能设太大，某些AVI的视频/音频交错间隔大，高帧率视频需要较长时间才能凑满音频数据。
static constexpr size_t kAudioPrebufferBytes = 4U * 1024U;
static constexpr uint32_t kAudioPrebufferTimeoutMs = 1500U;
// Bridge 满是正常的生产者反压：等待 AudioTask 消费；仅在取消或 Source 关闭时退出。
static constexpr uint32_t kAudioBridgePushWaitForever = UINT32_MAX;

struct FrameSlot
{
    uint8_t *rgb565_be = nullptr;
    size_t rgb565_bytes = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint32_t pts_ms = 0U;
    uint32_t seek_epoch = 0U;  // 帧所属的 SEEK 世代，禁止旧帧进入新时间轴
    uint32_t compressed_bytes = 0U;
    uint32_t extract_us = 0U;
    uint32_t storage_us = 0U;
    uint32_t decode_us = 0U;
};

struct CompressedSlot
{
    uint8_t *data = nullptr;
    uint32_t bytes = 0U;
    uint32_t pts_ms = 0U;
    uint32_t seek_epoch = 0U;  // 提取时记录，解码前后再次校验
    uint32_t extract_us = 0U;
    uint32_t storage_us = 0U;
    uint32_t copy_us = 0U;
};

enum class PipelineMessageType : uint8_t
{
    Frame = 0,
    PoolSkipped,
    Eof,
    Stopped,
    Failed,
};

struct PipelineMessage
{
    PipelineMessageType type = PipelineMessageType::Failed;
    uint8_t slot = 0xFFU;
    esp_err_t result = ESP_FAIL;
};

struct TaskArgs
{
    char *path = nullptr; // PSRAM
    uint32_t generation = 0U;
    bool allow_fullcanvas_over20 = false;
    bool music_suspended = false;
};

struct ExtractTaskArgs
{
    VideoMediaIo::Context *io = nullptr;
    uint32_t generation = 0U;
};

struct PresentationClock
{
    bool started = false;
    uint32_t generation = 0U;
    uint32_t first_pts_ms = 0U;
    int64_t base_us = 0LL;
};

struct DecodeTimingModel
{
    uint32_t ewma_us = 0U;
    uint32_t jitter_ewma_us = 0U;
};

struct AudioProbeStats
{
    uint32_t frames_read = 0U;
    uint64_t compressed_bytes = 0ULL;
    uint32_t compressed_bytes_max = 0U;
    uint32_t first_pts_ms = 0U;
    uint32_t last_pts_ms = 0U;
    uint64_t extract_us_total = 0ULL;
    uint32_t extract_us_max = 0U;
    uint64_t storage_us_total = 0ULL;
    uint32_t storage_us_max = 0U;
};

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static Snapshot g_snapshot = {};
static uint32_t g_generation = 1U;
static TaskHandle_t g_task = nullptr;
static TaskHandle_t g_extract_task = nullptr;
static uint32_t g_stop_generation = 0U;
static bool g_pause_requested = false;
static bool g_paused = false;
static bool g_extract_pause_ack = false;
static int64_t g_pause_started_us = 0LL;
static uint32_t g_seek_epoch = 0U;
static bool g_seek_audio_resume_pending = false;  // SEEK 后首帧显示前保持音频暂停
// 暂停态 SEEK 后短暂解除冻结，只为把目标位置首帧推上屏，Present 完成后立即回到暂停。
static bool g_paused_seek_preview = false;
static esp_err_t g_seek_failure = ESP_OK;  // 本轮 SEEK 致命错误，仅随新播放世代清除
static QueueHandle_t g_free_queue = nullptr;
static QueueHandle_t g_ready_queue = nullptr;
static QueueHandle_t g_compressed_free_queue = nullptr;
static QueueHandle_t g_compressed_ready_queue = nullptr;
static FrameSlot g_slots[kFrameSlotCount] = {};
static CompressedSlot g_compressed_slots[kCompressedSlotCount] = {};
static uint8_t g_leased_mask = 0U;
static PresentationClock g_presentation_clock = {};
static AudioProbeStats g_audio_probe = {};

static BaseType_t create_video_worker_task(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_bytes,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t core)
{
    return xTaskCreatePinnedToCoreWithCaps(
        task, name, stack_bytes, arg, priority, handle, core, kVideoWorkerStackCaps);
}

static void delete_video_worker_task_self()
{
    vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
}

static void merge_audio_probe_unlocked(Snapshot *snapshot)
{
    if (snapshot == nullptr) return;
    snapshot->audio_frames_read = g_audio_probe.frames_read;
    snapshot->audio_compressed_bytes = g_audio_probe.compressed_bytes;
    snapshot->audio_compressed_bytes_max = g_audio_probe.compressed_bytes_max;
    snapshot->audio_first_pts_ms = g_audio_probe.first_pts_ms;
    snapshot->audio_last_pts_ms = g_audio_probe.last_pts_ms;
    snapshot->audio_extract_us_total = g_audio_probe.extract_us_total;
    snapshot->audio_extract_us_max = g_audio_probe.extract_us_max;
    snapshot->audio_storage_us_total = g_audio_probe.storage_us_total;
    snapshot->audio_storage_us_max = g_audio_probe.storage_us_max;
}

static void snapshot_audio_probe(Snapshot *snapshot)
{
    if (snapshot == nullptr) return;
    portENTER_CRITICAL(&g_mux);
    merge_audio_probe_unlocked(snapshot);
    portEXIT_CRITICAL(&g_mux);
}

static bool record_audio_probe(
    const AviDemuxer::FrameInfo &frame,
    uint32_t extract_us,
    uint32_t storage_us,
    uint32_t generation)
{
    bool first = false;
    portENTER_CRITICAL(&g_mux);
    if (generation == g_generation && g_stop_generation != generation) {
        first = g_audio_probe.frames_read == 0U;
        ++g_audio_probe.frames_read;
        g_audio_probe.compressed_bytes += frame.size;
        if (frame.size > g_audio_probe.compressed_bytes_max) {
            g_audio_probe.compressed_bytes_max = frame.size;
        }
        if (first) g_audio_probe.first_pts_ms = static_cast<uint32_t>(frame.pts);
        g_audio_probe.last_pts_ms = static_cast<uint32_t>(frame.pts);
        g_audio_probe.extract_us_total += extract_us;
        if (extract_us > g_audio_probe.extract_us_max) g_audio_probe.extract_us_max = extract_us;
        g_audio_probe.storage_us_total += storage_us;
        if (storage_us > g_audio_probe.storage_us_max) g_audio_probe.storage_us_max = storage_us;
    }
    portEXIT_CRITICAL(&g_mux);
    return first;
}

static bool generation_matches(uint32_t generation)
{
    bool matches = false;
    portENTER_CRITICAL(&g_mux);
    matches = generation == g_generation;
    portEXIT_CRITICAL(&g_mux);
    return matches;
}

static bool generation_current(uint32_t generation)
{
    bool current = false;
    portENTER_CRITICAL(&g_mux);
    current = generation == g_generation && g_stop_generation != generation;
    portEXIT_CRITICAL(&g_mux);
    return current;
}

static bool generation_paused(uint32_t generation)
{
    bool paused = false;
    portENTER_CRITICAL(&g_mux);
    paused = generation == g_generation && g_stop_generation != generation && g_paused;
    portEXIT_CRITICAL(&g_mux);
    return paused;
}

static bool wait_while_paused(uint32_t generation)
{
    while (generation_current(generation)) {
        if (!generation_paused(generation)) return true;
        vTaskDelay(1);
    }
    return false;
}

static bool extract_pause_gate(uint32_t generation)
{
    while (generation_current(generation)) {
        bool requested = false;
        portENTER_CRITICAL(&g_mux);
        if (generation == g_generation && g_stop_generation != generation) {
            requested = g_pause_requested;
            if (requested) g_extract_pause_ack = true;
        }
        portEXIT_CRITICAL(&g_mux);
        if (!requested) return true;
        vTaskDelay(1);
    }
    return false;
}

static void publish(const Snapshot &snapshot)
{
    portENTER_CRITICAL(&g_mux);
    if (snapshot.generation == g_generation) {
        g_snapshot = snapshot;
        if (g_seek_failure != ESP_OK) {
            g_snapshot.state = State::Failed;
            g_snapshot.result = g_seek_failure;
        }
        g_snapshot.paused = g_paused && g_snapshot.state == State::Running;
        merge_audio_probe_unlocked(&g_snapshot);
    }
    portEXIT_CRITICAL(&g_mux);
}

static void publish_task_exit(const Snapshot &snapshot)
{
    portENTER_CRITICAL(&g_mux);
    if (snapshot.generation == g_generation) {
        g_snapshot = snapshot;
        if (g_seek_failure != ESP_OK) {
            g_snapshot.state = State::Failed;
            g_snapshot.result = g_seek_failure;
        }
        g_snapshot.paused = false;
        merge_audio_probe_unlocked(&g_snapshot);
    }
    g_pause_requested = false;
    g_paused = false;
    g_extract_pause_ack = false;
    g_pause_started_us = 0LL;
    g_paused_seek_preview = false;
    g_seek_audio_resume_pending = false;
    g_task = nullptr;
    portEXIT_CRITICAL(&g_mux);
}

static void return_slot_to_free(uint8_t slot)
{
    if (g_free_queue == nullptr || slot >= kFrameSlotCount) return;
    (void)xQueueSend(g_free_queue, &slot, 0);
}

// 暂停请求/暂停生效期间，Producer 不能再阻塞在下游队列上：
// 否则 Presenter 一停，Decode 卡在 send_ready、Extract 卡在 send_pipeline_message，
// Extract 永远回不到 extract_pause_gate，seek 就等不到暂停确认而整条管线死锁。
static bool pause_transition_active()
{
    bool active = false;
    portENTER_CRITICAL(&g_mux);
    active = g_pause_requested || g_paused;
    portEXIT_CRITICAL(&g_mux);
    return active;
}

static bool send_ready(uint8_t slot, uint32_t generation, uint32_t frame_seek_epoch)
{
    while (generation_current(generation)) {
        // 队列清空时 DecodeTask 可能仍持有一帧；世代变化后不能再排入新时间轴。
        if (seek_epoch() != frame_seek_epoch || pause_transition_active()) {
            return_slot_to_free(slot);
            return true;
        }
        if (g_ready_queue != nullptr && xQueueSend(g_ready_queue, &slot, kQueuePollTicks) == pdTRUE) return true;
    }
    return false;
}

static bool take_free(uint8_t *slot, uint32_t generation)
{
    if (slot == nullptr) return false;
    while (generation_current(generation)) {
        if (g_free_queue != nullptr && xQueueReceive(g_free_queue, slot, kQueuePollTicks) == pdTRUE) return true;
    }
    return false;
}


static bool try_take_compressed_free(uint8_t *slot)
{
    return slot != nullptr && g_compressed_free_queue != nullptr &&
        xQueueReceive(g_compressed_free_queue, slot, 0) == pdTRUE;
}

// 只有 Demuxer 已确认下一个 chunk 是视频帧时才等待槽位；等待期间仍响应 SEEK 暂停。
static bool take_compressed_free_for_extract(
    uint8_t *slot, uint32_t generation, bool *out_waited)
{
    if (out_waited != nullptr) *out_waited = false;
    if (slot == nullptr || !extract_pause_gate(generation)) return false;
    if (try_take_compressed_free(slot)) return true;
    if (out_waited != nullptr) *out_waited = true;

    while (generation_current(generation)) {
        if (!extract_pause_gate(generation)) return false;
        // 队列收到 DecodeTask 归还的槽位时立即唤醒；最多等待 1 tick 后重新检查暂停请求。
        if (g_compressed_free_queue != nullptr &&
            xQueueReceive(g_compressed_free_queue, slot, 1) == pdTRUE) {
            return true;
        }
    }
    return false;
}

static void return_compressed_to_free(uint8_t slot)
{
    if (g_compressed_free_queue == nullptr || slot >= kCompressedVideoSlotCount) return;
    (void)xQueueSend(g_compressed_free_queue, &slot, 0);
}

static bool send_pipeline_message(const PipelineMessage &message, uint32_t generation)
{
    while (generation_current(generation)) {
        // 暂停/seek 期间丢弃视频帧消息（含槽归还）。Eof/Failed/Stopped 必须送达 Decode。
        if (message.type == PipelineMessageType::Frame && pause_transition_active()) {
            if (message.slot < kCompressedVideoSlotCount) return_compressed_to_free(message.slot);
            return true;
        }
        if (g_compressed_ready_queue != nullptr &&
            xQueueSend(g_compressed_ready_queue, &message, kQueuePollTicks) == pdTRUE) {
            return true;
        }
    }
    return false;
}

static bool receive_pipeline_message(PipelineMessage *message, uint32_t generation)
{
    if (message == nullptr) return false;
    while (generation_current(generation)) {
        if (g_compressed_ready_queue != nullptr &&
            xQueueReceive(g_compressed_ready_queue, message, kQueuePollTicks) == pdTRUE) {
            return true;
        }
    }
    return false;
}

static void stop_generation(uint32_t generation)
{
    portENTER_CRITICAL(&g_mux);
    if (generation == g_generation) {
        g_stop_generation = generation;
        g_pause_requested = false;
        g_paused = false;
        g_extract_pause_ack = false;
        g_pause_started_us = 0LL;
        g_paused_seek_preview = false;
        g_seek_audio_resume_pending = false;
    }
    portEXIT_CRITICAL(&g_mux);
}

static bool extract_task_running()
{
    bool running = false;
    portENTER_CRITICAL(&g_mux);
    running = g_extract_task != nullptr;
    portEXIT_CRITICAL(&g_mux);
    return running;
}

static void wait_extract_task_exit()
{
    while (extract_task_running()) vTaskDelay(1);
}

static esp_err_t wait_audio_prebuffer(
    uint32_t generation,
    AviMp3BridgeSnapshot *out_snapshot,
    uint32_t *out_wait_ms)
{
    if (out_snapshot == nullptr || out_wait_ms == nullptr) return ESP_ERR_INVALID_ARG;
    *out_snapshot = {};
    *out_wait_ms = 0U;
    const int64_t started_us = esp_timer_get_time();

    while (generation_current(generation)) {
        AviMp3BridgeSnapshot snapshot = {};
        if (!avi_mp3_bridge_get_snapshot(&snapshot) || !snapshot.active || snapshot.cancelled) {
            return ESP_ERR_INVALID_STATE;
        }
        const int64_t elapsed_signed_us = esp_timer_get_time() - started_us;
        const uint64_t elapsed_us = elapsed_signed_us > 0LL
            ? static_cast<uint64_t>(elapsed_signed_us) : 0ULL;
        *out_wait_ms = static_cast<uint32_t>(elapsed_us / 1000ULL);
        *out_snapshot = snapshot;

        if (snapshot.buffered_bytes >= kAudioPrebufferBytes) return ESP_OK;
        // 极短 AVI 可能在 4KB 前已经到 EOS；只要至少有一帧压缩音频，就允许启动并自然排空。
        if (snapshot.eof && snapshot.buffered_bytes != 0U) return ESP_OK;
        if (elapsed_us >= static_cast<uint64_t>(kAudioPrebufferTimeoutMs) * 1000ULL) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t validate_demuxer_stream(
    const AviDemuxer::StreamInfo &info,
    Snapshot *snapshot,
    bool allow_fullcanvas_over20)
{
    if (snapshot == nullptr) return ESP_ERR_INVALID_ARG;
    if (info.width == 0U || info.height == 0U || info.fps_x100 == 0U) {
        return ESP_ERR_NOT_FOUND;
    }
    snapshot->width = info.width;
    snapshot->height = info.height;
    snapshot->fps_hint = info.fps_x100 / 100U;
    snapshot->duration_ms = info.duration_ms;
    // 自定义解复用器仅支持 MJPEG（AVI strf biCompression="MJPG"），解析时已隐含。
    if (snapshot->width == 0U || snapshot->height == 0U ||
        snapshot->width > kWidth || snapshot->height > kHeight) {
        ESP_LOGE(TAG, "Video尺寸超出画布：stream=%ux%u canvas<=%ux%u",
            static_cast<unsigned>(snapshot->width), static_cast<unsigned>(snapshot->height),
            static_cast<unsigned>(kWidth), static_cast<unsigned>(kHeight));
        return ESP_ERR_INVALID_SIZE;
    }

    // 460x460 + >20FPS 在实机上可能因 JPEG 解码长期超过帧预算，最终反压 AVI Demux 并造成 MP3 断音。
    const bool fullcanvas_over20 =
        snapshot->width == kWidth && snapshot->height == kHeight && snapshot->fps_hint > 20U;
    if (fullcanvas_over20 && !allow_fullcanvas_over20) {
        ESP_LOGW(TAG,
            "Video高负载风险提示：460x460/%uFPS 超过稳定档位20FPS；本次只完成Profile探测，等待用户决定是否继续（可能断音/掉帧）",
            static_cast<unsigned>(snapshot->fps_hint));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (fullcanvas_over20) {
        ESP_LOGW(TAG,
            "Video高负载风险已由用户确认：继续460x460/%uFPS原速播放；不做运行时降帧，可能断音/掉帧",
            static_cast<unsigned>(snapshot->fps_hint));
    }

    if (info.has_audio) {
        snapshot->audio_streams = 1U;
        snapshot->audio_format = info.audio_format;
        snapshot->audio_duration_ms = info.duration_ms;
        snapshot->audio_bitrate = info.audio_bitrate;
        snapshot->audio_sample_rate = info.audio_sample_rate;
        snapshot->audio_channels = info.audio_channels;
        snapshot->audio_bits_per_sample = info.audio_bits_per_sample;
        // WAVEFORMATEX wFormatTag: 0x0055 = MP3
        if (info.audio_format != 0x0055) {
            ESP_LOGE(TAG,
                "AVI AUDIO轨暂不支持：format=0x%04x；Video V1要求MP3",
                static_cast<unsigned>(info.audio_format));
            return ESP_ERR_NOT_SUPPORTED;
        }
        ESP_LOGI(TAG,
            "AVI MP3 Audio Pipeline V1：%uHz/%uch bits=%u bitrate=%lu duration=%ums；Demuxer->32KB PSRAM Bridge->AudioTask->MP3->PCM",
            static_cast<unsigned>(snapshot->audio_sample_rate),
            static_cast<unsigned>(snapshot->audio_channels),
            static_cast<unsigned>(snapshot->audio_bits_per_sample),
            static_cast<unsigned long>(snapshot->audio_bitrate),
            static_cast<unsigned long>(snapshot->audio_duration_ms));
    } else {
        snapshot->audio_streams = 0U;
        ESP_LOGW(TAG, "AVI MP3 Audio Pipeline：未发现AUDIO轨；继续VIDEO-only兼容路径");
    }
    return ESP_OK;
}

static esp_err_t decode_frame(
    jpeg_dec_handle_t decoder,
    const uint8_t *data,
    uint32_t size,
    uint16_t expected_width,
    uint16_t expected_height,
    FrameSlot *slot,
    uint32_t *out_decode_us)
{
    if (decoder == nullptr || slot == nullptr || slot->rgb565_be == nullptr ||
        data == nullptr || size == 0U || out_decode_us == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t info = {};
    io.inbuf = const_cast<uint8_t *>(data);
    io.inbuf_len = static_cast<int>(size);

    const int64_t started_us = esp_timer_get_time();
    jpeg_error_t jpeg_ret = jpeg_dec_parse_header(decoder, &io, &info);
    if (jpeg_ret != JPEG_ERR_OK ||
        info.width != expected_width || info.height != expected_height ||
        info.width == 0U || info.height == 0U ||
        info.width > kWidth || info.height > kHeight) {
        *out_decode_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
        return jpeg_ret == JPEG_ERR_OK ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_RESPONSE;
    }

    int required = 0;
    const size_t expected_rgb565_bytes =
        static_cast<size_t>(info.width) * static_cast<size_t>(info.height) * 2U;
    if (jpeg_dec_get_outbuf_len(decoder, &required) != JPEG_ERR_OK || required <= 0 ||
        static_cast<size_t>(required) != expected_rgb565_bytes ||
        expected_rgb565_bytes > kRgb565Bytes) {
        *out_decode_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
        return ESP_ERR_INVALID_SIZE;
    }

    io.outbuf = slot->rgb565_be;
    io.out_size = static_cast<int>(kRgb565Bytes);
    jpeg_ret = jpeg_dec_process(decoder, &io);
    *out_decode_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
    if (jpeg_ret != JPEG_ERR_OK) return ESP_ERR_INVALID_RESPONSE;

    slot->width = static_cast<uint16_t>(info.width);
    slot->height = static_cast<uint16_t>(info.height);
    slot->rgb565_bytes = static_cast<size_t>(required);
    return ESP_OK;
}


static void extract_task(void *arg)
{
    ExtractTaskArgs *args = static_cast<ExtractTaskArgs *>(arg);
    if (args == nullptr || args->io == nullptr) {
        if (args != nullptr) heap_caps_free(args);
        portENTER_CRITICAL(&g_mux);
        g_extract_task = nullptr;
        portEXIT_CRITICAL(&g_mux);
        delete_video_worker_task_self();
        return;
    }

    ESP_LOGI(TAG, "VideoExtractTask启动：generation=%lu",
        static_cast<unsigned long>(args->generation));
    static uint32_t s_extract_debug_frames = 0U;  // 临时诊断：打印开头若干提取帧
    uint32_t audio_chunks_pushed = 0U;
    uint64_t audio_bytes_pushed = 0ULL;
    uint64_t audio_push_us_total = 0ULL;
    uint32_t audio_push_us_max = 0U;
    uint32_t video_slot_wait_count = 0U;
    uint64_t video_slot_wait_us_total = 0ULL;
    uint32_t video_slot_wait_us_max = 0U;

    // 水位观测：音频桥剩余毫秒数 + 视频压缩队列剩余槽位。
    // 用于判断是否真的存在音频饿死，再决定是否需要"紧急越过视频 Chunk 补音频"。
    constexpr int64_t kWatermarkWindowUs = 10 * 1000000LL;
    int64_t watermark_window_started_us = esp_timer_get_time();
    uint32_t watermark_samples = 0U;
    uint32_t watermark_bridge_ms_min = UINT32_MAX;
    uint64_t watermark_bridge_ms_sum = 0ULL;
    uint32_t watermark_bridge_under_600 = 0U;
    uint32_t watermark_bridge_under_300 = 0U;
    uint8_t watermark_slot_free_min = 0xFFU;
    uint32_t run_bridge_ms_min = UINT32_MAX;
    uint64_t run_bridge_ms_sum = 0ULL;
    uint32_t run_watermark_samples = 0U;

    const auto sample_pipeline_watermarks = [&]() {
        const uint8_t free_slots = g_compressed_free_queue != nullptr
            ? static_cast<uint8_t>(uxQueueMessagesWaiting(g_compressed_free_queue))
            : 0U;
        if (free_slots < watermark_slot_free_min) watermark_slot_free_min = free_slots;

        const int64_t now_us = esp_timer_get_time();
        const bool window_due = now_us - watermark_window_started_us >= kWatermarkWindowUs;

        AviMp3BridgeSnapshot bridge = {};
        if (!avi_mp3_bridge_get_snapshot(&bridge) || bridge.capacity_bytes == 0U) {
            if (window_due) {
                ESP_LOGI(TAG,
                    "AVI水位观测(%ds)：无音频桥 capacity=0 slots_free_min=%u",
                    10, static_cast<unsigned>(watermark_slot_free_min));
                watermark_window_started_us = now_us;
                watermark_slot_free_min = 0xFFU;
            }
            return;
        }

        uint32_t audio_bitrate = 0U;
        portENTER_CRITICAL(&g_mux);
        audio_bitrate = g_snapshot.audio_bitrate;
        portEXIT_CRITICAL(&g_mux);
        const uint32_t audio_avg_bps = audio_bitrate / 8U;
        if (audio_avg_bps == 0U) {
            if (window_due) {
                ESP_LOGI(TAG,
                    "AVI水位观测(%ds)：码率未知 audio_bitrate=0 slots_free_min=%u",
                    10, static_cast<unsigned>(watermark_slot_free_min));
                watermark_window_started_us = now_us;
                watermark_slot_free_min = 0xFFU;
            }
            return;
        }

        const uint32_t buffered_ms = static_cast<uint32_t>(
            static_cast<uint64_t>(bridge.buffered_bytes) * 1000ULL / audio_avg_bps);
        ++watermark_samples;
        watermark_bridge_ms_sum += buffered_ms;
        if (buffered_ms < watermark_bridge_ms_min) watermark_bridge_ms_min = buffered_ms;
        if (buffered_ms < run_bridge_ms_min) run_bridge_ms_min = buffered_ms;
        run_bridge_ms_sum += buffered_ms;
        ++run_watermark_samples;
        if (buffered_ms < 600U) ++watermark_bridge_under_600;
        if (buffered_ms < 300U) ++watermark_bridge_under_300;

        if (!window_due) return;

        ESP_LOGI(TAG,
            "AVI水位观测(%ds)：bridge_min=%lums bridge_avg=%lums <600ms=%lu <300ms=%lu "
            "bridge_push_wait=%lu slots_free_min=%u samples=%lu",
            10,
            static_cast<unsigned long>(
                watermark_bridge_ms_min != UINT32_MAX ? watermark_bridge_ms_min : 0U),
            static_cast<unsigned long>(watermark_samples != 0U
                ? watermark_bridge_ms_sum / watermark_samples : 0ULL),
            static_cast<unsigned long>(watermark_bridge_under_600),
            static_cast<unsigned long>(watermark_bridge_under_300),
            static_cast<unsigned long>(bridge.push_wait_count),
            static_cast<unsigned>(watermark_slot_free_min),
            static_cast<unsigned long>(watermark_samples));

        watermark_window_started_us = now_us;
        watermark_samples = 0U;
        watermark_bridge_ms_min = UINT32_MAX;
        watermark_bridge_ms_sum = 0ULL;
        watermark_bridge_under_600 = 0U;
        watermark_bridge_under_300 = 0U;
        watermark_slot_free_min = 0xFFU;
    };

    uint8_t compressed_slot_index = 0xFFU;
    bool have_video_slot = false;

    while (generation_current(args->generation)) {
        if (!extract_pause_gate(args->generation)) break;

        // 有空闲槽位时先非阻塞借用；若池已满，仍允许 Demuxer 直接读取音频 chunk。
        // 只有确认当前 chunk 是视频帧且无槽位时，才等待并重试同一帧，绝不主动丢帧。
        if (!have_video_slot) {
            have_video_slot = try_take_compressed_free(&compressed_slot_index);
        }
        uint8_t *video_buffer = have_video_slot
            ? g_compressed_slots[compressed_slot_index].data : nullptr;
        uint8_t *audio_buffer = g_compressed_slots[kAudioScratchSlotIndex].data;

        AviDemuxer::FrameInfo frame = {};
        const uint64_t storage_before = args->io->read_us_total;
        const int64_t extract_started_us = esp_timer_get_time();
        const esp_err_t ex = AviDemuxer::read_frame(
            &frame,
            video_buffer, have_video_slot ? kCompressedSlotBytes : 0U,
            audio_buffer, kCompressedSlotBytes);
        const uint32_t extract_us = static_cast<uint32_t>(esp_timer_get_time() - extract_started_us);
        const uint32_t storage_us = static_cast<uint32_t>(args->io->read_us_total - storage_before);

        if (ex == AviDemuxer::kErrNeedVideoSlot) {
            if (have_video_slot) {
                ESP_LOGE(TAG, "已有视频槽位但 Demuxer 仍请求槽位，状态异常");
                return_compressed_to_free(compressed_slot_index);
                have_video_slot = false;
                avi_mp3_bridge_cancel();
                PipelineMessage message = {};
                message.type = PipelineMessageType::Failed;
                message.result = ESP_ERR_INVALID_STATE;
                (void)send_pipeline_message(message, args->generation);
                break;
            }

            const int64_t wait_started_us = esp_timer_get_time();
            bool slot_was_waited = false;
            if (!take_compressed_free_for_extract(
                    &compressed_slot_index, args->generation, &slot_was_waited)) {
                break;
            }
            have_video_slot = true;
            if (slot_was_waited) {
                ++video_slot_wait_count;
                const int64_t wait_elapsed = esp_timer_get_time() - wait_started_us;
                const uint32_t wait_us = wait_elapsed > 0LL
                    ? static_cast<uint32_t>(wait_elapsed) : 0U;
                video_slot_wait_us_total += wait_us;
                if (wait_us > video_slot_wait_us_max) video_slot_wait_us_max = wait_us;
            }
            continue;
        }

        if (ex == AviDemuxer::kErrEof) {
            if (have_video_slot) {
                return_compressed_to_free(compressed_slot_index);
                have_video_slot = false;
            }
            avi_mp3_bridge_mark_eof();
            PipelineMessage message = {};
            message.type = PipelineMessageType::Eof;
            message.result = ESP_OK;
            (void)send_pipeline_message(message, args->generation);
            break;
        }

        if (ex != ESP_OK) {
            if (have_video_slot) {
                return_compressed_to_free(compressed_slot_index);
                have_video_slot = false;
            }
            avi_mp3_bridge_cancel();
            PipelineMessage message = {};
            if (!generation_current(args->generation)) {
                message.type = PipelineMessageType::Stopped;
                message.result = ESP_OK;
            } else {
                message.type = PipelineMessageType::Failed;
                message.result = ex;
            }
            (void)send_pipeline_message(message, args->generation);
            break;
        }

        if (s_extract_debug_frames < 8U) {
            ++s_extract_debug_frames;
            ESP_LOGI(TAG, "提取#%lu %s size=%lu pts=%llums extract=%luus",
                static_cast<unsigned long>(s_extract_debug_frames),
                frame.is_video ? "video" : "audio",
                static_cast<unsigned long>(frame.size),
                static_cast<unsigned long long>(frame.pts),
                static_cast<unsigned long>(extract_us));
        }

        if (!frame.is_video) {
            // 音频 chunk 不持有视频槽位；若 SEEK 后状态变化导致暂借了槽位，先立即归还。
            if (have_video_slot) {
                return_compressed_to_free(compressed_slot_index);
                have_video_slot = false;
            }
            esp_err_t audio_push_ret = ESP_OK;
            if (frame.data != nullptr && frame.size != 0U) {
                (void)record_audio_probe(frame, extract_us, storage_us, args->generation);
                const int64_t push_started_us = esp_timer_get_time();
                audio_push_ret = avi_mp3_bridge_push(
                    frame.data, frame.size, kAudioBridgePushWaitForever);
                const int64_t push_elapsed_signed = esp_timer_get_time() - push_started_us;
                const uint32_t push_elapsed_us = push_elapsed_signed > 0LL
                    ? static_cast<uint32_t>(push_elapsed_signed) : 0U;
                ++audio_chunks_pushed;
                audio_bytes_pushed += frame.size;
                audio_push_us_total += push_elapsed_us;
                if (push_elapsed_us > audio_push_us_max) audio_push_us_max = push_elapsed_us;
            }
            if (audio_push_ret != ESP_OK) {
                const bool generation_alive = generation_current(args->generation);
                PipelineMessage message = {};
                message.type = generation_alive
                    ? PipelineMessageType::Failed : PipelineMessageType::Stopped;
                message.result = generation_alive ? audio_push_ret : ESP_OK;
                if (generation_alive) {
                    ESP_LOGE(TAG, "AVI MP3 Bridge写入失败：ret=%s pts=%llums bytes=%lu",
                        esp_err_to_name(audio_push_ret),
                        static_cast<unsigned long long>(frame.pts),
                        static_cast<unsigned long>(frame.size));
                }
                (void)send_pipeline_message(message, args->generation);
                break;
            }
            sample_pipeline_watermarks();
            continue;
        }

        if (!have_video_slot || compressed_slot_index >= kCompressedVideoSlotCount) {
            ESP_LOGE(TAG, "Demuxer返回视频帧但没有有效压缩槽位");
            avi_mp3_bridge_cancel();
            PipelineMessage message = {};
            message.type = PipelineMessageType::Failed;
            message.result = ESP_ERR_INVALID_STATE;
            (void)send_pipeline_message(message, args->generation);
            break;
        }

        CompressedSlot &slot = g_compressed_slots[compressed_slot_index];
        slot.bytes = frame.size;
        slot.pts_ms = static_cast<uint32_t>(frame.pts);
        slot.seek_epoch = seek_epoch();
        slot.extract_us = extract_us;
        slot.storage_us = storage_us;
        slot.copy_us = 0U;

        PipelineMessage message = {};
        message.type = PipelineMessageType::Frame;
        message.slot = compressed_slot_index;
        message.result = ESP_OK;
        if (!send_pipeline_message(message, args->generation)) {
            return_compressed_to_free(compressed_slot_index);
            have_video_slot = false;
            break;
        }
        // 成功发送后槽位所有权转交给 DecodeTask；暂停期间的发送路径也会自行归还槽位。
        have_video_slot = false;
        sample_pipeline_watermarks();
    }

    if (have_video_slot) return_compressed_to_free(compressed_slot_index);

    ESP_LOGI(TAG,
        "Extractor供数汇总：audio_chunks=%lu audio_bytes=%llu push_avg=%luus push_max=%luus video_slot_waits=%lu wait_avg=%lluus wait_max=%luus "
        "bridge_ms_min=%lums bridge_ms_avg=%lums",
        static_cast<unsigned long>(audio_chunks_pushed),
        static_cast<unsigned long long>(audio_bytes_pushed),
        static_cast<unsigned long>(audio_chunks_pushed != 0U
            ? audio_push_us_total / audio_chunks_pushed : 0ULL),
        static_cast<unsigned long>(audio_push_us_max),
        static_cast<unsigned long>(video_slot_wait_count),
        static_cast<unsigned long long>(video_slot_wait_count != 0U
            ? video_slot_wait_us_total / video_slot_wait_count : 0ULL),
        static_cast<unsigned long>(video_slot_wait_us_max),
        static_cast<unsigned long>(run_bridge_ms_min != UINT32_MAX ? run_bridge_ms_min : 0U),
        static_cast<unsigned long>(run_watermark_samples != 0U
            ? run_bridge_ms_sum / run_watermark_samples : 0ULL));

    heap_caps_free(args);
    portENTER_CRITICAL(&g_mux);
    g_extract_task = nullptr;
    portEXIT_CRITICAL(&g_mux);
    delete_video_worker_task_self();
}

static PresentationClock presentation_clock_snapshot(uint32_t generation)
{
    PresentationClock clock = {};
    portENTER_CRITICAL(&g_mux);
    if (g_presentation_clock.started && g_presentation_clock.generation == generation) {
        clock = g_presentation_clock;
    }
    portEXIT_CRITICAL(&g_mux);
    return clock;
}

static uint32_t ewma_step(uint32_t current, uint32_t sample)
{
    if (current == 0U) return sample;
    const int64_t delta = static_cast<int64_t>(sample) - static_cast<int64_t>(current);
    const int64_t next = static_cast<int64_t>(current) +
        delta / static_cast<int64_t>(1U << kDecodeTimingEwmaShift);
    return next <= 0LL ? 1U : static_cast<uint32_t>(next);
}

static void decode_timing_update(DecodeTimingModel *timing, uint32_t decode_us)
{
    if (timing == nullptr || decode_us == 0U) return;
    if (timing->ewma_us == 0U) {
        timing->ewma_us = decode_us;
        timing->jitter_ewma_us = 0U;
        return;
    }
    const uint32_t previous = timing->ewma_us;
    const uint32_t deviation = decode_us >= previous ? decode_us - previous : previous - decode_us;
    timing->ewma_us = ewma_step(previous, decode_us);
    timing->jitter_ewma_us = ewma_step(timing->jitter_ewma_us, deviation);
}

static uint32_t decode_estimate_us(const DecodeTimingModel &timing)
{
    if (timing.ewma_us == 0U) return 0U;
    const uint32_t reserve = timing.jitter_ewma_us > kDecodeJitterReserveMaxUs
        ? kDecodeJitterReserveMaxUs : timing.jitter_ewma_us;
    const uint64_t estimated = static_cast<uint64_t>(timing.ewma_us) + reserve;
    return estimated > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(estimated);
}

static int64_t adaptive_predecode_deadline_us(const DecodeTimingModel &timing)
{
    uint32_t safety_us = kDeadlineSafetyBaseUs + timing.jitter_ewma_us / 2U;
    if (safety_us > kDeadlineSafetyMaxUs) safety_us = kDeadlineSafetyMaxUs;
    return kUiEmergencyLateUs - static_cast<int64_t>(safety_us);
}

static bool audio_master_late_us(uint32_t video_pts_ms, int64_t *out_late_us)
{
    if (out_late_us != nullptr) *out_late_us = 0LL;

    bool have_audio_pts = false;
    portENTER_CRITICAL(&g_mux);
    have_audio_pts = g_audio_probe.frames_read != 0U;
    portEXIT_CRITICAL(&g_mux);
    if (!have_audio_pts) return false;

    AudioVideoClockSnapshot audio_clock = {};
    if (!audio_service_video_mp3_get_clock(&audio_clock) ||
        !audio_clock.active || audio_clock.eof || audio_clock.sample_rate_hz == 0U ||
        audio_clock.submitted_frames == 0ULL) {
        return false;
    }

    // 音频 PCM 时钟零点即视频时间轴零点（seek 时已对齐），不能再减音频第一帧 PTS：
    // seek 后探针会记录新的第一帧 PTS，用它做基准会把目标整体拉回 0，导致全丢帧。
    const int64_t target_audio_us = static_cast<int64_t>(video_pts_ms) * 1000LL;
    const int64_t late_us = static_cast<int64_t>(audio_clock.position_us) - target_audio_us;
    if (out_late_us != nullptr) *out_late_us = late_us;
    return true;
}

static bool should_predecode_drop(
    const PresentationClock &clock,
    uint32_t pts_ms,
    uint32_t decode_estimate,
    int64_t deadline_us,
    int64_t *out_late_us,
    int64_t *out_projected_late_us,
    bool *out_audio_master)
{
    if (out_late_us != nullptr) *out_late_us = 0LL;
    if (out_projected_late_us != nullptr) *out_projected_late_us = 0LL;
    if (out_audio_master != nullptr) *out_audio_master = false;
    if (decode_estimate == 0U) return false;

    int64_t late_us = 0LL;
    if (audio_master_late_us(pts_ms, &late_us)) {
        if (out_audio_master != nullptr) *out_audio_master = true;
    } else {
        if (!clock.started) return false;
        const uint32_t pts_delta_ms =
            pts_ms >= clock.first_pts_ms ? pts_ms - clock.first_pts_ms : 0U;
        const int64_t target_us =
            clock.base_us + static_cast<int64_t>(pts_delta_ms) * 1000LL;
        late_us = esp_timer_get_time() - target_us;
    }

    const int64_t projected_late_us = late_us + static_cast<int64_t>(decode_estimate);
    if (out_late_us != nullptr) *out_late_us = late_us;
    if (out_projected_late_us != nullptr) *out_projected_late_us = projected_late_us;
    return projected_late_us > deadline_us;
}

static void benchmark_task(void *arg)
{
    TaskArgs *args = static_cast<TaskArgs *>(arg);
    if (args == nullptr || args->path == nullptr) {
        if (args != nullptr) heap_caps_free(args);
        portENTER_CRITICAL(&g_mux);
        g_task = nullptr;
        portEXIT_CRITICAL(&g_mux);
        delete_video_worker_task_self();
        return;
    }

    Snapshot result = {};
    result.state = State::Failed;
    result.generation = args->generation;
    result.result = ESP_FAIL;

    VideoMediaIo::Context io = {};
    VideoMediaIo::configure(&io, args->generation, generation_current, args->music_suspended);
    jpeg_dec_handle_t decoder = nullptr;
    uint32_t source_fps_x100 = 0U;
    // AVI 元信息需要同时供 Demuxer 初始化和后续 FVIX V2 完整性校验使用。
    AviDemuxer::StreamInfo avi_info = {};

    esp_err_t ret = VideoMediaIo::open(args->path, &io);
    if (ret == ESP_OK && generation_current(args->generation)) {
        ret = AviDemuxer::open(&io, &avi_info);
        if (ret == ESP_OK) {
            source_fps_x100 = avi_info.fps_x100;
            ret = validate_demuxer_stream(avi_info, &result, args->allow_fullcanvas_over20);
        }
    }

    // 加载侧车索引（用于 seek）：仅将 .avi 扩展名替换为 .fvix，不兼容旧的 .avi.fvix。
    if (ret == ESP_OK && generation_current(args->generation)) {
        char sidecar_path[512] = {};
        if (build_sidecar_path(args->path, sidecar_path, sizeof(sidecar_path))) {
            const esp_err_t idx_ret = VideoSeekIndex::load(
                sidecar_path, &io, source_fps_x100,
                avi_info.total_frames, avi_info.duration_ms);
            if (idx_ret == ESP_OK) {
                ESP_LOGI(TAG, "侧车索引已加载，%u 条目，来源抽样校验=%s，音频字节锚点=%s",
                    VideoSeekIndex::count(),
                    VideoSeekIndex::has_source_fingerprint() ? "通过" : "旧版未验证",
                    VideoSeekIndex::has_audio_byte_anchor() ? "是" : "否");
            } else {
                ESP_LOGW(TAG, "侧车索引不可用（缺失、版本不支持或来源校验失败），seek 不可用: %s",
                    sidecar_path);
            }
        } else {
            ESP_LOGW(TAG, "无法根据 AVI 路径生成 .fvix 索引路径: %s", args->path);
        }
    }

    if (ret == ESP_OK && generation_current(args->generation)) {
        jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
        // CO5300 wire-order 直接输出，BoundedSPI present(... wire_order=true) 不再做整帧 byte-swap。
        config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
        config.rotate = JPEG_ROTATE_0D;
        // Full-image native-size decode；显示端仅做局部居中，不做运行时 resize。
        config.block_enable = false;
        if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || decoder == nullptr) ret = ESP_FAIL;
    }

    if (ret == ESP_OK && generation_current(args->generation) && result.audio_streams != 0U) {
        ret = avi_mp3_bridge_begin(kAudioBridgeBytes);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "创建AVI MP3 32KB PSRAM Bridge失败：%s", esp_err_to_name(ret));
        }
    }

    uint64_t copy_us_total = 0ULL;
    uint32_t copy_us_max = 0U;
    uint32_t pipe_qmax = 0U;
    DecodeTimingModel decode_timing = {};
    uint32_t predecode_drops = 0U;
    uint32_t consecutive_decode_failures = 0U;
    uint32_t catchup_events = 0U;
    bool catchup_active = false;
    int64_t projected_late_us_max = 0LL;

    if (ret == ESP_OK && generation_current(args->generation)) {
        ExtractTaskArgs *extract_args = static_cast<ExtractTaskArgs *>(
            heap_caps_calloc(1U, sizeof(ExtractTaskArgs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (extract_args == nullptr) {
            ret = ESP_ERR_NO_MEM;
        } else {
            extract_args->io = &io;
            extract_args->generation = args->generation;
            const BaseType_t created = create_video_worker_task(
                extract_task, "VideoExtractTask", kExtractTaskStack, extract_args,
                kExtractTaskPriority, &g_extract_task, kExtractTaskCore);
            if (created != pdPASS) {
                heap_caps_free(extract_args);
                ret = ESP_ERR_NO_MEM;
            }
        }
    }

    if (ret == ESP_OK && generation_current(args->generation) && result.audio_streams != 0U) {
        AviMp3BridgeSnapshot prebuffer = {};
        uint32_t prebuffer_wait_ms = 0U;
        ret = wait_audio_prebuffer(args->generation, &prebuffer, &prebuffer_wait_ms);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG,
                "AVI MP3预取失败：need=%uB buffered=%uB high=%uB eof=%d wait=%lums ret=%s；拒绝带欠载风险启动AudioTask",
                static_cast<unsigned>(kAudioPrebufferBytes),
                static_cast<unsigned>(prebuffer.buffered_bytes),
                static_cast<unsigned>(prebuffer.high_water_bytes),
                prebuffer.eof ? 1 : 0,
                static_cast<unsigned long>(prebuffer_wait_ms),
                esp_err_to_name(ret));
            avi_mp3_bridge_cancel();
            stop_generation(args->generation);
        } else {
            ESP_LOGI(TAG, "视频音频预取完成：%uB，等待%lums",
                static_cast<unsigned>(prebuffer.buffered_bytes),
                static_cast<unsigned long>(prebuffer_wait_ms));
        }
    }

    if (ret == ESP_OK && generation_current(args->generation) && result.audio_streams != 0U) {
        const bool audio_prepared = audio_service_video_mp3_prepare(
            result.audio_sample_rate,
            result.audio_channels,
            result.audio_bits_per_sample,
            true);
        if (!audio_prepared) {
            ESP_LOGE(TAG, "AVI MP3 AudioTask预备失败；取消本次Video，避免无声状态继续占用Bridge");
            avi_mp3_bridge_cancel();
            stop_generation(args->generation);
            ret = ESP_FAIL;
        }
    }

    if (ret == ESP_OK && generation_current(args->generation)) {
        result.state = State::Running;
        result.result = ESP_OK;
        publish(result);
        ESP_LOGI(TAG, "视频解码启动：%ux%u %u帧/秒 音轨=%s",
            static_cast<unsigned>(result.width),
            static_cast<unsigned>(result.height),
            static_cast<unsigned>(result.fps_hint),
            result.audio_streams != 0U ? "MP3" : "无");

        bool done = false;
        while (!done && generation_current(args->generation)) {
            if (!wait_while_paused(args->generation)) break;
            PipelineMessage message = {};
            if (!receive_pipeline_message(&message, args->generation)) break;

            if (message.type == PipelineMessageType::PoolSkipped) {
                ++result.frames_pool_skipped;
                if ((result.frames_pool_skipped & 0x0FU) == 1U) {
                    ESP_LOGW(TAG, "Pipeline跳帧：count=%lu slot=%uKB",
                        static_cast<unsigned long>(result.frames_pool_skipped),
                        static_cast<unsigned>(kCompressedSlotBytes / 1024U));
                }
                continue;
            }
            if (message.type == PipelineMessageType::Eof) {
                result.state = State::Eof;
                result.result = ESP_OK;
                done = true;
                break;
            }
            if (message.type == PipelineMessageType::Stopped) {
                result.state = State::Stopped;
                result.result = ESP_OK;
                done = true;
                break;
            }
            if (message.type == PipelineMessageType::Failed) {
                result.state = State::Failed;
                result.result = message.result;
                ret = message.result;
                done = true;
                break;
            }
            if (message.type != PipelineMessageType::Frame || message.slot >= kCompressedVideoSlotCount) {
                result.state = State::Failed;
                result.result = ESP_ERR_INVALID_STATE;
                ret = ESP_ERR_INVALID_STATE;
                done = true;
                break;
            }

            const UBaseType_t queued_after_receive =
                g_compressed_ready_queue != nullptr ? uxQueueMessagesWaiting(g_compressed_ready_queue) : 0U;
            const uint32_t depth = static_cast<uint32_t>(queued_after_receive) + 1U;
            if (depth > pipe_qmax) pipe_qmax = depth;

            CompressedSlot &compressed = g_compressed_slots[message.slot];
            const uint32_t compressed_seek_epoch = compressed.seek_epoch;
            // 以 ExtractTask 给该帧盖的世代戳为准，不能把“等待队列期间发生 SEEK”误判成旧帧。
            if (compressed_seek_epoch != seek_epoch()) {
                return_compressed_to_free(message.slot);
                continue;
            }
            ++result.frames_read;
            result.extract_us_total += compressed.extract_us;
            if (compressed.extract_us > result.extract_us_max) result.extract_us_max = compressed.extract_us;
            result.storage_us_total += compressed.storage_us;
            if (compressed.storage_us > result.storage_us_max) result.storage_us_max = compressed.storage_us;
            copy_us_total += compressed.copy_us;
            if (compressed.copy_us > copy_us_max) copy_us_max = compressed.copy_us;

            int64_t late_us = 0LL;
            int64_t projected_late_us = 0LL;
            bool predecode_audio_master = false;
            PresentationClock clock = presentation_clock_snapshot(args->generation);
            uint32_t estimated_decode_us = decode_estimate_us(decode_timing);
            int64_t deadline_us = adaptive_predecode_deadline_us(decode_timing);
            if (should_predecode_drop(
                    clock, compressed.pts_ms, estimated_decode_us, deadline_us,
                    &late_us, &projected_late_us, &predecode_audio_master)) {
                ++predecode_drops;
                if (!catchup_active) {
                    ++catchup_events;
                    catchup_active = true;
                }
                if (projected_late_us > projected_late_us_max) projected_late_us_max = projected_late_us;
                return_compressed_to_free(message.slot);
                if ((result.frames_read % kPublishEveryFrames) == 0U) publish(result);
                continue;
            }

            uint8_t slot_index = 0U;
            if (!take_free(&slot_index, args->generation)) {
                return_compressed_to_free(message.slot);
                break;
            }

            // 等 RGB565 空槽也可能耗时；真正进入 JPEG 前用同一 Master Clock 再检查一次。
            clock = presentation_clock_snapshot(args->generation);
            estimated_decode_us = decode_estimate_us(decode_timing);
            deadline_us = adaptive_predecode_deadline_us(decode_timing);
            if (should_predecode_drop(
                    clock, compressed.pts_ms, estimated_decode_us, deadline_us,
                    &late_us, &projected_late_us, &predecode_audio_master)) {
                ++predecode_drops;
                if (!catchup_active) {
                    ++catchup_events;
                    catchup_active = true;
                }
                if (projected_late_us > projected_late_us_max) projected_late_us_max = projected_late_us;
                return_slot_to_free(slot_index);
                return_compressed_to_free(message.slot);
                if ((result.frames_read % kPublishEveryFrames) == 0U) publish(result);
                continue;
            }

            if (!wait_while_paused(args->generation)) {
                return_slot_to_free(slot_index);
                return_compressed_to_free(message.slot);
                break;
            }

            FrameSlot &slot = g_slots[slot_index];
            uint32_t decode_us = 0U;
            ret = decode_frame(
                decoder, compressed.data, compressed.bytes,
                result.width, result.height, &slot, &decode_us);
            const uint32_t pts_ms = compressed.pts_ms;
            const uint32_t compressed_bytes = compressed.bytes;
            const uint32_t extract_us = compressed.extract_us;
            const uint32_t storage_us = compressed.storage_us;
            return_compressed_to_free(message.slot);

            if (ret != ESP_OK) {
                ++result.decode_failures;
                ++consecutive_decode_failures;
                return_slot_to_free(slot_index);
                // 单帧损坏（例如 seek 落点附近）不应终止整段播放：跳过并继续，
                // 连续多帧都失败才判定管线异常。
                if (consecutive_decode_failures >= 8U) {
                    result.state = State::Failed;
                    result.result = ret;
                    ESP_LOGE(TAG, "MJPEG连续解码失败：frame=%lu pts=%lums bytes=%lu ret=%s",
                        static_cast<unsigned long>(result.frames_read),
                        static_cast<unsigned long>(pts_ms),
                        static_cast<unsigned long>(compressed_bytes), esp_err_to_name(ret));
                    stop_generation(args->generation);
                    done = true;
                    break;
                }
                ESP_LOGW(TAG, "MJPEG解码失败已跳过：frame=%lu pts=%lums bytes=%lu ret=%s",
                    static_cast<unsigned long>(result.frames_read),
                    static_cast<unsigned long>(pts_ms),
                    static_cast<unsigned long>(compressed_bytes), esp_err_to_name(ret));
                continue;
            }
            consecutive_decode_failures = 0U;

            // JPEG 解码期间可能发生 SEEK；旧帧即使解码成功，也不能进入新时间轴。
            if (compressed_seek_epoch != seek_epoch()) {
                return_slot_to_free(slot_index);
                continue;
            }

            slot.pts_ms = pts_ms;
            slot.seek_epoch = compressed_seek_epoch;
            slot.compressed_bytes = compressed_bytes;
            slot.extract_us = extract_us;
            slot.storage_us = storage_us;
            slot.decode_us = decode_us;

            ++result.frames_decoded;
            result.compressed_bytes += compressed_bytes;
            if (compressed_bytes > result.compressed_bytes_max) result.compressed_bytes_max = compressed_bytes;
            result.decode_us_total += decode_us;
            if (decode_us > result.decode_us_max) result.decode_us_max = decode_us;
            decode_timing_update(&decode_timing, decode_us);
            if (result.frames_decoded == 1U) result.first_pts_ms = pts_ms;
            result.last_pts_ms = pts_ms;
            result.storage_gate_wait_us = io.gate_wait_us_total;
            result.storage_gate_wait_count = io.gate_wait_count;

            if (!send_ready(slot_index, args->generation, compressed_seek_epoch)) {
                return_slot_to_free(slot_index);
                break;
            }
            catchup_active = false;
            if ((result.frames_decoded % kPublishEveryFrames) == 0U) publish(result);
            if ((result.frames_decoded % kDecodeYieldEveryFrames) == 0U) {
                vTaskDelay(kDecodeYieldTicks);
            }
        }
    }

    if (!generation_current(args->generation) && result.state != State::Failed) {
        result.state = State::Stopped;
        result.result = ESP_OK;
    } else if (ret != ESP_OK && result.state != State::Running && result.state != State::Eof) {
        result.state = State::Failed;
        result.result = ret;
    }

    result.storage_gate_wait_us = io.gate_wait_us_total;
    result.storage_gate_wait_count = io.gate_wait_count;

    // 非正常 EOF 先取消 AudioSource，立即唤醒可能正在等压缩数据的 AudioTask。
    if (result.state != State::Eof) avi_mp3_bridge_cancel();

    // 任何退出路径都先确保 ExtractTask 不再访问 extractor/io，再释放底层资源。
    if (extract_task_running()) {
        if (result.state == State::Failed || !generation_current(args->generation)) {
            stop_generation(args->generation);
        }
        wait_extract_task_exit();
    }

    if (decoder != nullptr) jpeg_dec_close(decoder);
    AviDemuxer::IoStats demux_io_stats = {};
    AviDemuxer::get_io_stats(&demux_io_stats);
    const uint32_t demux_seek_avg_us = demux_io_stats.fseek_calls != 0U
        ? static_cast<uint32_t>(demux_io_stats.fseek_us_total / demux_io_stats.fseek_calls)
        : 0U;
    ESP_LOGI(TAG,
        "AVI顺序IO统计：fseek=%lu seq_skip=%lu seek_avg=%luus seek_max=%luus forward_reads=%lu forward_bytes=%llu audio_chunks=%lu audio_bytes=%llu seek_scan=%llums scan_chunks=%lu scan_bytes=%llu scan_fseek=%lu",
        static_cast<unsigned long>(demux_io_stats.fseek_calls),
        static_cast<unsigned long>(demux_io_stats.sequential_seek_skips),
        static_cast<unsigned long>(demux_seek_avg_us),
        static_cast<unsigned long>(demux_io_stats.fseek_us_max),
        static_cast<unsigned long>(demux_io_stats.forward_reads),
        static_cast<unsigned long long>(demux_io_stats.forward_bytes),
        static_cast<unsigned long>(demux_io_stats.audio_chunks),
        static_cast<unsigned long long>(demux_io_stats.audio_bytes),
        static_cast<unsigned long long>(demux_io_stats.seek_scan_duration_us / 1000ULL),
        static_cast<unsigned long>(demux_io_stats.seek_scan_chunks),
        static_cast<unsigned long long>(demux_io_stats.seek_scan_skipped_bytes),
        static_cast<unsigned long>(demux_io_stats.seek_scan_fseek_calls));
    AviDemuxer::close();
    VideoSeekIndex::unload();
    VideoMediaIo::close(&io);

    snapshot_audio_probe(&result);

    if (generation_matches(args->generation)) {
        const uint32_t decode_avg_us = result.frames_decoded != 0U
            ? static_cast<uint32_t>(result.decode_us_total / result.frames_decoded) : 0U;
        const uint32_t extract_avg_us = result.frames_read != 0U
            ? static_cast<uint32_t>(result.extract_us_total / result.frames_read) : 0U;
        const uint32_t storage_avg_us = result.frames_read != 0U
            ? static_cast<uint32_t>(result.storage_us_total / result.frames_read) : 0U;
        const uint32_t copy_avg_us = result.frames_read != 0U
            ? static_cast<uint32_t>(copy_us_total / result.frames_read) : 0U;
        const uint32_t compressed_avg_bytes = result.frames_decoded != 0U
            ? static_cast<uint32_t>(result.compressed_bytes / result.frames_decoded) : 0U;
        ESP_LOGI(TAG,
            "视频解码结束：状态=%s 读取=%lu 解码=%lu 预丢帧=%lu JPEG平均=%lu.%03lums 最大=%lu.%03lums 音频帧=%lu",
            state_name(result.state),
            static_cast<unsigned long>(result.frames_read),
            static_cast<unsigned long>(result.frames_decoded),
            static_cast<unsigned long>(predecode_drops),
            static_cast<unsigned long>(decode_avg_us / 1000U),
            static_cast<unsigned long>(decode_avg_us % 1000U),
            static_cast<unsigned long>(result.decode_us_max / 1000U),
            static_cast<unsigned long>(result.decode_us_max % 1000U),
            static_cast<unsigned long>(result.audio_frames_read));
        ESP_LOGI(TAG,
            "视频性能分解：提取=%lu.%03lu/%lu.%03lums SD=%lu.%03lu/%lu.%03lums PSRAM复制=%lu.%03lu/%lu.%03lums 压缩帧=%lu/%luB queue_max=%lu catchup=%lu projected_late_max=%lldus",
            static_cast<unsigned long>(extract_avg_us / 1000U),
            static_cast<unsigned long>(extract_avg_us % 1000U),
            static_cast<unsigned long>(result.extract_us_max / 1000U),
            static_cast<unsigned long>(result.extract_us_max % 1000U),
            static_cast<unsigned long>(storage_avg_us / 1000U),
            static_cast<unsigned long>(storage_avg_us % 1000U),
            static_cast<unsigned long>(result.storage_us_max / 1000U),
            static_cast<unsigned long>(result.storage_us_max % 1000U),
            static_cast<unsigned long>(copy_avg_us / 1000U),
            static_cast<unsigned long>(copy_avg_us % 1000U),
            static_cast<unsigned long>(copy_us_max / 1000U),
            static_cast<unsigned long>(copy_us_max % 1000U),
            static_cast<unsigned long>(compressed_avg_bytes),
            static_cast<unsigned long>(result.compressed_bytes_max),
            static_cast<unsigned long>(pipe_qmax),
            static_cast<unsigned long>(catchup_events),
            static_cast<long long>(projected_late_us_max));
    }

    publish_task_exit(result);
    heap_caps_free(args->path);
    heap_caps_free(args);
    delete_video_worker_task_self();
}

static void free_resources_unlocked()
{
    if (g_free_queue != nullptr) { vQueueDelete(g_free_queue); g_free_queue = nullptr; }
    if (g_ready_queue != nullptr) { vQueueDelete(g_ready_queue); g_ready_queue = nullptr; }
    if (g_compressed_free_queue != nullptr) {
        vQueueDelete(g_compressed_free_queue);
        g_compressed_free_queue = nullptr;
    }
    if (g_compressed_ready_queue != nullptr) {
        vQueueDelete(g_compressed_ready_queue);
        g_compressed_ready_queue = nullptr;
    }
    for (FrameSlot &slot : g_slots) {
        if (slot.rgb565_be != nullptr) heap_caps_free(slot.rgb565_be);
        slot = {};
    }
    for (CompressedSlot &slot : g_compressed_slots) {
        if (slot.data != nullptr) heap_caps_free(slot.data);
        slot = {};
    }
    g_leased_mask = 0U;
}

} // namespace

esp_err_t start(const char *path, bool allow_fullcanvas_over20, bool music_suspended)
{
    if (path == nullptr || path[0] == '\0') return ESP_ERR_INVALID_ARG;
    const esp_err_t clean_ret = cleanup();
    if (clean_ret != ESP_OK && clean_ret != ESP_ERR_INVALID_STATE) return clean_ret;

    portENTER_CRITICAL(&g_mux);
    const bool busy = g_task != nullptr || g_extract_task != nullptr;
    portEXIT_CRITICAL(&g_mux);
    if (busy) return ESP_ERR_INVALID_STATE;

    for (FrameSlot &slot : g_slots) {
        slot.rgb565_be = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16U, kRgb565Bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (slot.rgb565_be == nullptr) {
            free_resources_unlocked();
            return ESP_ERR_NO_MEM;
        }
    }
    for (CompressedSlot &slot : g_compressed_slots) {
        slot.data = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16U, kCompressedSlotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (slot.data == nullptr) {
            free_resources_unlocked();
            return ESP_ERR_NO_MEM;
        }
    }

    g_free_queue = xQueueCreate(kFrameSlotCount, sizeof(uint8_t));
    g_ready_queue = xQueueCreate(kFrameSlotCount, sizeof(uint8_t));
    g_compressed_free_queue = xQueueCreate(kCompressedVideoSlotCount, sizeof(uint8_t));
    g_compressed_ready_queue = xQueueCreate(kCompressedVideoSlotCount + 2U, sizeof(PipelineMessage));
    if (g_free_queue == nullptr || g_ready_queue == nullptr ||
        g_compressed_free_queue == nullptr || g_compressed_ready_queue == nullptr) {
        free_resources_unlocked();
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t i = 0U; i < kFrameSlotCount; ++i) (void)xQueueSend(g_free_queue, &i, 0);
    for (uint8_t i = 0U; i < kCompressedVideoSlotCount; ++i) {
        (void)xQueueSend(g_compressed_free_queue, &i, 0);
    }

    TaskArgs *args = static_cast<TaskArgs *>(heap_caps_calloc(1U, sizeof(TaskArgs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    char *path_copy = static_cast<char *>(heap_caps_calloc(512U, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (args == nullptr || path_copy == nullptr) {
        if (args != nullptr) heap_caps_free(args);
        if (path_copy != nullptr) heap_caps_free(path_copy);
        free_resources_unlocked();
        return ESP_ERR_NO_MEM;
    }
    snprintf(path_copy, 512U, "%s", path);

    portENTER_CRITICAL(&g_mux);
    ++g_generation;
    if (g_generation == 0U) ++g_generation;
    args->generation = g_generation;
    args->path = path_copy;
    args->allow_fullcanvas_over20 = allow_fullcanvas_over20;
    args->music_suspended = music_suspended;
    g_snapshot = {};
    g_snapshot.state = State::Starting;
    g_snapshot.generation = g_generation;
    g_seek_failure = ESP_OK;
    g_stop_generation = 0U;
    g_pause_requested = false;
    g_paused = false;
    g_extract_pause_ack = false;
    g_pause_started_us = 0LL;
    g_paused_seek_preview = false;
    g_seek_audio_resume_pending = false;
    g_extract_task = nullptr;
    g_leased_mask = 0U;
    g_presentation_clock = {};
    g_presentation_clock.generation = g_generation;
    g_audio_probe = {};
    portEXIT_CRITICAL(&g_mux);

    // 直接让 FreeRTOS 写入全局 handle，避免 task 极快退出后 caller 再把过期 handle 写回 g_task。
    ESP_LOGI(TAG, "Video worker栈：Decode=%uB PSRAM Extract=%uB PSRAM；Presenter保持INTERNAL",
        static_cast<unsigned>(kTaskStack),
        static_cast<unsigned>(kExtractTaskStack));
    const BaseType_t created = create_video_worker_task(
        benchmark_task, "VideoDecodeTask", kTaskStack, args, kTaskPriority, &g_task, kTaskCore);
    if (created != pdPASS) {
        heap_caps_free(path_copy);
        heap_caps_free(args);
        free_resources_unlocked();
        portENTER_CRITICAL(&g_mux);
        g_task = nullptr;
        g_snapshot.state = State::Failed;
        g_snapshot.result = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&g_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void stop()
{
    avi_mp3_bridge_cancel();
    portENTER_CRITICAL(&g_mux);
    if (g_task != nullptr || g_extract_task != nullptr) {
        // 不推进 generation：让正在退出的 task 仍可发布最终统计；只用 stop_generation
        // 让 custom I/O/read loop 立刻看见取消。下一次 start 才创建新 generation。
        g_stop_generation = g_generation;
        g_pause_requested = false;
        g_paused = false;
        g_extract_pause_ack = false;
        g_pause_started_us = 0LL;
        g_paused_seek_preview = false;
        g_seek_audio_resume_pending = false;
        g_snapshot.state = State::Stopped;
        g_snapshot.paused = false;
        g_snapshot.result = ESP_OK;
    }
    portEXIT_CRITICAL(&g_mux);
}

bool set_paused(bool paused)
{
    const int64_t now_us = esp_timer_get_time();
    uint32_t generation = 0U;
    bool running = false;

    portENTER_CRITICAL(&g_mux);
    running = g_task != nullptr && g_stop_generation != g_generation &&
        g_snapshot.state == State::Running;
    generation = g_generation;
    if (running && paused && !g_paused) {
        // 两阶段暂停：先只关 Extractor 入口，让已开始的 AVI/Bridge 写完整结束；
        // 等生产者到达帧边界后，再冻结 Decode/Presenter，避免半个 MP3 packet 留在 Bridge。
        g_pause_requested = true;
        g_extract_pause_ack = false;
    } else if (running && !paused && g_paused) {
        const int64_t paused_us = g_pause_started_us > 0LL ? now_us - g_pause_started_us : 0LL;
        if (paused_us > 0LL && g_presentation_clock.started &&
            g_presentation_clock.generation == g_generation) {
            g_presentation_clock.base_us += paused_us;
        }
        g_pause_requested = false;
        g_paused = false;
        g_extract_pause_ack = false;
        g_pause_started_us = 0LL;
        g_snapshot.paused = false;
    }
    const bool already_target = running && (g_paused == paused) &&
        (!paused || g_pause_requested);
    portEXIT_CRITICAL(&g_mux);

    if (!running) return false;
    if (!paused) return true;
    if (already_target) return true;

    const int64_t deadline_us = now_us + 500000LL;
    while (generation_current(generation)) {
        bool ack = false;
        portENTER_CRITICAL(&g_mux);
        ack = generation == g_generation && g_extract_pause_ack;
        portEXIT_CRITICAL(&g_mux);
        if (ack) break;
        if (esp_timer_get_time() >= deadline_us) {
            portENTER_CRITICAL(&g_mux);
            if (generation == g_generation) {
                g_pause_requested = false;
                g_extract_pause_ack = false;
            }
            portEXIT_CRITICAL(&g_mux);
            return false;
        }
        vTaskDelay(1);
    }

    portENTER_CRITICAL(&g_mux);
    const bool same_generation = generation == g_generation && g_stop_generation != generation;
    if (same_generation && g_extract_pause_ack) {
        g_paused = true;
        g_pause_started_us = esp_timer_get_time();
        g_snapshot.paused = true;
    }
    const bool success = same_generation && g_paused;
    portEXIT_CRITICAL(&g_mux);
    return success;
}

bool is_paused()
{
    bool paused = false;
    portENTER_CRITICAL(&g_mux);
    paused = g_task != nullptr && g_stop_generation != g_generation && g_paused;
    portEXIT_CRITICAL(&g_mux);
    return paused;
}

bool paused_seek_preview_active()
{
    bool active = false;
    portENTER_CRITICAL(&g_mux);
    active = g_paused_seek_preview &&
        g_task != nullptr && g_stop_generation != g_generation;
    portEXIT_CRITICAL(&g_mux);
    return active;
}

bool finish_paused_seek_preview()
{
    portENTER_CRITICAL(&g_mux);
    const bool was_expected = g_paused_seek_preview;
    g_paused_seek_preview = false;
    portEXIT_CRITICAL(&g_mux);
    if (!was_expected) return true;
    // 首帧已上屏，立即回到暂停；音频保持暂停，等待用户手动恢复。
    return set_paused(true);
}

bool can_seek()
{
    return VideoSeekIndex::count() > 0U;
}

uint32_t seek_epoch()
{
    uint32_t epoch = 0U;
    portENTER_CRITICAL(&g_mux);
    epoch = g_seek_epoch;
    portEXIT_CRITICAL(&g_mux);
    return epoch;
}

bool seek_audio_resume_pending()
{
    bool pending = false;
    portENTER_CRITICAL(&g_mux);
    pending = g_seek_audio_resume_pending &&
        g_task != nullptr && g_stop_generation != g_generation;
    portEXIT_CRITICAL(&g_mux);
    return pending;
}

bool resume_audio_after_seek()
{
    uint32_t generation = 0U;
    uint32_t epoch = 0U;
    bool pending = false;
    bool pause_requested = false;
    portENTER_CRITICAL(&g_mux);
    generation = g_generation;
    epoch = g_seek_epoch;
    pending = g_seek_audio_resume_pending &&
        g_task != nullptr && g_stop_generation != g_generation;
    pause_requested = g_pause_requested || g_paused;
    portEXIT_CRITICAL(&g_mux);

    if (!pending) return true;
    // 用户可能在首帧显示前再次暂停，或已经开始下一次 SEEK；此时不能提前放开音频。
    if (pause_requested || seek_epoch() != epoch) return true;
    if (!audio_service_video_mp3_resume(true)) {
        ESP_LOGE(TAG, "SEEK首帧显示后恢复音频失败");
        return false;
    }

    // 防止音频恢复命令执行期间发生暂停、下一次 SEEK 或播放世代切换。
    portENTER_CRITICAL(&g_mux);
    const bool must_pause = generation != g_generation || epoch != g_seek_epoch ||
        g_pause_requested || g_paused || g_stop_generation == g_generation;
    if (!must_pause && generation == g_generation) {
        g_seek_audio_resume_pending = false;
    }
    portEXIT_CRITICAL(&g_mux);
    if (must_pause) {
        (void)audio_service_video_mp3_pause(true);
        return true;
    }

    ESP_LOGI(TAG, "SEEK首帧已显示，音频时钟恢复推进");
    return true;
}

esp_err_t seek(uint32_t target_ms)
{
    if (!can_seek()) return ESP_ERR_NOT_SUPPORTED;

    // 仅在 Running 状态允许 seek
    bool running = false;
    uint32_t generation = 0U;
    portENTER_CRITICAL(&g_mux);
    running = g_task != nullptr && g_stop_generation != g_generation && g_snapshot.state == State::Running;
    generation = g_generation;
    portEXIT_CRITICAL(&g_mux);
    if (!running) return ESP_ERR_INVALID_STATE;

    // 索引偏移或扫描失败后已清空旧帧，不能简单恢复旧播放位置。
    // 终止当前世代并取消音频桥，由现有终止页统一停止 AudioTask，避免音画错位。
    auto abort_seek = [generation](esp_err_t error) -> esp_err_t {
        portENTER_CRITICAL(&g_mux);
        if (generation == g_generation) {
            g_seek_failure = error;
            g_stop_generation = generation;
            g_pause_requested = false;
            g_paused = false;
            g_extract_pause_ack = false;
            g_pause_started_us = 0LL;
            g_paused_seek_preview = false;
            g_seek_audio_resume_pending = false;
            g_snapshot.state = State::Failed;
            g_snapshot.result = error;
            g_snapshot.paused = false;
        }
        portEXIT_CRITICAL(&g_mux);
        avi_mp3_bridge_cancel();
        return error;
    };

    VideoSeekIndex::Entry entry = {};
    esp_err_t ret = VideoSeekIndex::lookup(target_ms, &entry);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Seek: target=%ums -> index=%ums offset=%u",
        static_cast<unsigned>(target_ms),
        static_cast<unsigned>(entry.timestamp_ms),
        static_cast<unsigned>(entry.file_offset));

    // 1. 暂停 pipeline，等待 ExtractTask 确认暂停
    const bool was_paused = is_paused();
    bool has_audio = false;
    portENTER_CRITICAL(&g_mux);
    has_audio = g_snapshot.audio_streams != 0U;
    portEXIT_CRITICAL(&g_mux);
    if (!set_paused(true)) {
        ESP_LOGE(TAG, "Seek前暂停失败");
        return ESP_FAIL;
    }
    for (int i = 0; i < 100; ++i) {
        bool ack = false;
        portENTER_CRITICAL(&g_mux);
        ack = g_extract_pause_ack;
        portEXIT_CRITICAL(&g_mux);
        if (ack) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // 2. 清空 compressed ready queue（未解码的压缩帧归还给 free）
    if (g_compressed_ready_queue != nullptr) {
        PipelineMessage message = {};
        while (xQueueReceive(g_compressed_ready_queue, &message, 0) == pdTRUE) {
            // 队列元素是 PipelineMessage，必须按消息取 slot；直接读 uint8_t 会读到 type 字段。
            if (message.type == PipelineMessageType::Frame &&
                message.slot < kCompressedVideoSlotCount) {
                return_compressed_to_free(message.slot);
            }
        }
    }

    // 3. 清空 RGB ready queue（已解码帧归还给 free）
    if (g_ready_queue != nullptr) {
        uint8_t slot = 0U;
        while (xQueueReceive(g_ready_queue, &slot, 0) == pdTRUE) {
            return_slot_to_free(slot);
        }
    }

    // 4. 音频必须先暂停：否则 AudioTask 在桥被清空、Extract 已停的情况下会阻塞在
    //    Source read 的空等循环里，VideoMp3Seek 命令要等到 1.5s 超时才可能执行，
    //    新旧位置音频就会混播一段，听起来就是“咔咔”。
    if (has_audio && !audio_service_video_mp3_pause(true)) {
        ESP_LOGE(TAG, "Seek暂停音频失败，终止当前视频");
        return abort_seek(ESP_FAIL);
    }

    // 5. 清空音频桥残留数据
    avi_mp3_bridge_clear();

    // 6. demuxer seek 到索引偏移，恢复绝对时间轴的帧/字节计数。
    // V2 直接使用索引生成时累计的音频字节数和原始视频帧序号；V1 保留旧估算方式。
    uint32_t audio_bytes_at_seek = entry.audio_bytes_before;
    uint32_t video_frame_index = entry.video_frame_index;
    if (!VideoSeekIndex::has_audio_byte_anchor()) {
        uint32_t audio_bitrate = 0U;
        portENTER_CRITICAL(&g_mux);
        audio_bitrate = g_snapshot.audio_bitrate;
        portEXIT_CRITICAL(&g_mux);
        const uint32_t audio_avg_bps = audio_bitrate / 8U;
        audio_bytes_at_seek = audio_avg_bps > 0U
            ? static_cast<uint32_t>(static_cast<uint64_t>(entry.timestamp_ms) * audio_avg_bps / 1000ULL)
            : 0U;
        video_frame_index = 0xFFFFFFFFU;
    }
    ret = AviDemuxer::seek_to_offset(
        entry.file_offset, entry.timestamp_ms, audio_bytes_at_seek, video_frame_index);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Demuxer seek 失败: offset=%u", entry.file_offset);
        return abort_seek(ret);
    }

    // 6.5 索引粒度为 2 秒，索引落点未必等于目标时间；顺序扫描丢弃 PTS 早于目标的视频帧，
    //     精确定位到目标帧。跳过的音频 chunk 只累加字节计数，保持音视频同一时间轴。
    // SEEK 时 ExtractTask 已确认暂停，复用独立 MP3 scratch 作为顺序丢弃缓冲，不额外分配 RAM。
    uint8_t *seek_scan_scratch = g_compressed_slots[kAudioScratchSlotIndex].data;
    const esp_err_t scan_ret = AviDemuxer::scan_forward_to(
        target_ms, seek_scan_scratch, kCompressedSlotBytes);
    if (scan_ret != ESP_OK) {
        ESP_LOGE(TAG, "Demuxer精确定位失败: target=%ums ret=%s",
            static_cast<unsigned>(target_ms), esp_err_to_name(scan_ret));
        return abort_seek(scan_ret);
    }

    // 7. 用 demuxer 实际恢复的时间轴位置对齐音频时钟。
    //     必须以 next_video_pts_ms() 为准，而不是索引时间戳：两者可能差 1~2 帧，
    //     音频时钟只要和视频 PTS 不同轴，Presenter 就会把后续帧全部判为迟到。
    //     该调用在 AudioTask 内执行：静音→排空 DMA 旧 PCM→丢弃解码残留→淡入，避免 seek 爆音。
    const uint32_t base_pts_ms = AviDemuxer::next_video_pts_ms();
    if (has_audio && !audio_service_video_mp3_seek(base_pts_ms)) {
        ESP_LOGE(TAG, "视频音频seek时钟失败，终止当前视频：base=%lums",
            static_cast<unsigned long>(base_pts_ms));
        return abort_seek(ESP_FAIL);
    }

    // 音频探针的 first_pts 是时钟零点基准，seek 后必须归零；
    // 同时立即刷新快照位置，让连续 seek 的目标计算保持正确。
    portENTER_CRITICAL(&g_mux);
    g_audio_probe = {};
    merge_audio_probe_unlocked(&g_snapshot);
    g_snapshot.first_pts_ms = base_pts_ms;
    g_snapshot.last_pts_ms = base_pts_ms;
    portEXIT_CRITICAL(&g_mux);

    // 8. 重置 presentation clock，并递增 seek epoch 让 Presenter 重新锚定
    portENTER_CRITICAL(&g_mux);
    g_presentation_clock = {};
    g_presentation_clock.generation = generation;
    g_presentation_clock.started = false;
    ++g_seek_epoch;
    g_seek_audio_resume_pending = has_audio;
    const uint32_t completed_epoch = g_seek_epoch;
    portEXIT_CRITICAL(&g_mux);

    ESP_LOGI(TAG, "SEEK新时间轴已就绪：epoch=%lu base=%lums audio_barrier=%s",
        static_cast<unsigned long>(completed_epoch),
        static_cast<unsigned long>(base_pts_ms),
        has_audio ? "等待首帧显示" : "无音轨");

    // SEEK 前正在播放时只恢复视频提取/解码；音频保持暂停，直到新位置首帧真正显示。
    // SEEK 前已经暂停时：短暂恢复提取/解码，仅让目标位置首帧上屏，
    // Presenter 展示完成后调用 finish_paused_seek_preview() 立即回到暂停。
    if (!was_paused) {
        if (!set_paused(false)) {
            ESP_LOGE(TAG, "Seek恢复视频失败，终止当前视频");
            return abort_seek(ESP_FAIL);
        }
        return ESP_OK;
    }
    portENTER_CRITICAL(&g_mux);
    if (generation == g_generation) g_paused_seek_preview = true;
    portEXIT_CRITICAL(&g_mux);
    if (!set_paused(false)) {
        portENTER_CRITICAL(&g_mux);
        if (generation == g_generation) g_paused_seek_preview = false;
        portEXIT_CRITICAL(&g_mux);
        ESP_LOGE(TAG, "暂停态Seek展示首帧失败，终止当前视频");
        return abort_seek(ESP_FAIL);
    }
    return ESP_OK;
}

bool get_snapshot(Snapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    portENTER_CRITICAL(&g_mux);
    *out_snapshot = g_snapshot;
    portEXIT_CRITICAL(&g_mux);
    return true;
}

bool set_presentation_clock(uint32_t frame_seek_epoch, uint32_t first_pts_ms, int64_t clock_base_us)
{
    if (clock_base_us <= 0LL) return false;

    bool published = false;
    portENTER_CRITICAL(&g_mux);
    // 与 SEEK 世代在同一临界区校验，防止旧帧在 SEEK 重置时钟后重新写入旧 PTS。
    if (frame_seek_epoch == g_seek_epoch &&
        g_task != nullptr && g_stop_generation != g_generation) {
        if (!g_presentation_clock.started ||
            g_presentation_clock.generation != g_generation) {
            g_presentation_clock.started = true;
            g_presentation_clock.generation = g_generation;
            g_presentation_clock.first_pts_ms = first_pts_ms;
            g_presentation_clock.base_us = clock_base_us;
            published = true;
        } else if (g_presentation_clock.first_pts_ms == first_pts_ms &&
                   g_presentation_clock.base_us == clock_base_us) {
            published = true;
        }
    }
    portEXIT_CRITICAL(&g_mux);

    return published;
}

static bool receive_frame(FrameView *out_frame, TickType_t wait_ticks)
{
    if (out_frame == nullptr || g_ready_queue == nullptr) return false;
    uint8_t slot_index = 0U;
    if (xQueueReceive(g_ready_queue, &slot_index, wait_ticks) != pdTRUE || slot_index >= kFrameSlotCount) return false;

    FrameSlot &slot = g_slots[slot_index];
    portENTER_CRITICAL(&g_mux);
    g_leased_mask |= static_cast<uint8_t>(1U << slot_index);
    portEXIT_CRITICAL(&g_mux);

    out_frame->rgb565_be = slot.rgb565_be;
    out_frame->bytes = slot.rgb565_bytes;
    out_frame->width = slot.width;
    out_frame->height = slot.height;
    out_frame->pts_ms = slot.pts_ms;
    out_frame->seek_epoch = slot.seek_epoch;
    out_frame->compressed_bytes = slot.compressed_bytes;
    out_frame->extract_us = slot.extract_us;
    out_frame->storage_us = slot.storage_us;
    out_frame->decode_us = slot.decode_us;
    out_frame->slot = slot_index;
    return true;
}

bool take_frame(FrameView *out_frame)
{
    return receive_frame(out_frame, 0);
}

bool wait_frame(FrameView *out_frame, uint32_t timeout_ms)
{
    TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms != 0U && wait_ticks == 0) wait_ticks = 1;
    return receive_frame(out_frame, wait_ticks);
}

void release_frame(uint8_t slot)
{
    if (slot >= kFrameSlotCount) return;
    bool was_leased = false;
    portENTER_CRITICAL(&g_mux);
    const uint8_t mask = static_cast<uint8_t>(1U << slot);
    was_leased = (g_leased_mask & mask) != 0U;
    g_leased_mask &= static_cast<uint8_t>(~mask);
    portEXIT_CRITICAL(&g_mux);
    if (was_leased) return_slot_to_free(slot);
}

esp_err_t cleanup()
{
    bool can_cleanup = false;
    portENTER_CRITICAL(&g_mux);
    can_cleanup = g_task == nullptr && g_extract_task == nullptr && g_leased_mask == 0U;
    portEXIT_CRITICAL(&g_mux);
    if (!can_cleanup) return ESP_ERR_INVALID_STATE;
    const esp_err_t bridge_ret = avi_mp3_bridge_release();
    if (bridge_ret != ESP_OK) return bridge_ret;
    free_resources_unlocked();
    return ESP_OK;
}

const char *state_name(State state)
{
    switch (state) {
        case State::Idle: return "Idle";
        case State::Starting: return "Starting";
        case State::Running: return "Running";
        case State::Eof: return "Eof";
        case State::Stopped: return "Stopped";
        case State::Failed: return "Failed";
        default: return "Unknown";
    }
}

} // namespace VideoBenchmark
