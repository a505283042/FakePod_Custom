#include "video_probe.h"

#include <stdio.h>
#include <string.h>
#include "esp_extractor.h"
#include "esp_extractor_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "video_media_io.h"

static const char *TAG = "VideoProbe";

namespace VideoProbe
{
namespace
{

static constexpr uint32_t kExtractorPoolBytes = 16U * 1024U;
static constexpr uint32_t kProbeTaskStack = 6144U;
static constexpr UBaseType_t kProbeTaskPriority = 2U;
static constexpr BaseType_t kProbeTaskCore = 0;

struct TaskArgs
{
    char *path = nullptr; // PSRAM
    uint32_t generation = 0;
};

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static Snapshot g_snapshot = {};
static uint32_t g_generation = 1U;
static uint32_t g_active_tasks = 0U;
static bool g_registered = false;

static bool generation_current(uint32_t generation)
{
    bool current = false;
    portENTER_CRITICAL(&g_mux);
    current = generation == g_generation;
    portEXIT_CRITICAL(&g_mux);
    return current;
}

static void publish(const Snapshot &snapshot)
{
    portENTER_CRITICAL(&g_mux);
    if (snapshot.generation == g_generation) g_snapshot = snapshot;
    portEXIT_CRITICAL(&g_mux);
}

static void task_finished()
{
    portENTER_CRITICAL(&g_mux);
    if (g_active_tasks > 0U) --g_active_tasks;
    portEXIT_CRITICAL(&g_mux);
}

static uint32_t active_task_count()
{
    uint32_t count = 0U;
    portENTER_CRITICAL(&g_mux);
    count = g_active_tasks;
    portEXIT_CRITICAL(&g_mux);
    return count;
}

static void probe_task(void *arg)
{
    TaskArgs *args = static_cast<TaskArgs *>(arg);
    if (args == nullptr || args->path == nullptr) {
        if (args != nullptr) heap_caps_free(args);
        task_finished();
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

    esp_err_t ret = VideoMediaIo::open(args->path, &io);
    if (ret == ESP_OK && generation_current(args->generation)) {
        result.file_size = io.size;
        esp_extractor_config_t cfg = {};
        cfg.type = ESP_EXTRACTOR_TYPE_AVI;
        cfg.extract_mask = ESP_EXTRACT_MASK_AV;
        cfg.in_read_cb = VideoMediaIo::read_cb;
        cfg.in_seek_cb = VideoMediaIo::seek_cb;
        cfg.in_size_cb = VideoMediaIo::size_cb;
        cfg.in_ctx = &io;
        cfg.out_pool_size = kExtractorPoolBytes;
        cfg.out_align = 16U;

        esp_extractor_err_t ex = esp_extractor_open(&cfg, &extractor);
        ret = VideoMediaIo::map_extractor_error(ex);
        if (ret == ESP_OK) {
            ex = esp_extractor_parse_stream(extractor);
            ret = VideoMediaIo::map_extractor_error(ex);
        }
    }

    if (ret == ESP_OK && generation_current(args->generation)) {
        uint16_t video_num = 0U;
        uint16_t audio_num = 0U;
        const esp_extractor_err_t vr = esp_extractor_get_stream_num(
            extractor, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, &video_num);
        const esp_extractor_err_t ar = esp_extractor_get_stream_num(
            extractor, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, &audio_num);
        result.has_video = vr == ESP_EXTRACTOR_ERR_OK && video_num > 0U;
        result.has_audio = ar == ESP_EXTRACTOR_ERR_OK && audio_num > 0U;

        if (result.has_video) {
            esp_extractor_stream_info_t info = {};
            if (esp_extractor_get_stream_info(
                    extractor, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, 0U, &info) == ESP_EXTRACTOR_ERR_OK) {
                result.width = info.video_info.width;
                result.height = info.video_info.height;
                result.fps = info.video_info.fps;
                result.duration_ms = info.duration;
                result.video_bitrate = info.bitrate;
                result.video_is_mjpeg = info.video_info.format == ESP_EXTRACTOR_VIDEO_FORMAT_MJPEG;
            }
        }
        if (result.has_audio) {
            esp_extractor_stream_info_t info = {};
            if (esp_extractor_get_stream_info(
                    extractor, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, 0U, &info) == ESP_EXTRACTOR_ERR_OK) {
                result.audio_sample_rate = info.audio_info.sample_rate;
                result.audio_channels = info.audio_info.channel;
                result.audio_bitrate = info.bitrate;
                result.audio_is_mp3 = info.audio_info.format == ESP_EXTRACTOR_AUDIO_FORMAT_MP3;
                if (result.duration_ms == 0U) result.duration_ms = info.duration;
            }
        }

        // Probe 只验证 AVI time-seek 是否可调用，不构建播放队列。
        const uint32_t seek_target = result.duration_ms > 2000U ? 1000U : 0U;
        result.seek_probe_ok = esp_extractor_seek(extractor, seek_target) == ESP_EXTRACTOR_ERR_OK;
        result.state = State::Ready;
        result.result = ESP_OK;
    } else {
        result.result = ret;
    }

    if (extractor != nullptr) esp_extractor_close(extractor);
    VideoMediaIo::close(&io);

    if (generation_current(args->generation)) {
        publish(result);
        if (result.state == State::Ready) {
            ESP_LOGI(TAG,
                "AVI Probe完成：%ux%u %ufps video=%s audio=%s %luHz/%uch duration=%lums seek=%s",
                static_cast<unsigned>(result.width),
                static_cast<unsigned>(result.height),
                static_cast<unsigned>(result.fps),
                result.video_is_mjpeg ? "MJPEG" : (result.has_video ? "OTHER" : "NONE"),
                result.audio_is_mp3 ? "MP3" : (result.has_audio ? "OTHER" : "NONE"),
                static_cast<unsigned long>(result.audio_sample_rate),
                static_cast<unsigned>(result.audio_channels),
                static_cast<unsigned long>(result.duration_ms),
                result.seek_probe_ok ? "YES" : "NO");
        } else {
            ESP_LOGW(TAG, "AVI Probe失败：path=%s ret=%s", args->path, esp_err_to_name(result.result));
        }
    }

    heap_caps_free(args->path);
    heap_caps_free(args);
    task_finished();
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t init()
{
    if (g_registered) return ESP_OK;
    const esp_extractor_err_t ret = esp_extractor_register_default();
    if (ret != ESP_EXTRACTOR_ERR_OK) return VideoMediaIo::map_extractor_error(ret);
    g_registered = true;
    ESP_LOGI(TAG, "ESP Extractor已注册：AVI-only，共享Storage IO；MJPEG+MP3 metadata/seek + R.40.2 VIDEO benchmark");
    return ESP_OK;
}

esp_err_t start(const char *path)
{
    if (!g_registered || path == nullptr || path[0] == '\0') return ESP_ERR_INVALID_STATE;
    TaskArgs *args = static_cast<TaskArgs *>(heap_caps_calloc(1U, sizeof(TaskArgs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    char *path_copy = static_cast<char *>(heap_caps_calloc(512U, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (args == nullptr || path_copy == nullptr) {
        if (args != nullptr) heap_caps_free(args);
        if (path_copy != nullptr) heap_caps_free(path_copy);
        return ESP_ERR_NO_MEM;
    }
    snprintf(path_copy, 512U, "%s", path);

    portENTER_CRITICAL(&g_mux);
    ++g_generation;
    if (g_generation == 0U) ++g_generation;
    args->generation = g_generation;
    args->path = path_copy;
    ++g_active_tasks;
    g_snapshot = {};
    g_snapshot.state = State::Running;
    g_snapshot.generation = g_generation;
    portEXIT_CRITICAL(&g_mux);

    const BaseType_t created = xTaskCreatePinnedToCore(
        probe_task,
        "VideoProbeTask",
        kProbeTaskStack,
        args,
        kProbeTaskPriority,
        nullptr,
        kProbeTaskCore);
    if (created != pdPASS) {
        heap_caps_free(path_copy);
        heap_caps_free(args);
        portENTER_CRITICAL(&g_mux);
        if (g_active_tasks > 0U) --g_active_tasks;
        g_snapshot.state = State::Failed;
        g_snapshot.result = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&g_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void cancel()
{
    portENTER_CRITICAL(&g_mux);
    ++g_generation;
    if (g_generation == 0U) ++g_generation;
    g_snapshot = {};
    g_snapshot.generation = g_generation;
    portEXIT_CRITICAL(&g_mux);
}

bool prepare_storage_handoff(TickType_t timeout_ticks)
{
    cancel();
    const TickType_t started = xTaskGetTickCount();
    TickType_t poll = pdMS_TO_TICKS(5);
    if (poll == 0) poll = 1;

    while (active_task_count() != 0U) {
        if (timeout_ticks == 0 ||
            (timeout_ticks != portMAX_DELAY && xTaskGetTickCount() - started >= timeout_ticks)) {
            ESP_LOGE(TAG, "USB接管等待AVI Probe退出超时：active=%lu",
                static_cast<unsigned long>(active_task_count()));
            return false;
        }
        vTaskDelay(poll);
    }
    return true;
}

bool get_snapshot(Snapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    portENTER_CRITICAL(&g_mux);
    *out_snapshot = g_snapshot;
    portEXIT_CRITICAL(&g_mux);
    return true;
}

const char *state_name(State state)
{
    switch (state) {
        case State::Idle: return "Idle";
        case State::Running: return "Running";
        case State::Ready: return "Ready";
        case State::Failed: return "Failed";
        default: return "Unknown";
    }
}

} // namespace VideoProbe
