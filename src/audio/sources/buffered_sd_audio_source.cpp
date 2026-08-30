#include "buffered_sd_audio_source.h"

#include "app_diag_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sd_file_audio_source.h"

static const char *TAG = "音频预读";

// 沿用已经在 44.1/48kHz FLAC 路径验证过的低采样率预取参数。
// MP3/WAV 当前也只支持 44.1/48kHz，不为它们另造一套未经验证的调度参数。
static constexpr size_t BUFFERED_SD_RING_BYTES = 192 * 1024;
static constexpr size_t BUFFERED_SD_READ_CHUNK_BYTES = 8 * 1024;
static constexpr size_t BUFFERED_SD_START_TARGET_BYTES = 64 * 1024;
static constexpr uint32_t BUFFERED_SD_TASK_STACK_BYTES = 3072;
static constexpr UBaseType_t BUFFERED_SD_TASK_PRIORITY = 4;
static constexpr BaseType_t BUFFERED_SD_TASK_CORE = 1;
static constexpr uint32_t BUFFERED_SD_COOPERATIVE_READ_BATCH = 4;
static constexpr TickType_t BUFFERED_SD_COOPERATIVE_BLOCK_TICKS = 1;
static constexpr TickType_t BUFFERED_SD_SEND_WAIT = pdMS_TO_TICKS(20);
static constexpr TickType_t BUFFERED_SD_RECEIVE_WAIT = pdMS_TO_TICKS(20);
static constexpr TickType_t BUFFERED_SD_START_WAIT = pdMS_TO_TICKS(750);
static constexpr TickType_t BUFFERED_SD_STOP_WARN_WAIT = pdMS_TO_TICKS(1000);

struct BufferedSdContext
{
    AudioSource backend = {};
    SdFileAudioSource backend_storage = {};

    StreamBufferHandle_t stream = nullptr;
    StaticStreamBuffer_t stream_storage = {};
    StaticSemaphore_t done_storage = {};
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t task = nullptr;

    uint8_t *ring_storage = nullptr;
    uint8_t *read_buffer = nullptr;
    uint64_t size_bytes = 0;
    volatile BaseType_t core_id = -1;
    volatile bool stop_requested = false;
    volatile bool eof = false;
    volatile bool io_error = false;
};

static BufferedSdContext *buffered_context(void *context)
{
    return static_cast<BufferedSdContext *>(context);
}

static void buffered_sd_task(void *arg)
{
    BufferedSdContext *context = static_cast<BufferedSdContext *>(arg);
    if (context == nullptr || !audio_source_is_open(&context->backend) ||
        context->stream == nullptr || context->read_buffer == nullptr) {
        if (context != nullptr) {
            context->io_error = true;
            if (context->done != nullptr) {
                xSemaphoreGive(context->done);
            }
        }
        vTaskDelete(nullptr);
        return;
    }

    context->core_id = xPortGetCoreID();
    if (context->core_id != BUFFERED_SD_TASK_CORE) {
        context->io_error = true;
        ESP_LOGE(TAG, "预读任务核心绑定异常：当前=%d，期望=%d",
            static_cast<int>(context->core_id),
            static_cast<int>(BUFFERED_SD_TASK_CORE));
        if (context->done != nullptr) {
            xSemaphoreGive(context->done);
        }
        vTaskDelete(nullptr);
        return;
    }

#if APP_DIAG_AUDIO_SOURCE
    ESP_LOGI(TAG, "顺序预读任务已启动：核心=%d，优先级=%u，ring=%uKB，SD块=%uB",
        static_cast<int>(context->core_id),
        static_cast<unsigned>(uxTaskPriorityGet(nullptr)),
        static_cast<unsigned>(BUFFERED_SD_RING_BYTES / 1024U),
        static_cast<unsigned>(BUFFERED_SD_READ_CHUNK_BYTES));
#endif

    uint32_t cooperative_read_count = 0;
    while (!context->stop_requested) {
        const size_t free_bytes = xStreamBufferSpacesAvailable(context->stream);
        if (free_bytes < BUFFERED_SD_READ_CHUNK_BYTES) {
            cooperative_read_count = 0;
            vTaskDelay(BUFFERED_SD_COOPERATIVE_BLOCK_TICKS);
            continue;
        }

        size_t bytes_read = 0;
        const esp_err_t read_ret = audio_source_read(
            &context->backend,
            context->read_buffer,
            BUFFERED_SD_READ_CHUNK_BYTES,
            &bytes_read);

        if (bytes_read > 0) {
            size_t sent = 0;
            while (sent < bytes_read && !context->stop_requested) {
                const size_t chunk = xStreamBufferSend(
                    context->stream,
                    context->read_buffer + sent,
                    bytes_read - sent,
                    BUFFERED_SD_SEND_WAIT);
                sent += chunk;
            }

            ++cooperative_read_count;
            if (cooperative_read_count >= BUFFERED_SD_COOPERATIVE_READ_BATCH) {
                cooperative_read_count = 0;
                vTaskDelay(BUFFERED_SD_COOPERATIVE_BLOCK_TICKS);
            }
        }

        if (read_ret != ESP_OK) {
            if (!context->stop_requested) {
                context->io_error = true;
                ESP_LOGE(TAG, "顺序预读失败：ret=%s", esp_err_to_name(read_ret));
            }
            break;
        }
        if (bytes_read < BUFFERED_SD_READ_CHUNK_BYTES) {
            if (audio_source_eof(&context->backend)) {
                context->eof = true;
            } else if (!context->stop_requested) {
                context->io_error = true;
                ESP_LOGE(TAG, "顺序预读发生非 EOF 短读");
            }
            break;
        }
    }

    if (context->done != nullptr) {
        xSemaphoreGive(context->done);
    }
    vTaskDelete(nullptr);
}

static esp_err_t buffered_sd_read(void *raw_context, void *buffer, size_t bytes, size_t *out_bytes)
{
    BufferedSdContext *context = buffered_context(raw_context);
    if (out_bytes != nullptr) {
        *out_bytes = 0;
    }
    if (context == nullptr || context->stream == nullptr || buffer == nullptr || out_bytes == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes == 0) {
        return ESP_OK;
    }

    size_t total = 0;
    while (total < bytes) {
        const size_t received = xStreamBufferReceive(
            context->stream,
            static_cast<uint8_t *>(buffer) + total,
            bytes - total,
            BUFFERED_SD_RECEIVE_WAIT);
        if (received > 0) {
            total += received;
            continue;
        }

        if (context->io_error) {
            *out_bytes = total;
            return ESP_FAIL;
        }
        if (context->eof) {
            break;
        }

        // 正常播放只允许有界等待预读数据；不能重新退化成 AudioTask 无限等待 SD。
        *out_bytes = total;
        return ESP_ERR_TIMEOUT;
    }

    *out_bytes = total;
    return ESP_OK;
}

static esp_err_t buffered_sd_size(void *raw_context, uint64_t *out_size)
{
    BufferedSdContext *context = buffered_context(raw_context);
    if (context == nullptr || out_size == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_size = context->size_bytes;
    return ESP_OK;
}

static bool buffered_sd_eof(void *raw_context)
{
    BufferedSdContext *context = buffered_context(raw_context);
    if (context == nullptr || context->stream == nullptr) {
        return false;
    }
    return context->eof && xStreamBufferBytesAvailable(context->stream) == 0;
}

static void buffered_sd_destroy_context(BufferedSdContext *context)
{
    if (context == nullptr) {
        return;
    }

    context->stop_requested = true;
    if (context->task != nullptr && context->done != nullptr) {
        if (xSemaphoreTake(context->done, BUFFERED_SD_STOP_WARN_WAIT) != pdTRUE) {
            // 预读任务可能正在持有全局 SD 递归锁执行 fread。外部强删任务不会执行
            // StorageSdLockGuard 析构，可能把全局 SD 锁永久留在已删除任务名下。
            // 超过正常退出窗口后只告警，并继续等待任务自行离开 I/O 临界区。
            ESP_LOGW(TAG, "顺序预读任务退出超过1000ms；继续等待安全退出，禁止在SD I/O中强删任务");
            (void)xSemaphoreTake(context->done, portMAX_DELAY);
        }
        context->task = nullptr;
    }

    if (context->stream != nullptr) {
        vStreamBufferDelete(context->stream);
        context->stream = nullptr;
    }
    if (context->ring_storage != nullptr) {
        heap_caps_free(context->ring_storage);
        context->ring_storage = nullptr;
    }
    if (context->read_buffer != nullptr) {
        heap_caps_free(context->read_buffer);
        context->read_buffer = nullptr;
    }
    if (audio_source_is_open(&context->backend)) {
        (void)audio_source_close(&context->backend);
    }
    heap_caps_free(context);
}

static esp_err_t buffered_sd_close(void *raw_context)
{
    BufferedSdContext *context = buffered_context(raw_context);
    if (context == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    buffered_sd_destroy_context(context);
    return ESP_OK;
}

static const char *buffered_sd_name(void *)
{
    return "BUFFERED_SD";
}

static const AudioSourceOps BUFFERED_SD_OPS = {
    buffered_sd_read,
    nullptr,
    nullptr,
    buffered_sd_size,
    buffered_sd_eof,
    buffered_sd_close,
    buffered_sd_name,
};

esp_err_t buffered_sd_audio_source_open(
    AudioSource *out_source,
    const char *path,
    uint64_t start_offset)
{
    if (out_source == nullptr || path == nullptr || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    (void)audio_source_close(out_source);
    BufferedSdContext *context = static_cast<BufferedSdContext *>(
        heap_caps_calloc(1, sizeof(BufferedSdContext), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (context == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = sd_file_audio_source_open(
        &context->backend,
        &context->backend_storage,
        path);
    if (ret != ESP_OK) {
        buffered_sd_destroy_context(context);
        return ret;
    }

    ret = audio_source_size(&context->backend, &context->size_bytes);
    if (ret != ESP_OK || start_offset > context->size_bytes || start_offset > static_cast<uint64_t>(INT64_MAX)) {
        buffered_sd_destroy_context(context);
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }
    ret = audio_source_seek(
        &context->backend,
        static_cast<int64_t>(start_offset),
        AudioSourceSeekOrigin::Begin);
    if (ret != ESP_OK) {
        buffered_sd_destroy_context(context);
        return ret;
    }
    context->ring_storage = static_cast<uint8_t *>(heap_caps_malloc(
        BUFFERED_SD_RING_BYTES + 1U,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    context->read_buffer = static_cast<uint8_t *>(heap_caps_malloc(
        BUFFERED_SD_READ_CHUNK_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (context->ring_storage == nullptr || context->read_buffer == nullptr) {
        ESP_LOGE(TAG, "顺序预读 PSRAM 分配失败：ring=%uKB read=%uB",
            static_cast<unsigned>(BUFFERED_SD_RING_BYTES / 1024U),
            static_cast<unsigned>(BUFFERED_SD_READ_CHUNK_BYTES));
        buffered_sd_destroy_context(context);
        return ESP_ERR_NO_MEM;
    }

    context->stream = xStreamBufferCreateStatic(
        BUFFERED_SD_RING_BYTES + 1U,
        1,
        context->ring_storage,
        &context->stream_storage);
    context->done = xSemaphoreCreateBinaryStatic(&context->done_storage);
    if (context->stream == nullptr || context->done == nullptr) {
        buffered_sd_destroy_context(context);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t task_ret = xTaskCreatePinnedToCore(
        buffered_sd_task,
        "AudioReadAhead",
        BUFFERED_SD_TASK_STACK_BYTES,
        context,
        BUFFERED_SD_TASK_PRIORITY,
        &context->task,
        BUFFERED_SD_TASK_CORE);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "创建顺序预读任务失败");
        buffered_sd_destroy_context(context);
        return ESP_ERR_NO_MEM;
    }

    const uint64_t remaining = context->size_bytes - start_offset;
    size_t target = BUFFERED_SD_START_TARGET_BYTES;
    if (remaining < target) {
        target = static_cast<size_t>(remaining);
    }
    const size_t minimum = remaining < BUFFERED_SD_READ_CHUNK_BYTES
        ? static_cast<size_t>(remaining)
        : BUFFERED_SD_READ_CHUNK_BYTES;

    const TickType_t start_tick = xTaskGetTickCount();
    while (xStreamBufferBytesAvailable(context->stream) < target &&
           !context->eof && !context->io_error &&
           xTaskGetTickCount() - start_tick < BUFFERED_SD_START_WAIT) {
        vTaskDelay(BUFFERED_SD_COOPERATIVE_BLOCK_TICKS);
    }

    const size_t primed = xStreamBufferBytesAvailable(context->stream);
    if (context->io_error || primed < minimum) {
        const bool io_error = context->io_error;
        ESP_LOGE(TAG, "顺序预读启动失败：已缓存=%uB，最低需要=%uB",
            static_cast<unsigned>(primed),
            static_cast<unsigned>(minimum));
        buffered_sd_destroy_context(context);
        return io_error ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }

    out_source->ops = &BUFFERED_SD_OPS;
    out_source->context = context;
    out_source->capabilities =
        AUDIO_SOURCE_CAP_READ |
        AUDIO_SOURCE_CAP_SIZE |
        AUDIO_SOURCE_CAP_EOF;
    out_source->stats = {};

#if APP_DIAG_AUDIO_SOURCE
    ESP_LOGI(TAG,
        "SOURCE_TRACE: OPEN type=BUFFERED_SD start=%llu size=%llu primed=%u",
        static_cast<unsigned long long>(start_offset),
        static_cast<unsigned long long>(context->size_bytes),
        static_cast<unsigned>(primed));
#endif
    return ESP_OK;
}
