#include "video_benchmark.h"

#include <stdio.h>
#include <string.h>

#include "esp_extractor.h"
#include "esp_extractor_ctrl.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "audio/audio_service.h"
#include "audio/sources/avi_mp3_audio_source.h"
#include "video_media_io.h"
#include "video_jpeg_probe.h"

static const char *TAG = "VideoBench";

namespace VideoBenchmark
{
namespace
{

static constexpr size_t kFrameSlotCount = 2U;
static constexpr size_t kCompressedSlotCount = 8U;
static constexpr size_t kCompressedSlotBytes = 128U * 1024U;
static constexpr uint32_t kExtractorPoolBytes = 384U * 1024U;
static constexpr uint32_t kTaskStack = 8192U;
static constexpr uint32_t kExtractTaskStack = 6144U;
static constexpr UBaseType_t kTaskPriority = 2U;
static constexpr UBaseType_t kExtractTaskPriority = 2U;
// 视频播放双核调度：JPEG Decode=P2 固定在 Core1，避免 Core0/P5 AudioTask 对持续 CPU 解码的高频抢占。
// Extractor=P2 固定在 Core0，主要等待 SD/I/O，可让位给 AudioTask=P5 与 Presenter=P3。
static constexpr BaseType_t kTaskCore = 1;
static constexpr BaseType_t kExtractTaskCore = 0;
static constexpr TickType_t kQueuePollTicks = pdMS_TO_TICKS(20);
static constexpr uint32_t kPublishEveryFrames = 8U;
static constexpr uint32_t kDecodeYieldEveryFrames = 8U;
static constexpr TickType_t kDecodeYieldTicks = 1U;
static constexpr int64_t kUiEmergencyLateUs = 50000LL;
static constexpr uint32_t kDeadlineSafetyBaseUs = 5000U;
static constexpr uint32_t kDeadlineSafetyMaxUs = 12000U;
static constexpr uint32_t kDecodeJitterReserveMaxUs = 10000U;
static constexpr uint8_t kDecodeTimingEwmaShift = 3U; // alpha = 1/8
static constexpr size_t kAudioBridgeBytes = 32U * 1024U;
static constexpr size_t kAudioPrebufferBytes = 4U * 1024U;
static constexpr uint32_t kAudioPrebufferTimeoutMs = 1500U;
static constexpr uint32_t kAudioBridgePushTimeoutMs = 250U;

struct FrameSlot
{
    uint8_t *rgb565_be = nullptr;
    size_t rgb565_bytes = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint32_t pts_ms = 0U;
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
};

struct ExtractTaskArgs
{
    esp_extractor_handle_t extractor = nullptr;
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
static QueueHandle_t g_free_queue = nullptr;
static QueueHandle_t g_ready_queue = nullptr;
static QueueHandle_t g_compressed_free_queue = nullptr;
static QueueHandle_t g_compressed_ready_queue = nullptr;
static FrameSlot g_slots[kFrameSlotCount] = {};
static CompressedSlot g_compressed_slots[kCompressedSlotCount] = {};
static uint8_t g_leased_mask = 0U;
static PresentationClock g_presentation_clock = {};
static AudioProbeStats g_audio_probe = {};

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
    const esp_extractor_frame_info_t &frame,
    uint32_t extract_us,
    uint32_t storage_us,
    uint32_t generation)
{
    bool first = false;
    portENTER_CRITICAL(&g_mux);
    if (generation == g_generation && g_stop_generation != generation) {
        first = g_audio_probe.frames_read == 0U;
        ++g_audio_probe.frames_read;
        g_audio_probe.compressed_bytes += frame.frame_size;
        if (frame.frame_size > g_audio_probe.compressed_bytes_max) {
            g_audio_probe.compressed_bytes_max = frame.frame_size;
        }
        if (first) g_audio_probe.first_pts_ms = frame.pts;
        g_audio_probe.last_pts_ms = frame.pts;
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

static void publish(const Snapshot &snapshot)
{
    portENTER_CRITICAL(&g_mux);
    if (snapshot.generation == g_generation) {
        g_snapshot = snapshot;
        merge_audio_probe_unlocked(&g_snapshot);
    }
    portEXIT_CRITICAL(&g_mux);
}

static void publish_task_exit(const Snapshot &snapshot)
{
    portENTER_CRITICAL(&g_mux);
    if (snapshot.generation == g_generation) {
        g_snapshot = snapshot;
        merge_audio_probe_unlocked(&g_snapshot);
    }
    g_task = nullptr;
    portEXIT_CRITICAL(&g_mux);
}

static void return_slot_to_free(uint8_t slot)
{
    if (g_free_queue == nullptr || slot >= kFrameSlotCount) return;
    (void)xQueueSend(g_free_queue, &slot, 0);
}

static bool send_ready(uint8_t slot, uint32_t generation)
{
    while (generation_current(generation)) {
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


static bool take_compressed_free(uint8_t *slot, uint32_t generation)
{
    if (slot == nullptr) return false;
    while (generation_current(generation)) {
        if (g_compressed_free_queue != nullptr &&
            xQueueReceive(g_compressed_free_queue, slot, kQueuePollTicks) == pdTRUE) {
            return true;
        }
    }
    return false;
}

static void return_compressed_to_free(uint8_t slot)
{
    if (g_compressed_free_queue == nullptr || slot >= kCompressedSlotCount) return;
    (void)xQueueSend(g_compressed_free_queue, &slot, 0);
}

static bool send_pipeline_message(const PipelineMessage &message, uint32_t generation)
{
    while (generation_current(generation)) {
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
    if (generation == g_generation) g_stop_generation = generation;
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

static esp_err_t validate_stream(
    esp_extractor_handle_t extractor,
    Snapshot *snapshot,
    bool allow_fullcanvas_over20)
{
    if (extractor == nullptr || snapshot == nullptr) return ESP_ERR_INVALID_ARG;
    uint16_t video_num = 0U;
    if (esp_extractor_get_stream_num(extractor, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, &video_num) != ESP_EXTRACTOR_ERR_OK ||
        video_num == 0U) return ESP_ERR_NOT_FOUND;

    esp_extractor_stream_info_t info = {};
    if (esp_extractor_get_stream_info(
            extractor, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, 0U, &info) != ESP_EXTRACTOR_ERR_OK) {
        return ESP_FAIL;
    }
    snapshot->width = info.video_info.width;
    snapshot->height = info.video_info.height;
    snapshot->fps_hint = info.video_info.fps; // AVI dwRate/dwScale 可能不是实际整数 fps，仅诊断。
    snapshot->duration_ms = info.duration;
    if (info.video_info.format != ESP_EXTRACTOR_VIDEO_FORMAT_MJPEG) return ESP_ERR_NOT_SUPPORTED;
    if (snapshot->width == 0U || snapshot->height == 0U ||
        snapshot->width > kWidth || snapshot->height > kHeight) {
        ESP_LOGE(TAG, "Video尺寸超出画布：stream=%ux%u canvas<=%ux%u",
            static_cast<unsigned>(snapshot->width), static_cast<unsigned>(snapshot->height),
            static_cast<unsigned>(kWidth), static_cast<unsigned>(kHeight));
        return ESP_ERR_INVALID_SIZE;
    }

    // 460x460 + >20FPS 在实机上可能因 JPEG 解码长期超过帧预算，最终反压 AVI Demux 并造成 MP3 断音。
    // 不做运行时自动降帧：首次启动只返回风险信号，由 UI 让用户决定是否仍按源FPS播放。
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

    uint16_t audio_num = 0U;
    const esp_extractor_err_t audio_num_ret = esp_extractor_get_stream_num(
        extractor, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, &audio_num);
    if (audio_num_ret == ESP_EXTRACTOR_ERR_OK && audio_num > 0U) {
        esp_extractor_stream_info_t audio = {};
        if (esp_extractor_get_stream_info(
                extractor, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, 0U, &audio) != ESP_EXTRACTOR_ERR_OK) {
            return ESP_FAIL;
        }
        snapshot->audio_streams = audio_num;
        snapshot->audio_format = static_cast<uint32_t>(audio.audio_info.format);
        snapshot->audio_duration_ms = audio.duration;
        snapshot->audio_bitrate = audio.bitrate;
        snapshot->audio_sample_rate = audio.audio_info.sample_rate;
        snapshot->audio_channels = audio.audio_info.channel;
        snapshot->audio_bits_per_sample = audio.audio_info.bits_per_sample;
        if (audio.audio_info.format != ESP_EXTRACTOR_AUDIO_FORMAT_MP3) {
            ESP_LOGE(TAG,
                "AVI AUDIO轨暂不支持：format=0x%08lx streams=%u；Video V1要求MP3",
                static_cast<unsigned long>(snapshot->audio_format),
                static_cast<unsigned>(audio_num));
            return ESP_ERR_NOT_SUPPORTED;
        }
        ESP_LOGI(TAG,
            "AVI MP3 Audio Pipeline V1：streams=%u %luHz/%uch bits=%u bitrate=%lu duration=%lums；Extractor->32KB PSRAM Bridge->AudioTask->MP3->PCM",
            static_cast<unsigned>(audio_num),
            static_cast<unsigned long>(snapshot->audio_sample_rate),
            static_cast<unsigned>(snapshot->audio_channels),
            static_cast<unsigned>(snapshot->audio_bits_per_sample),
            static_cast<unsigned long>(snapshot->audio_bitrate),
            static_cast<unsigned long>(snapshot->audio_duration_ms));
    } else if (audio_num_ret == ESP_EXTRACTOR_ERR_OK || audio_num_ret == ESP_EXTRACTOR_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "AVI MP3 Audio Pipeline：未发现AUDIO轨；继续VIDEO-only兼容路径");
    } else {
        ESP_LOGE(TAG, "查询AVI AUDIO轨失败：extractor_ret=%d", static_cast<int>(audio_num_ret));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t decode_frame(
    jpeg_dec_handle_t decoder,
    const esp_extractor_frame_info_t &frame,
    uint16_t expected_width,
    uint16_t expected_height,
    FrameSlot *slot,
    uint32_t *out_decode_us)
{
    if (decoder == nullptr || slot == nullptr || slot->rgb565_be == nullptr ||
        frame.frame_buffer == nullptr || frame.frame_size == 0U || out_decode_us == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t info = {};
    io.inbuf = frame.frame_buffer;
    io.inbuf_len = static_cast<int>(frame.frame_size);

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
    if (args == nullptr || args->extractor == nullptr || args->io == nullptr) {
        if (args != nullptr) heap_caps_free(args);
        portENTER_CRITICAL(&g_mux);
        g_extract_task = nullptr;
        portEXIT_CRITICAL(&g_mux);
        vTaskDelete(nullptr);
        return;
    }

    while (generation_current(args->generation)) {
        uint8_t compressed_slot_index = 0U;
        if (!take_compressed_free(&compressed_slot_index, args->generation)) break;

        esp_extractor_frame_info_t frame = {};
        const uint64_t storage_before = args->io->read_us_total;
        const int64_t extract_started_us = esp_timer_get_time();
        const esp_extractor_err_t ex = esp_extractor_read_frame(args->extractor, &frame);
        const uint32_t extract_us = static_cast<uint32_t>(esp_timer_get_time() - extract_started_us);
        const uint32_t storage_us = static_cast<uint32_t>(args->io->read_us_total - storage_before);

        if (ex == ESP_EXTRACTOR_ERR_WAITING_OUTPUT || ex == ESP_EXTRACTOR_ERR_NEED_MORE_BUF) {
            return_compressed_to_free(compressed_slot_index);
            vTaskDelay(1);
            continue;
        }

        if (ex == ESP_EXTRACTOR_ERR_SKIPPED) {
            return_compressed_to_free(compressed_slot_index);
            PipelineMessage message = {};
            message.type = PipelineMessageType::PoolSkipped;
            message.result = ESP_OK;
            if (!send_pipeline_message(message, args->generation)) break;
            continue;
        }

        if (ex == ESP_EXTRACTOR_ERR_EOS) {
            return_compressed_to_free(compressed_slot_index);
            avi_mp3_bridge_mark_eof();
            PipelineMessage message = {};
            message.type = PipelineMessageType::Eof;
            message.result = ESP_OK;
            (void)send_pipeline_message(message, args->generation);
            break;
        }

        if (ex != ESP_EXTRACTOR_ERR_OK) {
            return_compressed_to_free(compressed_slot_index);
            avi_mp3_bridge_cancel();
            PipelineMessage message = {};
            if (!generation_current(args->generation) || ex == ESP_EXTRACTOR_ERR_ABORTED) {
                message.type = PipelineMessageType::Stopped;
                message.result = ESP_OK;
            } else {
                message.type = PipelineMessageType::Failed;
                message.result = VideoMediaIo::map_extractor_error(ex);
            }
            (void)send_pipeline_message(message, args->generation);
            break;
        }

        if (frame.stream_type == ESP_EXTRACTOR_STREAM_TYPE_AUDIO) {
            esp_err_t audio_push_ret = ESP_OK;
            if (frame.frame_buffer != nullptr && frame.frame_size != 0U) {
                (void)record_audio_probe(
                    frame, extract_us, storage_us, args->generation);
                audio_push_ret = avi_mp3_bridge_push(
                    frame.frame_buffer, frame.frame_size, kAudioBridgePushTimeoutMs);
            }
            (void)esp_extractor_release_frame(args->extractor, &frame);
            return_compressed_to_free(compressed_slot_index);
            if (audio_push_ret != ESP_OK) {
                const bool generation_alive = generation_current(args->generation);
                PipelineMessage message = {};
                message.type = generation_alive
                    ? PipelineMessageType::Failed : PipelineMessageType::Stopped;
                message.result = generation_alive ? audio_push_ret : ESP_OK;
                if (generation_alive) {
                    ESP_LOGE(TAG, "AVI MP3 Bridge写入失败：ret=%s pts=%lums bytes=%lu",
                        esp_err_to_name(audio_push_ret),
                        static_cast<unsigned long>(frame.pts),
                        static_cast<unsigned long>(frame.frame_size));
                }
                (void)send_pipeline_message(message, args->generation);
                break;
            }
            continue;
        }

        if (frame.stream_type != ESP_EXTRACTOR_STREAM_TYPE_VIDEO ||
            frame.frame_buffer == nullptr || frame.frame_size == 0U) {
            (void)esp_extractor_release_frame(args->extractor, &frame);
            return_compressed_to_free(compressed_slot_index);
            continue;
        }

        if (frame.frame_size > kCompressedSlotBytes) {
            ESP_LOGE(TAG,
                "MJPEG压缩帧超过Pipeline槽容量：bytes=%lu capacity=%uKB pts=%lums",
                static_cast<unsigned long>(frame.frame_size),
                static_cast<unsigned>(kCompressedSlotBytes / 1024U),
                static_cast<unsigned long>(frame.pts));
            (void)esp_extractor_release_frame(args->extractor, &frame);
            return_compressed_to_free(compressed_slot_index);
            PipelineMessage message = {};
            message.type = PipelineMessageType::Failed;
            message.result = ESP_ERR_INVALID_SIZE;
            (void)send_pipeline_message(message, args->generation);
            break;
        }

        CompressedSlot &slot = g_compressed_slots[compressed_slot_index];
        const int64_t copy_started_us = esp_timer_get_time();
        memcpy(slot.data, frame.frame_buffer, frame.frame_size);
        const uint32_t copy_us = static_cast<uint32_t>(esp_timer_get_time() - copy_started_us);
        slot.bytes = frame.frame_size;
        slot.pts_ms = frame.pts;
        slot.extract_us = extract_us;
        slot.storage_us = storage_us;
        slot.copy_us = copy_us;

        // Extractor pool frame 不跨任务持有；压缩 payload 复制到 PSRAM 后立即归还。
        (void)esp_extractor_release_frame(args->extractor, &frame);

        PipelineMessage message = {};
        message.type = PipelineMessageType::Frame;
        message.slot = compressed_slot_index;
        message.result = ESP_OK;
        if (!send_pipeline_message(message, args->generation)) {
            return_compressed_to_free(compressed_slot_index);
            break;
        }
    }

    heap_caps_free(args);
    portENTER_CRITICAL(&g_mux);
    g_extract_task = nullptr;
    portEXIT_CRITICAL(&g_mux);
    vTaskDelete(nullptr);
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

    uint32_t audio_first_pts_ms = 0U;
    bool have_audio_pts = false;
    portENTER_CRITICAL(&g_mux);
    if (g_audio_probe.frames_read != 0U) {
        audio_first_pts_ms = g_audio_probe.first_pts_ms;
        have_audio_pts = true;
    }
    portEXIT_CRITICAL(&g_mux);
    if (!have_audio_pts) return false;

    AudioVideoClockSnapshot audio_clock = {};
    if (!audio_service_video_mp3_get_clock(&audio_clock) ||
        !audio_clock.active || audio_clock.eof || audio_clock.sample_rate_hz == 0U ||
        audio_clock.submitted_frames == 0ULL) {
        return false;
    }

    const int64_t target_audio_us =
        (static_cast<int64_t>(video_pts_ms) - static_cast<int64_t>(audio_first_pts_ms)) * 1000LL;
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
        vTaskDelete(nullptr);
        return;
    }

    Snapshot result = {};
    result.state = State::Failed;
    result.generation = args->generation;
    result.result = ESP_FAIL;

    VideoMediaIo::Context io = {};
    VideoMediaIo::configure(&io, args->generation, generation_current);
    esp_extractor_handle_t extractor = nullptr;
    jpeg_dec_handle_t decoder = nullptr;

    esp_err_t ret = VideoMediaIo::open(args->path, &io);
    if (ret == ESP_OK && generation_current(args->generation)) {
        esp_extractor_config_t cfg = {};
        cfg.type = ESP_EXTRACTOR_TYPE_AVI;
        cfg.extract_mask = ESP_EXTRACT_MASK_AV;
        cfg.in_read_cb = VideoMediaIo::read_cb;
        cfg.in_seek_cb = VideoMediaIo::seek_cb;
        cfg.in_size_cb = VideoMediaIo::size_cb;
        cfg.in_ctx = &io;
        cfg.out_pool_size = kExtractorPoolBytes;
        cfg.out_align = 16U;
        ret = VideoMediaIo::map_extractor_error(esp_extractor_open(&cfg, &extractor));
        if (ret == ESP_OK) {
            // R.40.2 只做顺序 decode/display benchmark，不做 seek。关闭 AVI 全量索引，
            // 避免长片 idx（实机样本约 3.5MB）为了本阶段无用的 seek 占用内存/解析时间。
            bool no_indexing = true;
            ret = VideoMediaIo::map_extractor_error(esp_extractor_ctrl(
                extractor, ESP_EXTRACTOR_CTRL_TYPE_SET_NO_INDEXING,
                &no_indexing, sizeof(no_indexing)));
        }
        if (ret == ESP_OK) ret = VideoMediaIo::map_extractor_error(esp_extractor_parse_stream(extractor));
        if (ret == ESP_OK) {
            ret = validate_stream(extractor, &result, args->allow_fullcanvas_over20);
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
    uint32_t catchup_events = 0U;
    bool catchup_active = false;
    int64_t projected_late_us_max = 0LL;

    if (ret == ESP_OK && generation_current(args->generation)) {
        ExtractTaskArgs *extract_args = static_cast<ExtractTaskArgs *>(
            heap_caps_calloc(1U, sizeof(ExtractTaskArgs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (extract_args == nullptr) {
            ret = ESP_ERR_NO_MEM;
        } else {
            extract_args->extractor = extractor;
            extract_args->io = &io;
            extract_args->generation = args->generation;
            const BaseType_t created = xTaskCreatePinnedToCore(
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
                "AVI MP3预取失败：need=%uB buffered=%uB high=%uB wait=%lums ret=%s；拒绝带欠载风险启动AudioTask",
                static_cast<unsigned>(kAudioPrebufferBytes),
                static_cast<unsigned>(prebuffer.buffered_bytes),
                static_cast<unsigned>(prebuffer.high_water_bytes),
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
            PipelineMessage message = {};
            if (!receive_pipeline_message(&message, args->generation)) break;

            if (message.type == PipelineMessageType::PoolSkipped) {
                ++result.frames_pool_skipped;
                if ((result.frames_pool_skipped & 0x0FU) == 1U) {
                    ESP_LOGW(TAG, "Extractor pool跳帧：count=%lu pool=%uKB；需降低MJPEG单帧大小或提高pool",
                        static_cast<unsigned long>(result.frames_pool_skipped),
                        static_cast<unsigned>(kExtractorPoolBytes / 1024U));
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
            if (message.type != PipelineMessageType::Frame || message.slot >= kCompressedSlotCount) {
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

            esp_extractor_frame_info_t frame = {};
            frame.stream_type = ESP_EXTRACTOR_STREAM_TYPE_VIDEO;
            frame.frame_buffer = compressed.data;
            frame.frame_size = compressed.bytes;
            frame.pts = compressed.pts_ms;

            FrameSlot &slot = g_slots[slot_index];
            uint32_t decode_us = 0U;
            ret = decode_frame(
                decoder, frame, result.width, result.height, &slot, &decode_us);
            const uint32_t pts_ms = compressed.pts_ms;
            const uint32_t compressed_bytes = compressed.bytes;
            const uint32_t extract_us = compressed.extract_us;
            const uint32_t storage_us = compressed.storage_us;
            return_compressed_to_free(message.slot);

            if (ret != ESP_OK) {
                ++result.decode_failures;
                return_slot_to_free(slot_index);
                result.state = State::Failed;
                result.result = ret;
                ESP_LOGE(TAG, "MJPEG解码失败：frame=%lu pts=%lums bytes=%lu ret=%s",
                    static_cast<unsigned long>(result.frames_read),
                    static_cast<unsigned long>(pts_ms),
                    static_cast<unsigned long>(compressed_bytes), esp_err_to_name(ret));
                stop_generation(args->generation);
                done = true;
                break;
            }

            slot.pts_ms = pts_ms;
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

            if (!send_ready(slot_index, args->generation)) {
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
    if (extractor != nullptr) esp_extractor_close(extractor);
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
    vTaskDelete(nullptr);
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

esp_err_t start(const char *path, bool allow_fullcanvas_over20)
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
    g_compressed_free_queue = xQueueCreate(kCompressedSlotCount, sizeof(uint8_t));
    g_compressed_ready_queue = xQueueCreate(kCompressedSlotCount + 2U, sizeof(PipelineMessage));
    if (g_free_queue == nullptr || g_ready_queue == nullptr ||
        g_compressed_free_queue == nullptr || g_compressed_ready_queue == nullptr) {
        free_resources_unlocked();
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t i = 0U; i < kFrameSlotCount; ++i) (void)xQueueSend(g_free_queue, &i, 0);
    for (uint8_t i = 0U; i < kCompressedSlotCount; ++i) {
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
    g_snapshot = {};
    g_snapshot.state = State::Starting;
    g_snapshot.generation = g_generation;
    g_stop_generation = 0U;
    g_extract_task = nullptr;
    g_leased_mask = 0U;
    g_presentation_clock = {};
    g_presentation_clock.generation = g_generation;
    g_audio_probe = {};
    portEXIT_CRITICAL(&g_mux);

    // 直接让 FreeRTOS 写入全局 handle，避免 task 极快退出后 caller 再把过期 handle 写回 g_task。
    const BaseType_t created = xTaskCreatePinnedToCore(
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
        g_snapshot.state = State::Stopped;
        g_snapshot.result = ESP_OK;
    }
    portEXIT_CRITICAL(&g_mux);
}

bool get_snapshot(Snapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    portENTER_CRITICAL(&g_mux);
    *out_snapshot = g_snapshot;
    portEXIT_CRITICAL(&g_mux);
    return true;
}

bool set_presentation_clock(uint32_t first_pts_ms, int64_t clock_base_us)
{
    if (clock_base_us <= 0LL) return false;

    bool published = false;
    portENTER_CRITICAL(&g_mux);
    if (g_task != nullptr && g_stop_generation != g_generation) {
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
