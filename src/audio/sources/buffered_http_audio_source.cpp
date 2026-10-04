#include "buffered_http_audio_source.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "NAS音频";

namespace {

static constexpr size_t kRingBytes = 128U * 1024U;
static constexpr size_t kReadChunkBytes = 8U * 1024U;
static constexpr size_t kStartTargetBytes = 64U * 1024U;
static constexpr uint32_t kTaskStackBytes = 4096U;
static constexpr UBaseType_t kTaskPriority = 4U;
static constexpr BaseType_t kTaskCore = 1;
static constexpr TickType_t kReadWait = pdMS_TO_TICKS(500);
static constexpr TickType_t kOpenWait = pdMS_TO_TICKS(12000);
static constexpr TickType_t kStopWarnWait = pdMS_TO_TICKS(1500);
static constexpr int kHttpTimeoutMs = 5000;
static constexpr size_t kHttpRxBufferBytes = 1024U;
static constexpr size_t kHttpTxBufferBytes = 256U;

struct BufferedHttpContext
{
    StreamBufferHandle_t stream = nullptr;
    StaticStreamBuffer_t stream_storage = {};
    StaticSemaphore_t ready_storage = {};
    StaticSemaphore_t done_storage = {};
    SemaphoreHandle_t ready = nullptr;
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t task = nullptr;

    uint8_t *ring_storage = nullptr;
    uint8_t *read_buffer = nullptr;
    char *url = nullptr;
    char *username = nullptr;
    char *password = nullptr;

    volatile bool stop_requested = false;
    volatile bool ready_signaled = false;
    volatile bool eof = false;
    volatile bool io_error = false;
    volatile esp_err_t io_error_code = ESP_OK;
    volatile uint64_t network_bytes = 0ULL;
    volatile uint64_t content_length = 0ULL;
    volatile uint32_t min_buffered_bytes = UINT32_MAX;
    volatile uint32_t task_stack_hwm = 0U;
};

extern const AudioSourceOps kOps;

static BufferedHttpContext *context_from_raw(void *raw)
{
    return static_cast<BufferedHttpContext *>(raw);
}

static void signal_ready(BufferedHttpContext *context)
{
    if (context == nullptr || context->ready_signaled) return;
    context->ready_signaled = true;
    if (context->ready != nullptr) xSemaphoreGive(context->ready);
}

static void set_error(BufferedHttpContext *context, esp_err_t error)
{
    if (context == nullptr) return;
    context->io_error = true;
    context->io_error_code = error != ESP_OK ? error : ESP_FAIL;
    signal_ready(context);
}

static char *dup_psram(const char *text)
{
    if (text == nullptr) return nullptr;
    const size_t length = strlen(text);
    char *copy = static_cast<char *>(heap_caps_malloc(
        length + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy != nullptr) memcpy(copy, text, length + 1U);
    return copy;
}

static void http_prefetch_task(void *arg)
{
    BufferedHttpContext *context = static_cast<BufferedHttpContext *>(arg);
    esp_http_client_handle_t client = nullptr;
    if (context == nullptr || context->stream == nullptr || context->read_buffer == nullptr ||
        context->url == nullptr) {
        set_error(context, ESP_ERR_INVALID_STATE);
        if (context != nullptr && context->done != nullptr) xSemaphoreGive(context->done);
        vTaskDelete(nullptr);
        return;
    }

    esp_http_client_config_t config = {};
    config.url = context->url;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = kHttpRxBufferBytes;
    config.buffer_size_tx = kHttpTxBufferBytes;
    config.keep_alive_enable = false;
    if (context->username != nullptr && context->username[0] != '\0') {
        config.username = context->username;
        config.password = context->password != nullptr ? context->password : "";
        config.auth_type = HTTP_AUTH_TYPE_BASIC;
    }

    client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_error(context, ESP_ERR_NO_MEM);
        goto finish;
    }

    {
        esp_err_t ret = esp_http_client_open(client, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "HTTP连接失败：%s", esp_err_to_name(ret));
            set_error(context, ret);
            goto finish;
        }
        const int64_t header_length = esp_http_client_fetch_headers(client);
        if (header_length < 0) {
            ESP_LOGE(TAG, "HTTP响应头读取失败");
            set_error(context, ESP_ERR_HTTP_CONNECT);
            goto finish;
        }
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "HTTP状态异常：status=%d", status);
            set_error(context, ESP_ERR_HTTP_CONNECT);
            goto finish;
        }
        const int64_t content_length = esp_http_client_get_content_length(client);
        if (content_length > 0) {
            context->content_length = static_cast<uint64_t>(content_length);
        }
        ESP_LOGI(TAG, "NAS MP3 HTTP已连接：content=%lldB ring=%uKB start=%uKB",
            static_cast<long long>(content_length),
            static_cast<unsigned>(kRingBytes / 1024U),
            static_cast<unsigned>(kStartTargetBytes / 1024U));
    }

    while (!context->stop_requested) {
        if (xStreamBufferSpacesAvailable(context->stream) < kReadChunkBytes) {
            vTaskDelay(1);
            continue;
        }

        const int got = esp_http_client_read(
            client, reinterpret_cast<char *>(context->read_buffer), kReadChunkBytes);
        if (got < 0) {
            set_error(context, ESP_FAIL);
            break;
        }
        if (got == 0) {
            const bool complete = esp_http_client_is_complete_data_received(client) ||
                (context->content_length > 0ULL && context->network_bytes >= context->content_length);
            if (complete) {
                context->eof = true;
                signal_ready(context);
            } else if (!context->stop_requested) {
                set_error(context, ESP_ERR_INVALID_RESPONSE);
            }
            break;
        }

        size_t sent = 0U;
        while (sent < static_cast<size_t>(got) && !context->stop_requested) {
            sent += xStreamBufferSend(
                context->stream,
                context->read_buffer + sent,
                static_cast<size_t>(got) - sent,
                pdMS_TO_TICKS(50));
        }
        context->network_bytes += sent;

        const size_t buffered = xStreamBufferBytesAvailable(context->stream);
        if (buffered < context->min_buffered_bytes) {
            context->min_buffered_bytes = static_cast<uint32_t>(buffered);
        }
        if (buffered >= kStartTargetBytes) signal_ready(context);
        taskYIELD();
    }

finish:
    context->task_stack_hwm = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    signal_ready(context);
    if (client != nullptr) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (context->done != nullptr) xSemaphoreGive(context->done);
    vTaskDelete(nullptr);
}

static esp_err_t source_read(void *raw, void *buffer, size_t bytes, size_t *out_bytes)
{
    BufferedHttpContext *context = context_from_raw(raw);
    if (out_bytes != nullptr) *out_bytes = 0U;
    if (context == nullptr || context->stream == nullptr || buffer == nullptr || out_bytes == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes == 0U) return ESP_OK;

    size_t total = 0U;
    while (total < bytes) {
        const size_t received = xStreamBufferReceive(
            context->stream,
            static_cast<uint8_t *>(buffer) + total,
            bytes - total,
            kReadWait);
        if (received > 0U) {
            total += received;
            const size_t buffered = xStreamBufferBytesAvailable(context->stream);
            if (buffered < context->min_buffered_bytes) {
                context->min_buffered_bytes = static_cast<uint32_t>(buffered);
            }
            continue;
        }
        if (context->io_error) {
            *out_bytes = total;
            return total > 0U ? ESP_OK :
                (context->io_error_code != ESP_OK ? context->io_error_code : ESP_FAIL);
        }
        if (context->eof) break;
        // 网络流允许把已经拿到的部分数据先交给 MP3 parser；只有完全无数据时才报告欠载超时。
        // 这样一次小抖动不会因为“未凑满整个 8KB 窗口”被误判成播放故障。
        *out_bytes = total;
        return total > 0U ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    *out_bytes = total;
    return ESP_OK;
}

static bool source_eof(void *raw)
{
    BufferedHttpContext *context = context_from_raw(raw);
    return context != nullptr && context->stream != nullptr && context->eof &&
        xStreamBufferBytesAvailable(context->stream) == 0U;
}

static void destroy_context(BufferedHttpContext *context)
{
    if (context == nullptr) return;
    context->stop_requested = true;
    if (context->task != nullptr && context->done != nullptr) {
        if (xSemaphoreTake(context->done, kStopWarnWait) != pdTRUE) {
            ESP_LOGW(TAG, "NAS HTTP预读退出超过1500ms，等待网络读安全返回");
            (void)xSemaphoreTake(context->done, portMAX_DELAY);
        }
        context->task = nullptr;
    }

    const size_t buffered = context->stream != nullptr
        ? xStreamBufferBytesAvailable(context->stream) : 0U;
    ESP_LOGI(TAG,
        "NAS MP3 HTTP关闭：download=%lluB buffered=%uB min=%uB stack_hwm=%u eof=%u ioerr=%u",
        static_cast<unsigned long long>(context->network_bytes),
        static_cast<unsigned>(buffered),
        static_cast<unsigned>(context->min_buffered_bytes == UINT32_MAX ? 0U : context->min_buffered_bytes),
        static_cast<unsigned>(context->task_stack_hwm),
        context->eof ? 1U : 0U,
        context->io_error ? 1U : 0U);

    if (context->stream != nullptr) vStreamBufferDelete(context->stream);
    if (context->ring_storage != nullptr) heap_caps_free(context->ring_storage);
    if (context->read_buffer != nullptr) heap_caps_free(context->read_buffer);
    if (context->url != nullptr) heap_caps_free(context->url);
    if (context->username != nullptr) heap_caps_free(context->username);
    if (context->password != nullptr) heap_caps_free(context->password);
    heap_caps_free(context);
}

static esp_err_t source_close(void *raw)
{
    BufferedHttpContext *context = context_from_raw(raw);
    if (context == nullptr) return ESP_ERR_INVALID_ARG;
    destroy_context(context);
    return ESP_OK;
}

static const char *source_name(void *)
{
    return "HTTP_MP3";
}

const AudioSourceOps kOps = {
    source_read,
    nullptr,
    nullptr,
    nullptr,
    source_eof,
    source_close,
    source_name,
};

} // namespace

esp_err_t buffered_http_audio_source_open(
    AudioSource *out_source,
    const char *url,
    const char *username,
    const char *password)
{
    if (out_source == nullptr || url == nullptr || strncmp(url, "http://", 7U) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)audio_source_close(out_source);

    BufferedHttpContext *context = static_cast<BufferedHttpContext *>(heap_caps_calloc(
        1, sizeof(BufferedHttpContext), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (context == nullptr) return ESP_ERR_NO_MEM;
    context->min_buffered_bytes = UINT32_MAX;

    context->ring_storage = static_cast<uint8_t *>(heap_caps_malloc(
        kRingBytes + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    context->read_buffer = static_cast<uint8_t *>(heap_caps_malloc(
        kReadChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    context->url = dup_psram(url);
    context->username = dup_psram(username != nullptr ? username : "");
    context->password = dup_psram(password != nullptr ? password : "");
    if (context->ring_storage == nullptr || context->read_buffer == nullptr || context->url == nullptr ||
        context->username == nullptr || context->password == nullptr) {
        destroy_context(context);
        return ESP_ERR_NO_MEM;
    }

    context->stream = xStreamBufferCreateStatic(
        kRingBytes + 1U, 1U, context->ring_storage, &context->stream_storage);
    context->ready = xSemaphoreCreateBinaryStatic(&context->ready_storage);
    context->done = xSemaphoreCreateBinaryStatic(&context->done_storage);
    if (context->stream == nullptr || context->ready == nullptr || context->done == nullptr) {
        destroy_context(context);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        http_prefetch_task,
        "NasMp3Http",
        kTaskStackBytes,
        context,
        kTaskPriority,
        &context->task,
        kTaskCore);
    if (created != pdPASS) {
        destroy_context(context);
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(context->ready, kOpenWait) != pdTRUE) {
        ESP_LOGE(TAG, "NAS MP3 HTTP启动超时：%ums", static_cast<unsigned>(kOpenWait * portTICK_PERIOD_MS));
        destroy_context(context);
        return ESP_ERR_TIMEOUT;
    }

    const size_t primed = xStreamBufferBytesAvailable(context->stream);
    const size_t minimum = context->eof
        ? static_cast<size_t>(context->network_bytes)
        : kReadChunkBytes;
    if (context->io_error || primed < minimum) {
        const esp_err_t error = context->io_error && context->io_error_code != ESP_OK
            ? context->io_error_code : ESP_ERR_TIMEOUT;
        ESP_LOGE(TAG, "NAS MP3 HTTP预取启动失败：primed=%uB min=%uB ret=%s",
            static_cast<unsigned>(primed), static_cast<unsigned>(minimum), esp_err_to_name(error));
        destroy_context(context);
        return error;
    }

    context->min_buffered_bytes = static_cast<uint32_t>(primed);
    out_source->ops = &kOps;
    out_source->context = context;
    out_source->capabilities = AUDIO_SOURCE_CAP_READ | AUDIO_SOURCE_CAP_EOF | AUDIO_SOURCE_CAP_STREAMING;
    out_source->stats = {};
    ESP_LOGI(TAG, "NAS MP3 HTTP预取就绪：primed=%uB ring=%uKB task_stack=%uB",
        static_cast<unsigned>(primed),
        static_cast<unsigned>(kRingBytes / 1024U),
        static_cast<unsigned>(kTaskStackBytes));
    return ESP_OK;
}

bool buffered_http_audio_source_get_stats(
    const AudioSource *source,
    BufferedHttpAudioSourceStats *out_stats)
{
    if (source == nullptr || out_stats == nullptr || source->ops != &kOps || source->context == nullptr) {
        return false;
    }
    BufferedHttpContext *context = static_cast<BufferedHttpContext *>(source->context);
    BufferedHttpAudioSourceStats stats = {};
    stats.ring_capacity_bytes = kRingBytes;
    stats.ring_buffered_bytes = context->stream != nullptr
        ? static_cast<uint32_t>(xStreamBufferBytesAvailable(context->stream)) : 0U;
    stats.ring_min_buffered_bytes = context->min_buffered_bytes == UINT32_MAX
        ? 0U : context->min_buffered_bytes;
    stats.network_bytes = context->network_bytes;
    stats.task_stack_hwm = context->task_stack_hwm;
    stats.eof = context->eof;
    stats.io_error = context->io_error;
    stats.io_error_code = context->io_error_code;
    *out_stats = stats;
    return true;
}
