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

static constexpr size_t kReadChunkBytes = 8U * 1024U;
// R46.0.85：统一 NAS Stream Worker 固定一份 Internal stack。
// HTTP connect/retry/read/EOF 全部只在此任务执行；为后续 FLAC 共用保留 4KB，不继续沿用 MP3-only 3072B trial。
static constexpr uint32_t kTaskStackBytes = 4096U;
static constexpr UBaseType_t kTaskPriority = 4U;
static constexpr BaseType_t kTaskCore = 1;
static constexpr TickType_t kReadWait = pdMS_TO_TICKS(20);
static constexpr TickType_t kOpenWait = pdMS_TO_TICKS(10000);
static constexpr TickType_t kStopWarnWait = pdMS_TO_TICKS(1500);
// R46.0.95：连接/响应头单次上限收紧为 3s；新 transport intent 会在连接检查点取消旧重试。
// 进入连续音频体后单次 read 仍保持 1s，避免网络阻塞拖住 AudioTask 的恢复/切歌。
static constexpr int kConnectHeaderTimeoutMs = 3000;
static constexpr int kStreamReadTimeoutMs = 1000;
static constexpr uint32_t kConnectAttempts = 2U;
static constexpr TickType_t kConnectRetryDelay = pdMS_TO_TICKS(250);
// R46.0.94/R46.0.95：RX 2KB覆盖较长响应头；Short-ID 后请求 URL 固定很短，TX 1KB
// 足够容纳请求行/Basic Auth 等头部，不再为多字节文件名无限扩大 Internal buffer。
static constexpr size_t kHttpRxBufferBytes = 2048U;
static constexpr size_t kHttpTxBufferBytes = 1024U;

struct HttpStreamProfile
{
    const char *label = nullptr;
    size_t ring_bytes = 0U;
    size_t start_target_bytes = 0U;
};

static bool resolve_profile(BufferedHttpAudioProfile profile, HttpStreamProfile *out)
{
    if (out == nullptr) return false;
    switch (profile) {
        case BufferedHttpAudioProfile::Mp3:
            // MP3 parser 首块可能先消费较大的 ID3/内嵌封面；保留 R46.0.82 的 128KB/96KB 实测配置。
            *out = {"MP3", 128U * 1024U, 96U * 1024U};
            return true;
        case BufferedHttpAudioProfile::Flac:
            // R46.0.98：NAS 在 HTTP 建链前拿不到可靠采样率；所有 FLAC 固定使用高余量档，
            // 避免 96k/24bit 被误分到 192KB ring。只增加 PSRAM，不增加 Internal task stack。
            *out = {"FLAC", 384U * 1024U, 256U * 1024U};
            return true;
        default:
            return false;
    }
}

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
    const char *profile_label = nullptr;
    size_t ring_bytes = 0U;
    size_t start_target_bytes = 0U;
    char *url = nullptr;
    char *username = nullptr;
    char *password = nullptr;
    BufferedHttpAudioAbortFn abort_fn = nullptr;
    const void *abort_context = nullptr;

    volatile bool stop_requested = false;
    volatile bool ready_signaled = false;
    volatile bool eof = false;
    volatile bool io_error = false;
    volatile esp_err_t io_error_code = ESP_OK;
    volatile uint64_t network_bytes = 0ULL;
    volatile uint64_t content_length = 0ULL;
    volatile uint32_t min_buffered_bytes = UINT32_MAX;
    volatile uint32_t task_stack_hwm = 0U;
    volatile uint32_t read_timeout_total = 0U;
    volatile uint32_t read_timeout_streak = 0U;
    volatile uint32_t read_timeout_streak_max = 0U;
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

static bool open_should_abort(const BufferedHttpContext *context)
{
    return context != nullptr && (context->stop_requested ||
        (context->abort_fn != nullptr && context->abort_fn(context->abort_context)));
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

static size_t nas_stream_http_tx_bytes(const BufferedHttpContext *context)
{
    if (context == nullptr || context->url == nullptr) return 0U;
    // The old /track/<short-id> fit in 1KB.  Encoded WebDAV paths can be longer.
    // Only enlarge the HTTP client's temporary INTERNAL TX buffer when required;
    // reject pathological paths instead of silently truncating a GET request.
    size_t needed = strlen(context->url) + 320U;
    if (context->username != nullptr && context->username[0] != '\0') {
        const size_t user_bytes = strlen(context->username);
        const size_t pass_bytes = context->password != nullptr ? strlen(context->password) : 0U;
        needed += ((user_bytes + pass_bytes + 3U) / 3U) * 4U + 32U;
    }
    if (needed > 4096U) return 0U;
    if (needed <= kHttpTxBufferBytes) return kHttpTxBufferBytes;
    return (needed + 255U) & ~static_cast<size_t>(255U);
}

static void nas_stream_task(void *arg)
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
    const size_t tx_bytes = nas_stream_http_tx_bytes(context);
    if (tx_bytes == 0U) {
        ESP_LOGW(TAG, "NAS URL/认证请求头过长，拒绝建立连接：url=%uB",
            static_cast<unsigned>(strlen(context->url)));
        set_error(context, ESP_ERR_INVALID_SIZE);
        goto finish;
    }
    config.url = context->url;
    config.timeout_ms = kConnectHeaderTimeoutMs;
    config.buffer_size = kHttpRxBufferBytes;
    config.buffer_size_tx = tx_bytes;
    config.keep_alive_enable = false;
    if (context->username != nullptr && context->username[0] != '\0') {
        config.username = context->username;
        config.password = context->password != nullptr ? context->password : "";
        config.auth_type = HTTP_AUTH_TYPE_BASIC;
    }

    {
        esp_err_t ret = ESP_FAIL;
        for (uint32_t attempt = 1U; attempt <= kConnectAttempts && !context->stop_requested; ++attempt) {
            if (open_should_abort(context)) {
                set_error(context, ESP_ERR_INVALID_STATE);
                goto finish;
            }
            client = esp_http_client_init(&config);
            if (client == nullptr) {
                set_error(context, ESP_ERR_NO_MEM);
                goto finish;
            }

            ret = esp_http_client_open(client, 0);
            if (ret == ESP_OK) break;

            if (open_should_abort(context)) {
                ESP_LOGI(TAG, "NAS旧建链已被更新播放意图取消：attempt=%u/%u",
                    static_cast<unsigned>(attempt), static_cast<unsigned>(kConnectAttempts));
                esp_http_client_cleanup(client);
                client = nullptr;
                set_error(context, ESP_ERR_INVALID_STATE);
                goto finish;
            }

            if (attempt < kConnectAttempts) {
                ESP_LOGW(TAG,
                    "HTTP连接超时/失败：attempt=%u/%u ret=%s，%ums后仅重试一次",
                    static_cast<unsigned>(attempt),
                    static_cast<unsigned>(kConnectAttempts),
                    esp_err_to_name(ret),
                    static_cast<unsigned>(kConnectRetryDelay * portTICK_PERIOD_MS));
                esp_http_client_cleanup(client);
                client = nullptr;
                vTaskDelay(kConnectRetryDelay);
                continue;
            }

            ESP_LOGE(TAG, "HTTP连接失败：attempt=%u/%u ret=%s",
                static_cast<unsigned>(attempt),
                static_cast<unsigned>(kConnectAttempts),
                esp_err_to_name(ret));
            set_error(context, ret);
            goto finish;
        }

        if (open_should_abort(context) || client == nullptr || ret != ESP_OK) {
            set_error(context, open_should_abort(context) ? ESP_ERR_INVALID_STATE : ret);
            goto finish;
        }

        const int64_t header_length = esp_http_client_fetch_headers(client);
        if (open_should_abort(context)) {
            set_error(context, ESP_ERR_INVALID_STATE);
            goto finish;
        }
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

        // open/fetch_headers 需要较宽容的连接时间；真正持续播放后，单次 socket read 必须有界。
        // ESP-IDF 5.5 的 esp_http_client_read() 在“暂时无数据直到超时”时返回 -ESP_ERR_HTTP_EAGAIN。
        const esp_err_t timeout_ret = esp_http_client_set_timeout_ms(client, kStreamReadTimeoutMs);
        if (timeout_ret != ESP_OK) {
            ESP_LOGE(TAG, "NAS流读取超时配置失败：ret=%s", esp_err_to_name(timeout_ret));
            set_error(context, timeout_ret);
            goto finish;
        }

        ESP_LOGI(TAG,
            "NAS流已连接：profile=%s content=%lldB ring=%uKB start=%uKB url=%uB http_rx=%uB http_tx=%uB read_timeout=%dms task=P%u/Core%d",
            context->profile_label != nullptr ? context->profile_label : "?",
            static_cast<long long>(content_length),
            static_cast<unsigned>(context->ring_bytes / 1024U),
            static_cast<unsigned>(context->start_target_bytes / 1024U),
            static_cast<unsigned>(strlen(context->url)),
            static_cast<unsigned>(kHttpRxBufferBytes),
            static_cast<unsigned>(tx_bytes),
            kStreamReadTimeoutMs,
            static_cast<unsigned>(kTaskPriority),
            static_cast<int>(kTaskCore));
    }

    while (!context->stop_requested) {
        if (xStreamBufferSpacesAvailable(context->stream) < kReadChunkBytes) {
            vTaskDelay(1);
            continue;
        }

        const int got = esp_http_client_read(
            client, reinterpret_cast<char *>(context->read_buffer), kReadChunkBytes);
        if (got == -ESP_ERR_HTTP_EAGAIN) {
            const uint32_t read_timeout_total = context->read_timeout_total + 1U;
            const uint32_t read_timeout_streak = context->read_timeout_streak + 1U;
            context->read_timeout_total = read_timeout_total;
            context->read_timeout_streak = read_timeout_streak;
            if (read_timeout_streak > context->read_timeout_streak_max) {
                context->read_timeout_streak_max = read_timeout_streak;
            }
            if (read_timeout_streak == 1U) {
                ESP_LOGW(TAG,
                    "NAS HTTP读暂时无数据：timeout=%dms buffered=%uB，保持同一连接等待恢复",
                    kStreamReadTimeoutMs,
                    static_cast<unsigned>(xStreamBufferBytesAvailable(context->stream)));
            }
            continue;
        }
        if (got < 0) {
            ESP_LOGE(TAG, "NAS HTTP读取失败：ret=%d", got);
            set_error(context, ESP_ERR_HTTP_CONNECT);
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

        if (context->read_timeout_streak > 0U) {
            ESP_LOGI(TAG,
                "NAS HTTP读已恢复：timeouts=%u total=%u buffered=%uB",
                static_cast<unsigned>(context->read_timeout_streak),
                static_cast<unsigned>(context->read_timeout_total),
                static_cast<unsigned>(xStreamBufferBytesAvailable(context->stream)));
            context->read_timeout_streak = 0U;
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
        if (buffered >= context->start_target_bytes) signal_ready(context);
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

    // Streaming source 只返回当前已经可用的一批数据，不为了凑满 decoder 的整个请求窗口。
    // Worker 不理解 MP3/FLAC 格式；真正 ring=0 后由对应 Codec/AudioTask 决定欠载策略。
    const size_t received = xStreamBufferReceive(
        context->stream,
        static_cast<uint8_t *>(buffer),
        bytes,
        kReadWait);
    if (received > 0U) {
        *out_bytes = received;
        const size_t buffered = xStreamBufferBytesAvailable(context->stream);
        if (buffered < context->min_buffered_bytes) {
            context->min_buffered_bytes = static_cast<uint32_t>(buffered);
        }
        return ESP_OK;
    }
    if (context->io_error) {
        return context->io_error_code != ESP_OK ? context->io_error_code : ESP_FAIL;
    }
    if (context->eof) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
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
        "NAS流关闭：profile=%s download=%lluB buffered=%uB min=%uB stack_hwm=%u read_timeout=%u max_streak=%u eof=%u ioerr=%u",
        context->profile_label != nullptr ? context->profile_label : "?",
        static_cast<unsigned long long>(context->network_bytes),
        static_cast<unsigned>(buffered),
        static_cast<unsigned>(context->min_buffered_bytes == UINT32_MAX ? 0U : context->min_buffered_bytes),
        static_cast<unsigned>(context->task_stack_hwm),
        static_cast<unsigned>(context->read_timeout_total),
        static_cast<unsigned>(context->read_timeout_streak_max),
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
    return "NAS_HTTP";
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
    BufferedHttpAudioProfile profile,
    const char *url,
    const char *username,
    const char *password,
    BufferedHttpAudioAbortFn abort_fn,
    const void *abort_context)
{
    HttpStreamProfile stream_profile = {};
    if (out_source == nullptr || url == nullptr || strncmp(url, "http://", 7U) != 0 ||
        !resolve_profile(profile, &stream_profile)) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)audio_source_close(out_source);

    BufferedHttpContext *context = static_cast<BufferedHttpContext *>(heap_caps_calloc(
        1, sizeof(BufferedHttpContext), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (context == nullptr) return ESP_ERR_NO_MEM;
    context->min_buffered_bytes = UINT32_MAX;
    context->profile_label = stream_profile.label;
    context->ring_bytes = stream_profile.ring_bytes;
    context->start_target_bytes = stream_profile.start_target_bytes;
    context->abort_fn = abort_fn;
    context->abort_context = abort_context;

    context->ring_storage = static_cast<uint8_t *>(heap_caps_malloc(
        context->ring_bytes + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
        context->ring_bytes + 1U, 1U, context->ring_storage, &context->stream_storage);
    context->ready = xSemaphoreCreateBinaryStatic(&context->ready_storage);
    context->done = xSemaphoreCreateBinaryStatic(&context->done_storage);
    if (context->stream == nullptr || context->ready == nullptr || context->done == nullptr) {
        destroy_context(context);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        nas_stream_task,
        "NasStream",
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
        ESP_LOGE(TAG, "NAS流启动超时：profile=%s wait=%ums",
            context->profile_label != nullptr ? context->profile_label : "?", static_cast<unsigned>(kOpenWait * portTICK_PERIOD_MS));
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
        ESP_LOGE(TAG, "NAS流预取启动失败：profile=%s primed=%uB min=%uB ret=%s",
            context->profile_label != nullptr ? context->profile_label : "?",
            static_cast<unsigned>(primed), static_cast<unsigned>(minimum), esp_err_to_name(error));
        destroy_context(context);
        return error;
    }

    context->min_buffered_bytes = static_cast<uint32_t>(primed);
    out_source->ops = &kOps;
    out_source->context = context;
    out_source->capabilities = AUDIO_SOURCE_CAP_READ | AUDIO_SOURCE_CAP_EOF | AUDIO_SOURCE_CAP_STREAMING;
    out_source->stats = {};
    ESP_LOGI(TAG, "NAS流预取就绪：profile=%s primed=%uB ring=%uKB task_stack=%uB priority=%u core=%d",
        context->profile_label != nullptr ? context->profile_label : "?",
        static_cast<unsigned>(primed),
        static_cast<unsigned>(context->ring_bytes / 1024U),
        static_cast<unsigned>(kTaskStackBytes),
        static_cast<unsigned>(kTaskPriority),
        static_cast<int>(kTaskCore));
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
    stats.ring_capacity_bytes = static_cast<uint32_t>(context->ring_bytes);
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

bool buffered_http_audio_source_get_endpoint_view(
    const AudioSource *source,
    const char **out_url,
    const char **out_username,
    const char **out_password)
{
    if (source == nullptr || source->ops != &kOps || source->context == nullptr ||
        out_url == nullptr || out_username == nullptr || out_password == nullptr) {
        return false;
    }
    const BufferedHttpContext *context = static_cast<const BufferedHttpContext *>(source->context);
    if (context->url == nullptr || context->username == nullptr || context->password == nullptr) {
        return false;
    }
    *out_url = context->url;
    *out_username = context->username;
    *out_password = context->password;
    return true;
}
