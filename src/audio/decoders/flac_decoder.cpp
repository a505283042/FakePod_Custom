#include "flac_decoder.h"

#include <limits.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_types.h"
#include "esp_flac_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "../audio_rate_profile.h"

static const char *TAG = "FLAC";

// Simple Decoder 每次最多取 32KB 连续压缩数据，输入工作区与 PCM 输出区都放在 PSRAM。
// SD 读取不再由 AudioTask 同步执行，而是由独立低优先级预取任务填充 PSRAM 环形缓冲。
// 预取缓存按采样率扩展，未来开放 96/192kHz 时优先增加 PSRAM 吞吐余量，不堆内部 DMA RAM。
static constexpr size_t FLAC_INPUT_BUFFER_BYTES = 32768;
// 在 Simple Decoder parser 模式下，尽量让下一完整 FLAC 压缩帧连续落在同一个输入窗口中。
// STREAMINFO 的 max_frame_size 再留少量保护字节，避免窗口边界导致一次 PCM refill 需要两轮 process。
static constexpr size_t FLAC_INPUT_FRAME_GUARD_BYTES = 64;
static constexpr size_t FLAC_PREFETCH_RING_48K_BYTES = 128 * 1024;
static constexpr size_t FLAC_PREFETCH_RING_96K_BYTES = 192 * 1024;
static constexpr size_t FLAC_PREFETCH_RING_192K_BYTES = 256 * 1024;
static constexpr size_t FLAC_PREFETCH_READ_48K_BYTES = 8192;
static constexpr size_t FLAC_PREFETCH_READ_96K_BYTES = 16384;
static constexpr size_t FLAC_PREFETCH_READ_192K_BYTES = 32768;
static constexpr size_t FLAC_PREFETCH_START_48K_BYTES = 64 * 1024;
static constexpr size_t FLAC_PREFETCH_START_96K_BYTES = 96 * 1024;
// 176.4/192k 长测中 256KB ring 最低仍约 198KB、starve=0，128KB 起播预充偏保守。
// Stage 9.5.4 将高采样率起播目标降到 64KB，预计减少约两次 32KB SD 读取的等待；
// 稳态 ring、SD read chunk 和解码窗口均保持不变。
static constexpr size_t FLAC_PREFETCH_START_192K_BYTES = 64 * 1024;
static constexpr uint32_t FLAC_PREFETCH_TASK_STACK_BYTES = 4096;
static constexpr UBaseType_t FLAC_PREFETCH_TASK_PRIORITY = 4;
static constexpr BaseType_t FLAC_PREFETCH_TASK_CORE = 1;
static constexpr TickType_t FLAC_PREFETCH_SEND_WAIT = pdMS_TO_TICKS(20);
static constexpr TickType_t FLAC_PREFETCH_RECEIVE_WAIT = pdMS_TO_TICKS(20);
// 首块 PCM 预解码发生在 I2S 启动前，不受实时播放预算约束。
// 允许等待完整一次较慢的 SD 读取，避免较大的 Vorbis Comment/PICTURE 元数据
// 在 64KB 起播预充后把 ring 短暂耗空时直接判定失败。
static constexpr TickType_t FLAC_PREFETCH_STARTUP_RECEIVE_WAIT = pdMS_TO_TICKS(100);
static constexpr TickType_t FLAC_PREFETCH_START_WAIT = pdMS_TO_TICKS(750);
static constexpr TickType_t FLAC_PREFETCH_STOP_WAIT = pdMS_TO_TICKS(1000);
static constexpr size_t FLAC_MIN_DECODED_BUFFER_BYTES = 16384;
static constexpr size_t FLAC_MAX_DECODED_BUFFER_BYTES = 1024 * 1024;

struct FlacPrefetchProfile
{
    size_t ring_bytes = FLAC_PREFETCH_RING_48K_BYTES;
    size_t read_chunk_bytes = FLAC_PREFETCH_READ_48K_BYTES;
    size_t start_target_bytes = FLAC_PREFETCH_START_48K_BYTES;
};

static constexpr size_t FLAC_STREAMINFO_BYTES = 34;
static constexpr size_t FLAC_SYNTHETIC_HEADER_BYTES = 4 + 4 + FLAC_STREAMINFO_BYTES;
static constexpr size_t FLAC_SEEKPOINT_BYTES = 18;
static constexpr uint64_t FLAC_SEEKPOINT_PLACEHOLDER = UINT64_MAX;

struct FlacStreamDescriptor
{
    uint64_t file_size_bytes = 0;
    uint64_t flac_offset_bytes = 0;
    uint64_t audio_data_offset_bytes = 0;
    uint64_t seektable_offset_bytes = 0;
    uint32_t seektable_length_bytes = 0;
    uint8_t streaminfo_payload[FLAC_STREAMINFO_BYTES] = {};

    uint16_t max_block_size = 0;
    uint32_t min_frame_size = 0;
    uint32_t max_frame_size = 0;
    uint32_t sample_rate_hz = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint64_t total_frames = 0;
};

struct FlacSeekPoint
{
    uint64_t sample_number = 0;
    uint64_t stream_offset = 0;
    uint16_t frame_samples = 0;
};

static FlacPrefetchProfile flac_prefetch_profile_for_rate(uint32_t sample_rate_hz)
{
    if (sample_rate_hz > 96000U) {
        return {
            FLAC_PREFETCH_RING_192K_BYTES,
            FLAC_PREFETCH_READ_192K_BYTES,
            FLAC_PREFETCH_START_192K_BYTES
        };
    }
    if (sample_rate_hz > 48000U) {
        return {
            FLAC_PREFETCH_RING_96K_BYTES,
            FLAC_PREFETCH_READ_96K_BYTES,
            FLAC_PREFETCH_START_96K_BYTES
        };
    }
    return {};
}

struct FlacPrefetchContext
{
    AudioSource *source = nullptr;
    StreamBufferHandle_t stream = nullptr;
    StaticStreamBuffer_t stream_storage = {};
    StaticSemaphore_t done_storage = {};
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t task = nullptr;
    uint8_t *ring_storage = nullptr;
    uint8_t *read_buffer = nullptr;
    uint8_t prefix[FLAC_SYNTHETIC_HEADER_BYTES] = {};
    size_t prefix_size = 0;
    size_t prefix_offset = 0;
    size_t ring_bytes = 0;
    size_t read_chunk_bytes = 0;
    size_t start_target_bytes = 0;
    volatile BaseType_t core_id = -1;
    volatile bool stop_requested = false;
    volatile bool eof = false;
    volatile bool io_error = false;
#if APP_DIAG_FLAC_PERFORMANCE
    volatile UBaseType_t stack_hwm = 0;
    uint64_t perf_read_total_us = 0;
    uint32_t perf_read_calls = 0;
    uint32_t perf_read_max_us = 0;
    uint32_t perf_read_over_10ms = 0;
#endif
};

static bool g_flac_backend_registered = false;

#if APP_DIAG_FLAC_PERFORMANCE
static portMUX_TYPE g_flac_prefetch_metrics_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_flac_perf_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static FlacPerfSnapshot g_flac_perf_snapshot = {};

// FLAC 专项核查已通过，正式固件默认不编译这些计时/统计；需要性能回归时再打开。
static constexpr uint32_t FLAC_PERF_REPORT_INTERVAL_US = 5000000U;
static constexpr uint32_t FLAC_SLOW_READ_US = 10000U;
static constexpr uint32_t FLAC_SLOW_DECODE_US = 20000U;
static constexpr uint32_t FLAC_CRITICAL_DECODE_US = 40000U;
static constexpr uint32_t FLAC_SLOW_REFILL_US = 20000U;
static constexpr uint32_t FLAC_CRITICAL_REFILL_US = 30000U;
#endif

static FlacPrefetchContext *flac_prefetch_context(FlacDecoder *decoder)
{
    return decoder != nullptr
        ? static_cast<FlacPrefetchContext *>(decoder->prefetch_context)
        : nullptr;
}

#if APP_DIAG_FLAC_PERFORMANCE
static void flac_perf_reset_runtime(FlacDecoder *decoder)
{
    if (decoder == nullptr) {
        return;
    }

    FlacPrefetchContext *context = flac_prefetch_context(decoder);
    if (context != nullptr) {
        portENTER_CRITICAL(&g_flac_prefetch_metrics_mux);
        context->perf_read_total_us = 0;
        context->perf_read_calls = 0;
        context->perf_read_max_us = 0;
        context->perf_read_over_10ms = 0;
        portEXIT_CRITICAL(&g_flac_prefetch_metrics_mux);
    }

    decoder->perf_decode_total_us = 0;
    decoder->perf_refill_total_us = 0;
    decoder->perf_last_report_us = 0;
    decoder->perf_decode_calls = 0;
    decoder->perf_refill_calls = 0;
    decoder->perf_decode_max_us = 0;
    decoder->perf_refill_max_us = 0;
    decoder->perf_decode_over_20ms = 0;
    decoder->perf_decode_over_40ms = 0;
    decoder->perf_refill_over_20ms = 0;
    decoder->perf_refill_over_30ms = 0;
    decoder->perf_refill_over_block_budget = 0;
    decoder->perf_refill_worst_over_budget_us = 0;
    decoder->perf_refill_process_total = 0;
    decoder->perf_refill_process_max = 0;
    decoder->perf_refill_multi_process = 0;
    decoder->perf_refill_input_fill_max = 0;
    decoder->perf_refill_multi_fill = 0;
    decoder->perf_input_compact_calls = 0;
    decoder->perf_input_compact_bytes = 0;
    decoder->perf_input_topup_bytes = 0;
    decoder->perf_input_topup_calls = 0;
    decoder->perf_input_topup_max_bytes = 0;
    decoder->perf_prefetch_wait_total_us = 0;
    decoder->perf_prefetch_wait_calls = 0;
    decoder->perf_prefetch_wait_max_us = 0;
    decoder->perf_prefetch_wait_over_2ms = 0;
    decoder->perf_prefetch_starve_count = 0;
    const FlacPrefetchContext *prefetch = flac_prefetch_context(decoder);
    decoder->perf_prefetch_min_buffered_bytes = prefetch != nullptr && prefetch->stream != nullptr
        ? xStreamBufferBytesAvailable(prefetch->stream)
        : 0;
}

static void flac_perf_maybe_publish(FlacDecoder *decoder, uint64_t now_us)
{
    if (decoder == nullptr) {
        return;
    }

    if (decoder->perf_last_report_us == 0) {
        decoder->perf_last_report_us = now_us;
        return;
    }
    if (now_us - decoder->perf_last_report_us < FLAC_PERF_REPORT_INTERVAL_US) {
        return;
    }

    uint64_t read_total_us = 0;
    uint32_t read_calls = 0;
    uint32_t read_max_us = 0;
    uint32_t read_over_10ms = 0;
    const FlacPrefetchContext *context = flac_prefetch_context(decoder);
    if (context != nullptr) {
        portENTER_CRITICAL(&g_flac_prefetch_metrics_mux);
        read_total_us = context->perf_read_total_us;
        read_calls = context->perf_read_calls;
        read_max_us = context->perf_read_max_us;
        read_over_10ms = context->perf_read_over_10ms;
        portEXIT_CRITICAL(&g_flac_prefetch_metrics_mux);
    }

    FlacPerfSnapshot snapshot = {};
    snapshot.active = true;
    snapshot.sample_rate_hz = decoder->sample_rate_hz;
    snapshot.prefetch_core_id = context != nullptr ? static_cast<int32_t>(context->core_id) : -1;
    snapshot.read_avg_us = read_calls > 0
        ? static_cast<uint32_t>(read_total_us / read_calls)
        : 0;
    snapshot.read_max_us = read_max_us;
    snapshot.read_over_10ms = read_over_10ms;
    snapshot.read_calls = read_calls;

    snapshot.ring_buffered_bytes = context != nullptr && context->stream != nullptr
        ? static_cast<uint32_t>(xStreamBufferBytesAvailable(context->stream))
        : 0;
    snapshot.ring_capacity_bytes = context != nullptr
        ? static_cast<uint32_t>(context->ring_bytes)
        : 0;
    snapshot.ring_min_buffered_bytes =
        static_cast<uint32_t>(decoder->perf_prefetch_min_buffered_bytes);
    snapshot.prefetch_copy_avg_us = decoder->perf_prefetch_wait_calls > 0
        ? static_cast<uint32_t>(
            decoder->perf_prefetch_wait_total_us / decoder->perf_prefetch_wait_calls
        )
        : 0;
    snapshot.prefetch_copy_max_us = decoder->perf_prefetch_wait_max_us;
    snapshot.prefetch_copy_over_2ms = decoder->perf_prefetch_wait_over_2ms;
    snapshot.prefetch_starve_count = decoder->perf_prefetch_starve_count;
    snapshot.prefetch_stack_hwm = context != nullptr
        ? static_cast<uint32_t>(context->stack_hwm)
        : 0;

    snapshot.decode_avg_us = decoder->perf_decode_calls > 0
        ? static_cast<uint32_t>(decoder->perf_decode_total_us / decoder->perf_decode_calls)
        : 0;
    snapshot.decode_max_us = decoder->perf_decode_max_us;
    snapshot.decode_over_20ms = decoder->perf_decode_over_20ms;
    snapshot.decode_over_40ms = decoder->perf_decode_over_40ms;
    snapshot.decode_calls = decoder->perf_decode_calls;

    snapshot.refill_avg_us = decoder->perf_refill_calls > 0
        ? static_cast<uint32_t>(decoder->perf_refill_total_us / decoder->perf_refill_calls)
        : 0;
    snapshot.refill_max_us = decoder->perf_refill_max_us;
    snapshot.refill_over_20ms = decoder->perf_refill_over_20ms;
    snapshot.refill_over_30ms = decoder->perf_refill_over_30ms;
    snapshot.refill_over_block_budget = decoder->perf_refill_over_block_budget;
    snapshot.refill_worst_over_budget_us = decoder->perf_refill_worst_over_budget_us;
    snapshot.refill_calls = decoder->perf_refill_calls;
    snapshot.process_per_refill_x100 = decoder->perf_refill_calls > 0
        ? static_cast<uint32_t>(
            (decoder->perf_refill_process_total * 100ULL + decoder->perf_refill_calls / 2U) /
            decoder->perf_refill_calls
        )
        : 0;
    snapshot.process_per_refill_max = decoder->perf_refill_process_max;
    snapshot.multi_process_refills = decoder->perf_refill_multi_process;
    snapshot.input_fills_per_refill_max = decoder->perf_refill_input_fill_max;
    snapshot.multi_fill_refills = decoder->perf_refill_multi_fill;
    snapshot.input_compact_calls = decoder->perf_input_compact_calls;
    snapshot.input_compact_total_bytes = static_cast<uint32_t>(
        decoder->perf_input_compact_bytes > UINT32_MAX
            ? UINT32_MAX
            : decoder->perf_input_compact_bytes
    );
    snapshot.input_topup_avg_bytes = decoder->perf_input_topup_calls > 0
        ? static_cast<uint32_t>(decoder->perf_input_topup_bytes / decoder->perf_input_topup_calls)
        : 0;
    snapshot.input_topup_max_bytes = decoder->perf_input_topup_max_bytes;

    snapshot.block_budget_us =
        decoder->sample_rate_hz > 0 && decoder->max_block_size > 0
        ? static_cast<uint32_t>(
            (static_cast<uint64_t>(decoder->max_block_size) * 1000000ULL) /
            decoder->sample_rate_hz
        )
        : 0;
    snapshot.refill_avg_load_percent = snapshot.block_budget_us > 0
        ? static_cast<uint32_t>(
            (static_cast<uint64_t>(snapshot.refill_avg_us) * 100ULL + snapshot.block_budget_us / 2U) /
            snapshot.block_budget_us
        )
        : 0;
    snapshot.refill_peak_load_percent = snapshot.block_budget_us > 0
        ? static_cast<uint32_t>(
            (static_cast<uint64_t>(snapshot.refill_max_us) * 100ULL + snapshot.block_budget_us / 2U) /
            snapshot.block_budget_us
        )
        : 0;
    snapshot.refill_peak_margin_us = snapshot.block_budget_us > 0
        ? static_cast<int32_t>(snapshot.block_budget_us) - static_cast<int32_t>(snapshot.refill_max_us)
        : 0;

    portENTER_CRITICAL(&g_flac_perf_snapshot_mux);
    snapshot.sequence = g_flac_perf_snapshot.sequence + 1U;
    g_flac_perf_snapshot = snapshot;
    portEXIT_CRITICAL(&g_flac_perf_snapshot_mux);

    decoder->perf_last_report_us = now_us;
}

bool flac_decoder_get_perf_snapshot(FlacPerfSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }

    portENTER_CRITICAL(&g_flac_perf_snapshot_mux);
    *out_snapshot = g_flac_perf_snapshot;
    portEXIT_CRITICAL(&g_flac_perf_snapshot_mux);
    return out_snapshot->sequence != 0;
}

#endif

static uint8_t *flac_alloc_buffer(size_t size);
static void flac_free_buffer(void *buffer);

#if APP_DIAG_FLAC_PERFORMANCE
static void flac_prefetch_record_read(FlacPrefetchContext *context, uint32_t read_us)
{
    if (context == nullptr) {
        return;
    }

    portENTER_CRITICAL(&g_flac_prefetch_metrics_mux);
    context->perf_read_total_us += read_us;
    ++context->perf_read_calls;
    if (read_us > context->perf_read_max_us) {
        context->perf_read_max_us = read_us;
    }
    if (read_us >= FLAC_SLOW_READ_US) {
        ++context->perf_read_over_10ms;
    }
    portEXIT_CRITICAL(&g_flac_prefetch_metrics_mux);
}

#endif

static void flac_prefetch_task(void *arg)
{
    FlacPrefetchContext *context = static_cast<FlacPrefetchContext *>(arg);
    if (
        context == nullptr ||
        context->source == nullptr ||
        context->stream == nullptr ||
        context->read_buffer == nullptr
    ) {
        if (context != nullptr) {
            context->io_error = true;
#if APP_DIAG_FLAC_PERFORMANCE
            context->stack_hwm = uxTaskGetStackHighWaterMark(nullptr);
#endif
            if (context->done != nullptr) {
                xSemaphoreGive(context->done);
            }
        }
        vTaskDelete(nullptr);
        return;
    }

    context->core_id = xPortGetCoreID();
    if (context->core_id != FLAC_PREFETCH_TASK_CORE) {
        context->io_error = true;
#if APP_DIAG_FLAC_PERFORMANCE
        context->stack_hwm = uxTaskGetStackHighWaterMark(nullptr);
#endif
        ESP_LOGE(TAG, "FLAC预取任务核心绑定异常：当前=%d，期望=%d",
            static_cast<int>(context->core_id),
            static_cast<int>(FLAC_PREFETCH_TASK_CORE));
        if (context->done != nullptr) {
            xSemaphoreGive(context->done);
        }
        vTaskDelete(nullptr);
        return;
    }

#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG, "FLAC预取任务已启动：核心=%d，优先级=%u，栈=%uB，SD块=%uB",
        static_cast<int>(context->core_id),
        static_cast<unsigned>(uxTaskPriorityGet(nullptr)),
        static_cast<unsigned>(FLAC_PREFETCH_TASK_STACK_BYTES),
        static_cast<unsigned>(context->read_chunk_bytes));
#endif

#if APP_DIAG_FLAC_PERFORMANCE
    uint32_t stack_sample_counter = 0;
#endif
    while (!context->stop_requested) {
#if APP_DIAG_FLAC_PERFORMANCE
        if ((stack_sample_counter++ & 0x3FU) == 0U) {
            context->stack_hwm = uxTaskGetStackHighWaterMark(nullptr);
        }
#endif

        // 非零 Seek 时先把最小合法 FLAC 头送入 ring，再读取 seekpoint 对应的真实压缩帧。
        // 前缀只存在于本次 Prefetch 生命周期，不修改底层文件，也不改变 AudioSource 的绝对位置语义。
        if (context->prefix_offset < context->prefix_size) {
            const size_t remaining_prefix = context->prefix_size - context->prefix_offset;
            const size_t sent = xStreamBufferSend(
                context->stream,
                context->prefix + context->prefix_offset,
                remaining_prefix,
                FLAC_PREFETCH_SEND_WAIT
            );
            context->prefix_offset += sent;
            if (sent == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            continue;
        }

        const size_t free_bytes = xStreamBufferSpacesAvailable(context->stream);
        if (free_bytes < context->read_chunk_bytes) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

#if APP_DIAG_FLAC_PERFORMANCE
        const int64_t read_begin_us = esp_timer_get_time();
#endif
        size_t bytes_read = 0;
        const esp_err_t source_ret = audio_source_read(
            context->source,
            context->read_buffer,
            context->read_chunk_bytes,
            &bytes_read
        );
#if APP_DIAG_FLAC_PERFORMANCE
        const uint32_t read_us = static_cast<uint32_t>(esp_timer_get_time() - read_begin_us);
        flac_prefetch_record_read(context, read_us);
#endif

        if (bytes_read > 0) {
            size_t sent = 0;
            while (sent < bytes_read && !context->stop_requested) {
                const size_t chunk = xStreamBufferSend(
                    context->stream,
                    context->read_buffer + sent,
                    bytes_read - sent,
                    FLAC_PREFETCH_SEND_WAIT
                );
                sent += chunk;
            }
        }

        if (source_ret != ESP_OK) {
            if (!context->stop_requested) {
                context->io_error = true;
                ESP_LOGE(TAG, "FLAC 预取任务读取 Source 失败：source=%s ret=%s",
                    audio_source_name(context->source), esp_err_to_name(source_ret));
            }
            break;
        }
        if (bytes_read < context->read_chunk_bytes) {
            if (audio_source_eof(context->source)) {
                context->eof = true;
            } else if (!context->stop_requested) {
                context->io_error = true;
                ESP_LOGE(TAG, "FLAC 预取任务发生非 EOF 短读");
            }
            break;
        }
    }

#if APP_DIAG_FLAC_PERFORMANCE
    context->stack_hwm = uxTaskGetStackHighWaterMark(nullptr);
#endif
    if (context->done != nullptr) {
        xSemaphoreGive(context->done);
    }
    vTaskDelete(nullptr);
}

static void flac_prefetch_destroy(FlacDecoder *decoder)
{
    FlacPrefetchContext *context = flac_prefetch_context(decoder);
    if (context == nullptr) {
        return;
    }

    context->stop_requested = true;
    if (context->task != nullptr && context->done != nullptr) {
        if (xSemaphoreTake(context->done, FLAC_PREFETCH_STOP_WAIT) != pdTRUE) {
            ESP_LOGE(TAG, "等待 FLAC 预取任务退出超时，强制结束任务");
            vTaskDelete(context->task);
        }
        context->task = nullptr;
    }

    if (context->stream != nullptr) {
        vStreamBufferDelete(context->stream);
        context->stream = nullptr;
    }
    flac_free_buffer(context->ring_storage);
    flac_free_buffer(context->read_buffer);
    context->ring_storage = nullptr;
    context->read_buffer = nullptr;
    decoder->prefetch_context = nullptr;
    heap_caps_free(context);
}

static esp_err_t flac_prefetch_start(FlacDecoder *decoder)
{
    if (decoder == nullptr || decoder->source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    FlacPrefetchContext *context = static_cast<FlacPrefetchContext *>(
        heap_caps_calloc(1, sizeof(FlacPrefetchContext), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
    );
    if (context == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const FlacPrefetchProfile profile = flac_prefetch_profile_for_rate(decoder->sample_rate_hz);
    context->source = decoder->source;
    context->ring_bytes = profile.ring_bytes;
    context->read_chunk_bytes = profile.read_chunk_bytes;
    context->start_target_bytes = profile.start_target_bytes;
    if (decoder->prefetch_prefix_size > sizeof(context->prefix)) {
        decoder->prefetch_context = context;
        flac_prefetch_destroy(decoder);
        return ESP_ERR_INVALID_SIZE;
    }
    context->prefix_size = decoder->prefetch_prefix_size;
    if (context->prefix_size > 0) {
        memcpy(context->prefix, decoder->prefetch_prefix, context->prefix_size);
    }
    context->ring_storage = flac_alloc_buffer(context->ring_bytes + 1U);
    context->read_buffer = flac_alloc_buffer(context->read_chunk_bytes);
    if (context->ring_storage == nullptr || context->read_buffer == nullptr) {
        decoder->prefetch_context = context;
        flac_prefetch_destroy(decoder);
        return ESP_ERR_NO_MEM;
    }

    context->stream = xStreamBufferCreateStatic(
        context->ring_bytes + 1U,
        1,
        context->ring_storage,
        &context->stream_storage
    );
    context->done = xSemaphoreCreateBinaryStatic(&context->done_storage);
    if (context->stream == nullptr || context->done == nullptr) {
        decoder->prefetch_context = context;
        flac_prefetch_destroy(decoder);
        return ESP_ERR_NO_MEM;
    }

    decoder->prefetch_context = context;
    const BaseType_t task_ret = xTaskCreatePinnedToCore(
        flac_prefetch_task,
        "FlacPrefetch",
        FLAC_PREFETCH_TASK_STACK_BYTES,
        context,
        FLAC_PREFETCH_TASK_PRIORITY,
        &context->task,
        FLAC_PREFETCH_TASK_CORE
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "创建 FLAC 预取任务失败");
        flac_prefetch_destroy(decoder);
        return ESP_ERR_NO_MEM;
    }

    const uint64_t source_remaining = decoder->file_size_bytes > decoder->prefetch_source_offset_bytes
        ? decoder->file_size_bytes - decoder->prefetch_source_offset_bytes
        : 0;
    const uint64_t remaining_bytes = source_remaining + context->prefix_size;
    size_t target = context->start_target_bytes;
    if (remaining_bytes < target) {
        target = static_cast<size_t>(remaining_bytes);
    }
    const size_t minimum = remaining_bytes < FLAC_INPUT_BUFFER_BYTES
        ? static_cast<size_t>(remaining_bytes)
        : FLAC_INPUT_BUFFER_BYTES;

    const TickType_t start_tick = xTaskGetTickCount();
    while (
        xStreamBufferBytesAvailable(context->stream) < target &&
        !context->eof &&
        !context->io_error &&
        xTaskGetTickCount() - start_tick < FLAC_PREFETCH_START_WAIT
    ) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    const size_t primed = xStreamBufferBytesAvailable(context->stream);
    if (context->io_error || primed < minimum) {
        const bool io_error = context->io_error;
        ESP_LOGE(TAG, "FLAC 预取启动失败：已缓存=%uB，最低需要=%uB",
            static_cast<unsigned>(primed),
            static_cast<unsigned>(minimum));
        flac_prefetch_destroy(decoder);
        return io_error ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }

#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG,
        "FLAC压缩流预取已就绪：%luHz，核心=%d，PSRAM环形=%uKB，SD块=%uB，起播缓存=%uB，任务栈=%uB",
        static_cast<unsigned long>(decoder->sample_rate_hz),
        static_cast<int>(context->core_id),
        static_cast<unsigned>(context->ring_bytes / 1024U),
        static_cast<unsigned>(context->read_chunk_bytes),
        static_cast<unsigned>(primed),
        static_cast<unsigned>(FLAC_PREFETCH_TASK_STACK_BYTES));
#endif
    return ESP_OK;
}

static esp_err_t flac_audio_error_to_esp(esp_audio_err_t error)
{
    switch (error) {
        case ESP_AUDIO_ERR_OK:
        case ESP_AUDIO_ERR_ALREADY_EXIST:
            return ESP_OK;
        case ESP_AUDIO_ERR_MEM_LACK:
            return ESP_ERR_NO_MEM;
        case ESP_AUDIO_ERR_INVALID_PARAMETER:
            return ESP_ERR_INVALID_ARG;
        case ESP_AUDIO_ERR_NOT_SUPPORT:
            return ESP_ERR_NOT_SUPPORTED;
        case ESP_AUDIO_ERR_DATA_LACK:
            return ESP_ERR_INVALID_SIZE;
        case ESP_AUDIO_ERR_HEADER_PARSE:
            return ESP_ERR_INVALID_RESPONSE;
        case ESP_AUDIO_ERR_NOT_FOUND:
            return ESP_ERR_NOT_FOUND;
        default:
            return ESP_FAIL;
    }
}

static uint8_t *flac_alloc_buffer(size_t size)
{
    // FLAC 流式输入/PCM 解码缓冲属于可放外部 RAM 的大块工作区。
    // 不允许 PSRAM 分配失败后悄悄吃掉内部 RAM；失败时由上层明确返回 ESP_ERR_NO_MEM。
    uint8_t *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "FLAC PSRAM 工作缓冲分配失败：%u字节，禁止回落内部RAM",
            static_cast<unsigned>(size));
    }
    return buffer;
}

static void flac_free_buffer(void *buffer)
{
    if (buffer != nullptr) {
        heap_caps_free(buffer);
    }
}

static uint32_t flac_read_synchsafe_u28(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0] & 0x7FU) << 21) |
           (static_cast<uint32_t>(p[1] & 0x7FU) << 14) |
           (static_cast<uint32_t>(p[2] & 0x7FU) << 7) |
           static_cast<uint32_t>(p[3] & 0x7FU);
}

static esp_err_t flac_seek_to_stream_marker(AudioSource *source, uint64_t *out_offset)
{
    if (!audio_source_is_open(source) || out_offset == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t first10[10] = {};
    esp_err_t ret = audio_source_seek(source, 0, AudioSourceSeekOrigin::Begin);
    if (ret != ESP_OK) {
        return ret;
    }
    size_t first_read = 0;
    ret = audio_source_read(source, first10, sizeof(first10), &first_read);
    if (ret != ESP_OK) {
        return ret;
    }
    if (first_read < 4) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t offset = 0;
    if (memcmp(first10, "ID3", 3) == 0) {
        if (first_read < sizeof(first10)) {
            ESP_LOGE(TAG, "ID3v2 文件头不完整");
            return ESP_ERR_INVALID_SIZE;
        }
        if ((first10[6] | first10[7] | first10[8] | first10[9]) & 0x80U) {
            ESP_LOGE(TAG, "ID3v2 标签长度不是 synchsafe 编码");
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t tag_size = flac_read_synchsafe_u28(&first10[6]);
        const bool has_footer = (first10[5] & 0x10U) != 0;
        offset = 10ULL + tag_size + (has_footer ? 10ULL : 0ULL);
#if APP_DIAG_AUDIO_CODEC
        ESP_LOGI(TAG, "检测到前置 ID3v2 标签：跳过=%llu字节",
            static_cast<unsigned long long>(offset));
#endif
    }

    if (offset > static_cast<uint64_t>(INT64_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ret = audio_source_seek(source, static_cast<int64_t>(offset), AudioSourceSeekOrigin::Begin);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t marker[4] = {};
    size_t marker_read = 0;
    ret = audio_source_read(source, marker, sizeof(marker), &marker_read);
    if (ret != ESP_OK || marker_read != sizeof(marker) || memcmp(marker, "fLaC", 4) != 0) {
        ESP_LOGE(TAG, "未找到标准 fLaC 文件头");
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }

    *out_offset = offset;
    return ESP_OK;
}

static uint64_t flac_read_be64(const uint8_t *p)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint64_t>(p[i]);
    }
    return value;
}

static uint16_t flac_read_be16(const uint8_t *p)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) |
        static_cast<uint16_t>(p[1])
    );
}

static esp_err_t flac_parse_stream_descriptor(
    AudioSource *source,
    FlacStreamDescriptor *out_descriptor)
{
    if (!audio_source_is_open(source) || out_descriptor == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_descriptor = {};

    esp_err_t ret = audio_source_size(source, &out_descriptor->file_size_bytes);
    if (ret != ESP_OK || out_descriptor->file_size_bytes == 0) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }

    ret = flac_seek_to_stream_marker(source, &out_descriptor->flac_offset_bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t block_header[4] = {};
    size_t block_header_read = 0;
    ret = audio_source_read(source, block_header, sizeof(block_header), &block_header_read);
    if (ret != ESP_OK || block_header_read != sizeof(block_header)) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }

    bool is_last = (block_header[0] & 0x80U) != 0;
    const uint8_t block_type = block_header[0] & 0x7FU;
    const uint32_t block_length =
        (static_cast<uint32_t>(block_header[1]) << 16) |
        (static_cast<uint32_t>(block_header[2]) << 8) |
        static_cast<uint32_t>(block_header[3]);
    if (block_type != 0 || block_length != FLAC_STREAMINFO_BYTES) {
        ESP_LOGE(TAG, "FLAC 首个元数据块不是标准 STREAMINFO：类型=%u 长度=%lu",
            static_cast<unsigned>(block_type),
            static_cast<unsigned long>(block_length));
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t info_read = 0;
    ret = audio_source_read(
        source,
        out_descriptor->streaminfo_payload,
        FLAC_STREAMINFO_BYTES,
        &info_read);
    if (ret != ESP_OK || info_read != FLAC_STREAMINFO_BYTES) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *info = out_descriptor->streaminfo_payload;
    out_descriptor->max_block_size = static_cast<uint16_t>(
        (static_cast<uint16_t>(info[2]) << 8) | info[3]
    );
    out_descriptor->min_frame_size =
        (static_cast<uint32_t>(info[4]) << 16) |
        (static_cast<uint32_t>(info[5]) << 8) |
        static_cast<uint32_t>(info[6]);
    out_descriptor->max_frame_size =
        (static_cast<uint32_t>(info[7]) << 16) |
        (static_cast<uint32_t>(info[8]) << 8) |
        static_cast<uint32_t>(info[9]);
    out_descriptor->sample_rate_hz =
        (static_cast<uint32_t>(info[10]) << 12) |
        (static_cast<uint32_t>(info[11]) << 4) |
        (static_cast<uint32_t>(info[12]) >> 4);
    out_descriptor->channels = static_cast<uint16_t>(((info[12] >> 1) & 0x07U) + 1U);
    out_descriptor->bits_per_sample = static_cast<uint16_t>(
        (((static_cast<uint16_t>(info[12]) & 0x01U) << 4) | (info[13] >> 4)) + 1U
    );
    out_descriptor->total_frames =
        (static_cast<uint64_t>(info[13] & 0x0FU) << 32) |
        (static_cast<uint64_t>(info[14]) << 24) |
        (static_cast<uint64_t>(info[15]) << 16) |
        (static_cast<uint64_t>(info[16]) << 8) |
        static_cast<uint64_t>(info[17]);

    if (out_descriptor->max_block_size == 0 ||
        out_descriptor->sample_rate_hz == 0 ||
        out_descriptor->channels == 0) {
        ESP_LOGE(TAG, "STREAMINFO 参数无效：block=%u rate=%lu channels=%u",
            static_cast<unsigned>(out_descriptor->max_block_size),
            static_cast<unsigned long>(out_descriptor->sample_rate_hz),
            static_cast<unsigned>(out_descriptor->channels));
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint64_t cursor = out_descriptor->flac_offset_bytes + 4ULL + 4ULL + FLAC_STREAMINFO_BYTES;
    while (!is_last) {
        block_header_read = 0;
        ret = audio_source_read(source, block_header, sizeof(block_header), &block_header_read);
        if (ret != ESP_OK || block_header_read != sizeof(block_header)) {
            return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
        }
        cursor += sizeof(block_header);

        is_last = (block_header[0] & 0x80U) != 0;
        const uint8_t metadata_type = block_header[0] & 0x7FU;
        const uint32_t metadata_length =
            (static_cast<uint32_t>(block_header[1]) << 16) |
            (static_cast<uint32_t>(block_header[2]) << 8) |
            static_cast<uint32_t>(block_header[3]);
        if (cursor > out_descriptor->file_size_bytes ||
            metadata_length > out_descriptor->file_size_bytes - cursor) {
            ESP_LOGE(TAG, "FLAC 元数据块越界：type=%u offset=%llu length=%lu file=%llu",
                static_cast<unsigned>(metadata_type),
                static_cast<unsigned long long>(cursor),
                static_cast<unsigned long>(metadata_length),
                static_cast<unsigned long long>(out_descriptor->file_size_bytes));
            return ESP_ERR_INVALID_SIZE;
        }

        if (metadata_type == 3U && out_descriptor->seektable_length_bytes == 0U) {
            if (metadata_length >= FLAC_SEEKPOINT_BYTES &&
                metadata_length % FLAC_SEEKPOINT_BYTES == 0U) {
                out_descriptor->seektable_offset_bytes = cursor;
                out_descriptor->seektable_length_bytes = metadata_length;
            } else {
                ESP_LOGW(TAG, "FLAC SEEKTABLE 长度非法：%luB，忽略该表",
                    static_cast<unsigned long>(metadata_length));
            }
        }

        ret = audio_source_seek(
            source,
            static_cast<int64_t>(metadata_length),
            AudioSourceSeekOrigin::Current);
        if (ret != ESP_OK) {
            return ret;
        }
        cursor += metadata_length;
    }

    out_descriptor->audio_data_offset_bytes = cursor;
    if (cursor >= out_descriptor->file_size_bytes) {
        ESP_LOGE(TAG, "FLAC 元数据结束后没有音频帧：audio_offset=%llu file=%llu",
            static_cast<unsigned long long>(cursor),
            static_cast<unsigned long long>(out_descriptor->file_size_bytes));
        return ESP_ERR_INVALID_SIZE;
    }

#if APP_DIAG_AUDIO_SEEK
    ESP_LOGI(TAG,
        "FLAC_SEEK_TRACE: TABLE audio_offset=%llu seektable_offset=%llu seekpoints=%lu",
        static_cast<unsigned long long>(out_descriptor->audio_data_offset_bytes),
        static_cast<unsigned long long>(out_descriptor->seektable_offset_bytes),
        static_cast<unsigned long>(out_descriptor->seektable_length_bytes / FLAC_SEEKPOINT_BYTES));
#endif

#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG,
        "STREAMINFO：%luHz / %ubit / %u声道，总帧=%llu，最大块=%u，压缩帧=%lu~%luB，audio_offset=%llu，seekpoints=%lu",
        static_cast<unsigned long>(out_descriptor->sample_rate_hz),
        static_cast<unsigned>(out_descriptor->bits_per_sample),
        static_cast<unsigned>(out_descriptor->channels),
        static_cast<unsigned long long>(out_descriptor->total_frames),
        static_cast<unsigned>(out_descriptor->max_block_size),
        static_cast<unsigned long>(out_descriptor->min_frame_size),
        static_cast<unsigned long>(out_descriptor->max_frame_size),
        static_cast<unsigned long long>(out_descriptor->audio_data_offset_bytes),
        static_cast<unsigned long>(out_descriptor->seektable_length_bytes / FLAC_SEEKPOINT_BYTES));
#endif
    return ESP_OK;
}

static void flac_apply_stream_descriptor(
    FlacDecoder *decoder,
    const FlacStreamDescriptor &descriptor)
{
    decoder->file_size_bytes = descriptor.file_size_bytes;
    decoder->flac_offset_bytes = descriptor.flac_offset_bytes;
    decoder->audio_data_offset_bytes = descriptor.audio_data_offset_bytes;
    decoder->seektable_offset_bytes = descriptor.seektable_offset_bytes;
    decoder->seektable_length_bytes = descriptor.seektable_length_bytes;
    memcpy(decoder->streaminfo_payload, descriptor.streaminfo_payload, FLAC_STREAMINFO_BYTES);
    decoder->max_block_size = descriptor.max_block_size;
    decoder->min_frame_size = descriptor.min_frame_size;
    decoder->max_frame_size = descriptor.max_frame_size;
    decoder->sample_rate_hz = descriptor.sample_rate_hz;
    decoder->channels = descriptor.channels;
    decoder->bits_per_sample = descriptor.bits_per_sample;
    decoder->total_frames = descriptor.total_frames;
}

static esp_err_t flac_parse_streaminfo(FlacDecoder *decoder)
{
    if (decoder == nullptr || decoder->source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    FlacStreamDescriptor descriptor = {};
    const esp_err_t ret = flac_parse_stream_descriptor(decoder->source, &descriptor);
    if (ret == ESP_OK) {
        flac_apply_stream_descriptor(decoder, descriptor);
    }
    return ret;
}

static esp_err_t flac_select_seekpoint(
    AudioSource *source,
    const FlacStreamDescriptor &descriptor,
    uint64_t target_frame,
    FlacSeekPoint *out_point)
{
    if (!audio_source_is_open(source) || out_point == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (descriptor.seektable_length_bytes < FLAC_SEEKPOINT_BYTES ||
        descriptor.seektable_length_bytes % FLAC_SEEKPOINT_BYTES != 0U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (descriptor.seektable_offset_bytes > static_cast<uint64_t>(INT64_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = audio_source_seek(
        source,
        static_cast<int64_t>(descriptor.seektable_offset_bytes),
        AudioSourceSeekOrigin::Begin);
    if (ret != ESP_OK) {
        return ret;
    }

    // 即使首个显式 seekpoint 晚于目标，也允许以第一帧 sample=0/offset=0 作为隐式基点。
    FlacSeekPoint best = {};
    bool found_valid_entry = false;
    const uint32_t count = descriptor.seektable_length_bytes / FLAC_SEEKPOINT_BYTES;
    uint8_t entry[FLAC_SEEKPOINT_BYTES] = {};
    for (uint32_t i = 0; i < count; ++i) {
        size_t bytes_read = 0;
        ret = audio_source_read(source, entry, sizeof(entry), &bytes_read);
        if (ret != ESP_OK || bytes_read != sizeof(entry)) {
            return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
        }

        const uint64_t sample_number = flac_read_be64(entry);
        if (sample_number == FLAC_SEEKPOINT_PLACEHOLDER) {
            continue;
        }
        const uint64_t stream_offset = flac_read_be64(entry + 8);
        const uint16_t frame_samples = flac_read_be16(entry + 16);
        if (sample_number >= descriptor.total_frames && descriptor.total_frames != 0) {
            continue;
        }
        if (stream_offset > descriptor.file_size_bytes - descriptor.audio_data_offset_bytes) {
            continue;
        }
        found_valid_entry = true;
        if (sample_number <= target_frame && sample_number >= best.sample_number) {
            best.sample_number = sample_number;
            best.stream_offset = stream_offset;
            best.frame_samples = frame_samples;
        }
    }

    if (!found_valid_entry) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *out_point = best;
    return ESP_OK;
}

static void flac_build_synthetic_header(
    const FlacStreamDescriptor &descriptor,
    uint8_t out_header[FLAC_SYNTHETIC_HEADER_BYTES])
{
    memcpy(out_header, "fLaC", 4);
    // 只保留 STREAMINFO，并把它标记为最后一个 metadata block。
    out_header[4] = 0x80U;
    out_header[5] = 0x00U;
    out_header[6] = 0x00U;
    out_header[7] = static_cast<uint8_t>(FLAC_STREAMINFO_BYTES);
    memcpy(out_header + 8, descriptor.streaminfo_payload, FLAC_STREAMINFO_BYTES);
}

static esp_err_t flac_validate_sink_format(const FlacDecoder *decoder)
{
    if (decoder == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!audio_rate_profile_flac_enabled(decoder->sample_rate_hz)) {
        ESP_LOGW(TAG, "当前 PCM 硬件档位不支持 FLAC=%luHz/%ubit/%u声道",
            static_cast<unsigned long>(decoder->sample_rate_hz),
            static_cast<unsigned>(decoder->bits_per_sample),
            static_cast<unsigned>(decoder->channels));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (decoder->channels != 1 && decoder->channels != 2) {
        ESP_LOGW(TAG, "当前 PCM sink 仅支持单/双声道 FLAC：实际=%u声道",
            static_cast<unsigned>(decoder->channels));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (
        decoder->bits_per_sample != 16 &&
        decoder->bits_per_sample != 24 &&
        decoder->bits_per_sample != 32
    ) {
        ESP_LOGW(TAG, "当前 FLAC PCM 位深仅支持 16/24/32bit：实际=%ubit",
            static_cast<unsigned>(decoder->bits_per_sample));
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t flac_resize_decoded_buffer(FlacDecoder *decoder, size_t requested)
{
    if (decoder == nullptr || requested == 0 || requested > FLAC_MAX_DECODED_BUFFER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (decoder->decoded_capacity >= requested) {
        return ESP_OK;
    }

    uint8_t *new_buffer = nullptr;
    esp_err_t reserve_ret = ESP_OK;
    if (decoder->workspace != nullptr) {
        reserve_ret = audio_decode_workspace_reserve_decoded(
            decoder->workspace, requested, &new_buffer);
    } else {
        new_buffer = flac_alloc_buffer(requested);
        reserve_ret = new_buffer != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (reserve_ret != ESP_OK || new_buffer == nullptr) {
        ESP_LOGE(TAG, "FLAC PCM 输出缓冲分配失败：%u字节", static_cast<unsigned>(requested));
        return reserve_ret != ESP_OK ? reserve_ret : ESP_ERR_NO_MEM;
    }
    if (decoder->workspace == nullptr) {
        flac_free_buffer(decoder->decoded_buffer);
    }
    decoder->decoded_buffer = new_buffer;
    decoder->decoded_capacity = requested;
    decoder->decoded_offset = 0;
    decoder->decoded_size = 0;
#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG, "FLAC PCM 输出缓冲调整为 %u 字节（shared=%u）",
        static_cast<unsigned>(requested),
        static_cast<unsigned>(decoder->workspace != nullptr));
#endif
    return ESP_OK;
}

static esp_err_t flac_verify_runtime_info(FlacDecoder *decoder)
{
    if (decoder == nullptr || decoder->simple_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (decoder->runtime_info_verified) {
        return ESP_OK;
    }

    esp_audio_simple_dec_info_t info = {};
    const esp_audio_err_t codec_ret = esp_audio_simple_dec_get_info(
        static_cast<esp_audio_simple_dec_handle_t>(decoder->simple_handle),
        &info
    );
    if (codec_ret != ESP_AUDIO_ERR_OK) {
        return flac_audio_error_to_esp(codec_ret);
    }

#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG, "乐鑫 FLAC 解码输出：%luHz / %ubit / %u声道，bitrate=%lu",
        static_cast<unsigned long>(info.sample_rate),
        static_cast<unsigned>(info.bits_per_sample),
        static_cast<unsigned>(info.channel),
        static_cast<unsigned long>(info.bitrate));
#endif

    if (
        info.sample_rate != decoder->sample_rate_hz ||
        info.channel != decoder->channels ||
        info.bits_per_sample != decoder->bits_per_sample
    ) {
        ESP_LOGE(TAG, "FLAC STREAMINFO 与解码器输出参数不一致，拒绝继续播放");
        return ESP_ERR_INVALID_RESPONSE;
    }

    decoder->runtime_info_verified = true;
    return ESP_OK;
}

static TickType_t flac_prefetch_receive_wait(const FlacDecoder *decoder)
{
    // 首块 PCM 尚未交给 I2S 时不存在 DMA underrun 截止时间。此阶段若 64KB 起播预充
    // 被 FLAC 元数据/parser 消耗完，允许等待后台完成下一次 SD 读取，而不是立即失败。
    if (decoder != nullptr && decoder->startup_predecode_active) {
        return FLAC_PREFETCH_STARTUP_RECEIVE_WAIT;
    }

    // 正式播放后仍按单个 FLAC 块播放预算的约 1/4 缩放等待：
    // 48k/4096帧约20ms，96k约10ms，192k约5ms，避免等待本身吃光实时预算。
    if (decoder == nullptr || decoder->sample_rate_hz == 0 || decoder->max_block_size == 0) {
        return FLAC_PREFETCH_RECEIVE_WAIT;
    }

    const uint64_t block_budget_us =
        (static_cast<uint64_t>(decoder->max_block_size) * 1000000ULL) / decoder->sample_rate_hz;
    uint32_t wait_ms = static_cast<uint32_t>(block_budget_us / 4000ULL);
    if (wait_ms == 0) {
        wait_ms = 1;
    }
    const uint32_t max_wait_ms = static_cast<uint32_t>(pdTICKS_TO_MS(FLAC_PREFETCH_RECEIVE_WAIT));
    if (wait_ms > max_wait_ms) {
        wait_ms = max_wait_ms;
    }

    // FreeRTOS tick 粒度可能大于 1ms；pdMS_TO_TICKS(5) 被截断为 0 时，
    // 必须至少保留 1 tick，否则会把“允许短暂等待”错误变成非阻塞读取。
    const TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
    return wait_ticks > 0 ? wait_ticks : 1;
}

static size_t flac_input_window_target(const FlacDecoder *decoder)
{
    if (decoder == nullptr || decoder->input_capacity == 0) {
        return 0;
    }

    // STREAMINFO 的最大压缩帧为 0 时表示文件未提供可靠上限，直接保持完整 32KB 窗口。
    if (decoder->max_frame_size == 0 || decoder->max_frame_size >= decoder->input_capacity) {
        return decoder->input_capacity;
    }

    size_t target = static_cast<size_t>(decoder->max_frame_size);
    if (target > decoder->input_capacity - FLAC_INPUT_FRAME_GUARD_BYTES) {
        return decoder->input_capacity;
    }
    target += FLAC_INPUT_FRAME_GUARD_BYTES;
    return target;
}

static esp_err_t flac_prepare_input_window(FlacDecoder *decoder, bool *out_received)
{
    if (out_received != nullptr) {
        *out_received = false;
    }
    if (decoder == nullptr || decoder->input_buffer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t remaining = decoder->input_offset < decoder->input_size
        ? decoder->input_size - decoder->input_offset
        : 0;

    if (decoder->input_chunk_eos) {
        if (remaining == 0) {
            decoder->eof = true;
        }
        return ESP_OK;
    }

    FlacPrefetchContext *context = flac_prefetch_context(decoder);
    if (context == nullptr || context->stream == nullptr) {
        ESP_LOGE(TAG, "FLAC 压缩流预取上下文不存在");
        return ESP_ERR_INVALID_STATE;
    }

    const size_t target = flac_input_window_target(decoder);

    // 已经有足够连续输入时直接交给 parser，不做无意义 memmove，也不从 ring 复制新数据。
    // 只有当前尾部不足以覆盖 max_frame_size + guard 时才补窗。
    if (remaining >= target) {
        decoder->input_chunk_eos =
            context->eof && xStreamBufferBytesAvailable(context->stream) == 0;
        return ESP_OK;
    }

    // 优先直接追加到当前输入缓冲尾部。只有尾部空间不足以补到目标窗口时，
    // 才把未消费数据压到缓冲头，减少 192kHz 热路径中的 PSRAM 搬运量。
    const size_t initial_needed = target - remaining;
    size_t tail_capacity = decoder->input_size < decoder->input_capacity
        ? decoder->input_capacity - decoder->input_size
        : 0;
    if (tail_capacity < initial_needed && remaining > 0 && decoder->input_offset > 0) {
        memmove(
            decoder->input_buffer,
            decoder->input_buffer + decoder->input_offset,
            remaining
        );
#if APP_DIAG_FLAC_PERFORMANCE
        ++decoder->perf_input_compact_calls;
        decoder->perf_input_compact_bytes += remaining;
#endif
        decoder->input_offset = 0;
        decoder->input_size = remaining;
    } else if (remaining == 0) {
        decoder->input_offset = 0;
        decoder->input_size = 0;
    }

    while ((decoder->input_size - decoder->input_offset) < target) {
#if APP_DIAG_FLAC_PERFORMANCE
        const size_t buffered_before = xStreamBufferBytesAvailable(context->stream);
        if (
            decoder->perf_prefetch_wait_calls == 0 ||
            buffered_before < decoder->perf_prefetch_min_buffered_bytes
        ) {
            decoder->perf_prefetch_min_buffered_bytes = buffered_before;
        }
#endif

        // 已经保留有可用压缩数据时，补窗只是优化，不为等待更多数据阻塞 AudioTask。
        // 只有输入完全耗尽时才允许按当前 FLAC 块预算等待预取任务。
        const size_t available_now = decoder->input_size - decoder->input_offset;
        const bool must_wait = available_now == 0 && !context->eof;
#if APP_DIAG_FLAC_PERFORMANCE
        if (must_wait && buffered_before == 0) {
            ++decoder->perf_prefetch_starve_count;
        }
#endif
        const TickType_t receive_wait = must_wait
            ? flac_prefetch_receive_wait(decoder)
            : 0;

        const size_t available_before_receive = decoder->input_size - decoder->input_offset;
        const size_t needed = target > available_before_receive
            ? target - available_before_receive
            : 0;
        const size_t tail_free = decoder->input_capacity - decoder->input_size;
        const size_t receive_capacity = needed < tail_free ? needed : tail_free;
        if (receive_capacity == 0) {
            ESP_LOGE(TAG, "FLAC 输入窗口无法继续补充：available=%u target=%u tail=%u",
                static_cast<unsigned>(available_before_receive),
                static_cast<unsigned>(target),
                static_cast<unsigned>(tail_free));
            return ESP_ERR_INVALID_SIZE;
        }

#if APP_DIAG_FLAC_PERFORMANCE
        const int64_t wait_begin_us = esp_timer_get_time();
#endif
        const size_t received = xStreamBufferReceive(
            context->stream,
            decoder->input_buffer + decoder->input_size,
            receive_capacity,
            receive_wait
        );
#if APP_DIAG_FLAC_PERFORMANCE
        const uint32_t wait_us = static_cast<uint32_t>(esp_timer_get_time() - wait_begin_us);
        decoder->perf_prefetch_wait_total_us += wait_us;
        ++decoder->perf_prefetch_wait_calls;
        if (wait_us > decoder->perf_prefetch_wait_max_us) {
            decoder->perf_prefetch_wait_max_us = wait_us;
        }
        if (wait_us >= 2000U) {
            ++decoder->perf_prefetch_wait_over_2ms;
        }
#endif

        if (received > 0) {
            decoder->input_size += received;
#if APP_DIAG_FLAC_PERFORMANCE
            decoder->perf_input_topup_bytes += received;
            ++decoder->perf_input_topup_calls;
            if (received > decoder->perf_input_topup_max_bytes) {
                decoder->perf_input_topup_max_bytes = static_cast<uint32_t>(received);
            }
#endif
            if (out_received != nullptr) {
                *out_received = true;
            }
            continue;
        }

        if (context->io_error) {
            ESP_LOGE(TAG, "FLAC 预取层报告 SD 读取失败");
            return ESP_FAIL;
        }
        if (context->eof) {
            decoder->input_chunk_eos = true;
            break;
        }
        if (must_wait) {
            ESP_LOGE(TAG, "FLAC 预取环形缓冲等待超时：%ums",
                static_cast<unsigned>(pdTICKS_TO_MS(receive_wait)));
            return ESP_ERR_TIMEOUT;
        }

        // 有未消费尾部时，即使当前 ring 瞬间没有新数据也继续交给 parser，
        // 不让“窗口补满”优化反过来成为新的实时阻塞点。
        break;
    }

    decoder->input_chunk_eos =
        context->eof && xStreamBufferBytesAvailable(context->stream) == 0;
    if (decoder->input_size == 0 && decoder->input_chunk_eos) {
        decoder->eof = true;
    }
    return ESP_OK;
}

static esp_err_t flac_decode_next_output(FlacDecoder *decoder)
{
    if (decoder == nullptr || decoder->simple_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

#if APP_DIAG_FLAC_PERFORMANCE
    const int64_t refill_begin_us = esp_timer_get_time();
    uint32_t refill_process_calls = 0;
    uint32_t refill_input_fills = 0;
#endif
    decoder->decoded_offset = 0;
    decoder->decoded_size = 0;

    while (!decoder->eof) {
#if APP_DIAG_FLAC_PERFORMANCE
        bool input_received = false;
        esp_err_t ret = flac_prepare_input_window(decoder, &input_received);
        if (input_received && ret == ESP_OK) {
            ++refill_input_fills;
        }
#else
        esp_err_t ret = flac_prepare_input_window(decoder, nullptr);
#endif
        if (ret != ESP_OK || decoder->eof) {
            return ret;
        }

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = decoder->input_buffer + decoder->input_offset;
        raw.len = static_cast<uint32_t>(decoder->input_size - decoder->input_offset);
        raw.eos = decoder->input_chunk_eos;

        esp_audio_simple_dec_out_t out = {};
        out.buffer = decoder->decoded_buffer;
        out.len = static_cast<uint32_t>(decoder->decoded_capacity);

#if APP_DIAG_FLAC_PERFORMANCE
        const int64_t decode_begin_us = esp_timer_get_time();
        ++refill_process_calls;
#endif
        esp_audio_err_t codec_ret = esp_audio_simple_dec_process(
            static_cast<esp_audio_simple_dec_handle_t>(decoder->simple_handle),
            &raw,
            &out
        );
#if APP_DIAG_FLAC_PERFORMANCE
        const uint32_t decode_us = static_cast<uint32_t>(esp_timer_get_time() - decode_begin_us);
        decoder->perf_decode_total_us += decode_us;
        ++decoder->perf_decode_calls;
        if (decode_us > decoder->perf_decode_max_us) {
            decoder->perf_decode_max_us = decode_us;
        }
        if (decode_us >= FLAC_SLOW_DECODE_US) {
            ++decoder->perf_decode_over_20ms;
        }
        if (decode_us >= FLAC_CRITICAL_DECODE_US) {
            ++decoder->perf_decode_over_40ms;
        }
#endif
        if (codec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (out.needed_size == 0 || out.needed_size > FLAC_MAX_DECODED_BUFFER_BYTES) {
                ESP_LOGE(TAG, "FLAC 解码器请求异常输出缓冲：%lu字节",
                    static_cast<unsigned long>(out.needed_size));
                return ESP_ERR_INVALID_SIZE;
            }
            ret = flac_resize_decoded_buffer(decoder, out.needed_size);
            if (ret != ESP_OK) {
                return ret;
            }
            continue;
        }
        if (codec_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "乐鑫 FLAC 解码失败：codec_ret=%d", static_cast<int>(codec_ret));
            return flac_audio_error_to_esp(codec_ret);
        }
        if (raw.consumed > raw.len) {
            ESP_LOGE(TAG, "FLAC 解码器报告非法 consumed=%lu/%lu",
                static_cast<unsigned long>(raw.consumed),
                static_cast<unsigned long>(raw.len));
            return ESP_ERR_INVALID_RESPONSE;
        }

        decoder->input_offset += raw.consumed;
        if (out.decoded_size > 0) {
            ret = flac_verify_runtime_info(decoder);
            if (ret != ESP_OK) {
                return ret;
            }
            decoder->decoded_size = out.decoded_size;
            decoder->decoded_offset = 0;

#if APP_DIAG_FLAC_PERFORMANCE
            const uint32_t refill_us = static_cast<uint32_t>(
                esp_timer_get_time() - refill_begin_us
            );
            decoder->perf_refill_total_us += refill_us;
            ++decoder->perf_refill_calls;
            if (refill_us > decoder->perf_refill_max_us) {
                decoder->perf_refill_max_us = refill_us;
            }
            if (refill_us >= FLAC_SLOW_REFILL_US) {
                ++decoder->perf_refill_over_20ms;
            }
            if (refill_us >= FLAC_CRITICAL_REFILL_US) {
                ++decoder->perf_refill_over_30ms;
            }

            // 高采样率判断不能只看固定 20/30ms 阈值。直接按当前 STREAMINFO
            // 的最大块帧数计算真实播放预算，统计 refill 是否已经追不上消费速度。
            if (decoder->sample_rate_hz > 0 && decoder->max_block_size > 0) {
                const uint32_t block_budget_us = static_cast<uint32_t>(
                    (static_cast<uint64_t>(decoder->max_block_size) * 1000000ULL) /
                    decoder->sample_rate_hz
                );
                if (block_budget_us > 0 && refill_us >= block_budget_us) {
                    ++decoder->perf_refill_over_block_budget;
                    const uint32_t over_budget_us = refill_us - block_budget_us;
                    if (over_budget_us > decoder->perf_refill_worst_over_budget_us) {
                        decoder->perf_refill_worst_over_budget_us = over_budget_us;
                    }
                }
            }

            decoder->perf_refill_process_total += refill_process_calls;
            if (refill_process_calls > decoder->perf_refill_process_max) {
                decoder->perf_refill_process_max = refill_process_calls;
            }
            if (refill_process_calls > 1U) {
                ++decoder->perf_refill_multi_process;
            }
            if (refill_input_fills > decoder->perf_refill_input_fill_max) {
                decoder->perf_refill_input_fill_max = refill_input_fills;
            }
            if (refill_input_fills > 1U) {
                ++decoder->perf_refill_multi_fill;
            }

            // refill 计时到这里已经结束；性能快照发布放在计时之后，
            // 避免诊断逻辑污染真实解码峰值。
            flac_perf_maybe_publish(decoder, static_cast<uint64_t>(esp_timer_get_time()));
#endif
            return ESP_OK;
        }

        if (raw.consumed == 0) {
            if (raw.eos) {
                decoder->eof = true;
                return ESP_OK;
            }
            ESP_LOGE(TAG, "FLAC 解码无输入消耗也无 PCM 输出，拒绝死循环");
            return ESP_ERR_INVALID_STATE;
        }

        if (decoder->input_offset >= decoder->input_size && decoder->input_chunk_eos) {
            decoder->eof = true;
            return ESP_OK;
        }
    }

    return ESP_OK;
}

static int32_t flac_read_sample_as_i2s32(const uint8_t *p, uint16_t bits_per_sample)
{
    if (bits_per_sample == 16) {
        const uint32_t raw = static_cast<uint32_t>(p[0]) |
            (static_cast<uint32_t>(p[1]) << 8);
        const uint32_t sign_extended = (raw & 0x8000U) != 0 ? (raw | 0xFFFF0000U) : raw;
        return static_cast<int32_t>(sign_extended << 16);
    }
    if (bits_per_sample == 24) {
        const uint32_t raw = static_cast<uint32_t>(p[0]) |
            (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16);
        const uint32_t sign_extended = (raw & 0x800000U) != 0 ? (raw | 0xFF000000U) : raw;
        return static_cast<int32_t>(sign_extended << 8);
    }

    const uint32_t raw = static_cast<uint32_t>(p[0]) |
        (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24);
    return static_cast<int32_t>(raw);
}

static esp_err_t flac_decoder_prepare_runtime(
    FlacDecoder *decoder,
    AudioDecodeWorkspace *workspace,
    uint64_t source_start_offset,
    const uint8_t *prefix,
    size_t prefix_size,
    uint64_t start_frame)
{
    if (decoder == nullptr || decoder->source == nullptr ||
        prefix_size > sizeof(decoder->prefetch_prefix)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = flac_validate_sink_format(decoder);
    if (ret != ESP_OK) {
        flac_decoder_close(decoder);
        return ret;
    }
    if (decoder->sample_rate_hz > 96000U) {
#if APP_DIAG_FLAC_PERFORMANCE
        ESP_LOGW(TAG, "高采样率 FLAC 实机验证：%luHz / %ubit / %u声道；请重点观察 refill预算、DMA余量和内部RAM",
            static_cast<unsigned long>(decoder->sample_rate_hz),
            static_cast<unsigned>(decoder->bits_per_sample),
            static_cast<unsigned>(decoder->channels));
#endif
    }

    const size_t sample_bytes = decoder->bits_per_sample / 8U;
    size_t decoded_capacity = static_cast<size_t>(decoder->max_block_size) * decoder->channels * sample_bytes;
    if (decoded_capacity < FLAC_MIN_DECODED_BUFFER_BYTES) {
        decoded_capacity = FLAC_MIN_DECODED_BUFFER_BYTES;
    }
    if (decoded_capacity > FLAC_MAX_DECODED_BUFFER_BYTES) {
        ESP_LOGE(TAG, "FLAC STREAMINFO 要求过大的 PCM 缓冲：%u字节",
            static_cast<unsigned>(decoded_capacity));
        flac_decoder_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }

    decoder->workspace = workspace;
    if (workspace != nullptr) {
        ret = audio_decode_workspace_reserve_input(
            workspace, FLAC_INPUT_BUFFER_BYTES, &decoder->input_buffer);
        if (ret == ESP_OK) {
            ret = audio_decode_workspace_reserve_decoded(
                workspace, decoded_capacity, &decoder->decoded_buffer);
        }
    } else {
        decoder->input_buffer = flac_alloc_buffer(FLAC_INPUT_BUFFER_BYTES);
        decoder->decoded_buffer = flac_alloc_buffer(decoded_capacity);
        ret = decoder->input_buffer != nullptr && decoder->decoded_buffer != nullptr
            ? ESP_OK
            : ESP_ERR_NO_MEM;
    }
    if (ret != ESP_OK || decoder->input_buffer == nullptr || decoder->decoded_buffer == nullptr) {
        ESP_LOGE(TAG, "FLAC 流缓冲分配失败：输入=%u 输出=%u shared=%u",
            static_cast<unsigned>(FLAC_INPUT_BUFFER_BYTES),
            static_cast<unsigned>(decoded_capacity),
            static_cast<unsigned>(workspace != nullptr));
        flac_decoder_close(decoder);
        return ret != ESP_OK ? ret : ESP_ERR_NO_MEM;
    }
    decoder->input_capacity = FLAC_INPUT_BUFFER_BYTES;
    decoder->decoded_capacity = decoded_capacity;
    if (decoder->max_frame_size > decoder->input_capacity) {
        ESP_LOGW(TAG,
            "FLAC 最大压缩帧=%luB 超过当前输入缓冲=%uB，单帧可能跨多次读取；高采样率优化时需要重点观察 refill 负载",
            static_cast<unsigned long>(decoder->max_frame_size),
            static_cast<unsigned>(decoder->input_capacity));
    }

    if (source_start_offset > static_cast<uint64_t>(INT64_MAX)) {
        flac_decoder_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }
    decoder->prefetch_source_offset_bytes = source_start_offset;
    decoder->prefetch_prefix_size = prefix_size;
    if (prefix_size > 0 && prefix != nullptr) {
        memcpy(decoder->prefetch_prefix, prefix, prefix_size);
    }
    ret = audio_source_seek(
        decoder->source,
        static_cast<int64_t>(source_start_offset),
        AudioSourceSeekOrigin::Begin);
    if (ret != ESP_OK) {
        flac_decoder_close(decoder);
        return ret;
    }

    ret = flac_prefetch_start(decoder);
    if (ret != ESP_OK) {
        flac_decoder_close(decoder);
        return ret;
    }

    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    cfg.dec_cfg = nullptr;
    cfg.cfg_size = 0;
    cfg.use_frame_dec = false;

    esp_audio_simple_dec_handle_t handle = nullptr;
    const esp_audio_err_t codec_ret = esp_audio_simple_dec_open(&cfg, &handle);
    if (codec_ret != ESP_AUDIO_ERR_OK || handle == nullptr) {
        ESP_LOGE(TAG, "打开乐鑫 FLAC Simple Decoder 失败：codec_ret=%d", static_cast<int>(codec_ret));
        flac_decoder_close(decoder);
        return flac_audio_error_to_esp(codec_ret);
    }
    decoder->simple_handle = handle;

    // 在触碰 I2S/DAC 之前先解出第一块 PCM。Seek 路径中的合成头也在这里完成实机验证。
    decoder->startup_predecode_active = true;
    ret = flac_decode_next_output(decoder);
    decoder->startup_predecode_active = false;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FLAC 首块 PCM 预解码失败：%s", esp_err_to_name(ret));
        flac_decoder_close(decoder);
        return ret;
    }
    if (decoder->decoded_size == 0) {
        ESP_LOGE(TAG, "FLAC 首块 PCM 为空，拒绝启动播放链路");
        flac_decoder_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t source_frame_bytes = sample_bytes * decoder->channels;
    if (source_frame_bytes == 0 || decoder->decoded_size % source_frame_bytes != 0) {
        ESP_LOGE(TAG, "FLAC 首块 PCM 长度与 %ubit/%u声道不整除：%u字节",
            static_cast<unsigned>(decoder->bits_per_sample),
            static_cast<unsigned>(decoder->channels),
            static_cast<unsigned>(decoder->decoded_size));
        flac_decoder_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }

    // decoded_buffer 的第一个样本对应 start_frame，尚未交给上层，因此 frames_read 正好落在该帧。
    decoder->frames_read = start_frame;
#if APP_DIAG_FLAC_PERFORMANCE
    flac_perf_reset_runtime(decoder);
#endif

#if APP_DIAG_AUDIO_CODEC
    const FlacPrefetchContext *ready_prefetch = flac_prefetch_context(decoder);
    ESP_LOGI(TAG, "FLAC流式解码已就绪：解码输入=%uB，预取环形=%uKB，PCM缓冲=%uB，首块PCM=%uB，起始帧=%llu",
        static_cast<unsigned>(decoder->input_capacity),
        static_cast<unsigned>(ready_prefetch != nullptr ? ready_prefetch->ring_bytes / 1024U : 0U),
        static_cast<unsigned>(decoder->decoded_capacity),
        static_cast<unsigned>(decoder->decoded_size),
        static_cast<unsigned long long>(start_frame));
#endif
    return ESP_OK;
}

esp_err_t flac_decoder_register_backend()
{
    if (g_flac_backend_registered) {
        return ESP_OK;
    }

    const esp_audio_err_t ret = esp_flac_dec_register();
    if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_ALREADY_EXIST) {
        ESP_LOGE(TAG, "注册乐鑫 FLAC 解码器失败：codec_ret=%d", static_cast<int>(ret));
        return flac_audio_error_to_esp(ret);
    }

    g_flac_backend_registered = true;
    ESP_LOGI(TAG, "乐鑫 FLAC 解码后端注册成功");
    return ESP_OK;
}

esp_err_t flac_decoder_open(FlacDecoder *decoder, AudioSource *source, AudioDecodeWorkspace *workspace)
{
    if (decoder == nullptr || !audio_source_is_open(source)) {
        return ESP_ERR_INVALID_ARG;
    }
    flac_decoder_close(decoder);

    esp_err_t ret = flac_decoder_register_backend();
    if (ret != ESP_OK) {
        return ret;
    }

    decoder->source = source;
    if (!audio_source_has_capability(source, AUDIO_SOURCE_CAP_READ) ||
        !audio_source_has_capability(source, AUDIO_SOURCE_CAP_SEEK) ||
        !audio_source_has_capability(source, AUDIO_SOURCE_CAP_SIZE) ||
        !audio_source_has_capability(source, AUDIO_SOURCE_CAP_EOF)) {
        flac_decoder_close(decoder);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ret = flac_parse_streaminfo(decoder);
    if (ret != ESP_OK) {
        flac_decoder_close(decoder);
        return ret;
    }

    // 普通起播仍把完整原始 FLAC（含全部 metadata）交给乐鑫 parser，行为与既有稳定路径一致。
    return flac_decoder_prepare_runtime(
        decoder,
        workspace,
        decoder->flac_offset_bytes,
        nullptr,
        0,
        0);
}

esp_err_t flac_decoder_read_pcm32(
    FlacDecoder *decoder,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames)
{
    if (out_frames != nullptr) {
        *out_frames = 0;
    }
    if (
        decoder == nullptr ||
        out_interleaved_stereo == nullptr ||
        out_frames == nullptr ||
        max_frames == 0 ||
        !flac_decoder_is_open(decoder)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t sample_bytes = decoder->bits_per_sample / 8U;
    const size_t source_frame_bytes = sample_bytes * decoder->channels;
    size_t produced = 0;

    while (produced < max_frames) {
        if (decoder->decoded_offset >= decoder->decoded_size) {
            esp_err_t ret = flac_decode_next_output(decoder);
            if (ret != ESP_OK) {
                return ret;
            }
            if (decoder->decoded_offset >= decoder->decoded_size) {
                break;
            }
            if (decoder->decoded_size % source_frame_bytes != 0) {
                ESP_LOGE(TAG, "FLAC PCM 输出长度与 %ubit/%u声道不整除：%u字节",
                    static_cast<unsigned>(decoder->bits_per_sample),
                    static_cast<unsigned>(decoder->channels),
                    static_cast<unsigned>(decoder->decoded_size));
                return ESP_ERR_INVALID_SIZE;
            }
        }

        const size_t available_frames =
            (decoder->decoded_size - decoder->decoded_offset) / source_frame_bytes;
        const size_t copy_frames = (max_frames - produced) < available_frames
            ? (max_frames - produced)
            : available_frames;

        const uint8_t *src = decoder->decoded_buffer + decoder->decoded_offset;
        for (size_t i = 0; i < copy_frames; ++i) {
            const int32_t left = flac_read_sample_as_i2s32(src, decoder->bits_per_sample);
            int32_t right = left;
            if (decoder->channels == 2) {
                right = flac_read_sample_as_i2s32(src + sample_bytes, decoder->bits_per_sample);
            }
            out_interleaved_stereo[(produced + i) * 2] = left;
            out_interleaved_stereo[(produced + i) * 2 + 1] = right;
            src += source_frame_bytes;
        }

        const size_t consumed_bytes = copy_frames * source_frame_bytes;
        decoder->decoded_offset += consumed_bytes;
        decoder->frames_read += copy_frames;
        produced += copy_frames;
    }

    *out_frames = produced;
    return ESP_OK;
}

static esp_err_t flac_decoder_discard_to_frame(FlacDecoder *decoder, uint64_t target_frame)
{
    if (decoder == nullptr || !flac_decoder_is_open(decoder) || target_frame < decoder->frames_read) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t sample_bytes = decoder->bits_per_sample / 8U;
    const size_t source_frame_bytes = sample_bytes * decoder->channels;
    if (source_frame_bytes == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    while (decoder->frames_read < target_frame) {
        if (decoder->decoded_offset >= decoder->decoded_size) {
            const esp_err_t ret = flac_decode_next_output(decoder);
            if (ret != ESP_OK) {
                return ret;
            }
            if (decoder->decoded_offset >= decoder->decoded_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (decoder->decoded_size % source_frame_bytes != 0) {
                return ESP_ERR_INVALID_SIZE;
            }
        }

        const uint64_t frames_needed = target_frame - decoder->frames_read;
        const size_t available_frames =
            (decoder->decoded_size - decoder->decoded_offset) / source_frame_bytes;
        if (available_frames == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        const size_t skip_frames = frames_needed < available_frames
            ? static_cast<size_t>(frames_needed)
            : available_frames;
        decoder->decoded_offset += skip_frames * source_frame_bytes;
        decoder->frames_read += skip_frames;
    }
    return ESP_OK;
}

esp_err_t flac_decoder_seek_frame(
    FlacDecoder *decoder,
    uint64_t target_frame,
    uint64_t *out_actual_frame,
    uint64_t *out_source_offset)
{
    if (out_actual_frame != nullptr) {
        *out_actual_frame = 0;
    }
    if (out_source_offset != nullptr) {
        *out_source_offset = 0;
    }
    if (decoder == nullptr || !flac_decoder_is_open(decoder)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (target_frame == 0) {
        AudioSource *source = decoder->source;
        AudioDecodeWorkspace *workspace = decoder->workspace;
        const esp_err_t ret = flac_decoder_open(decoder, source, workspace);
        if (ret == ESP_OK) {
            if (out_actual_frame != nullptr) {
                *out_actual_frame = 0;
            }
            if (out_source_offset != nullptr) {
                *out_source_offset = decoder->flac_offset_bytes;
            }
        }
        return ret;
    }
    if (!flac_decoder_has_seektable(decoder)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (decoder->total_frames != 0 && target_frame >= decoder->total_frames) {
        return ESP_ERR_INVALID_ARG;
    }

    AudioSource *source = decoder->source;
    AudioDecodeWorkspace *workspace = decoder->workspace;

    // 先停止旧 Core1 Prefetch 和旧 Simple Decoder，再读取同一个 Source 的 SEEKTABLE。
    // 任何时刻都只有一个任务拥有底层 Source 的文件位置。
    flac_decoder_close(decoder);

    FlacStreamDescriptor descriptor = {};
    esp_err_t ret = flac_parse_stream_descriptor(source, &descriptor);
    if (ret != ESP_OK) {
        return ret;
    }
    if (descriptor.seektable_length_bytes < FLAC_SEEKPOINT_BYTES) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    FlacSeekPoint point = {};
    ret = flac_select_seekpoint(source, descriptor, target_frame, &point);
    if (ret != ESP_OK) {
        return ret;
    }
    if (descriptor.audio_data_offset_bytes > descriptor.file_size_bytes ||
        point.stream_offset > descriptor.file_size_bytes - descriptor.audio_data_offset_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t absolute_source_offset = descriptor.audio_data_offset_bytes + point.stream_offset;
    if (absolute_source_offset >= descriptor.file_size_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t synthetic_header[FLAC_SYNTHETIC_HEADER_BYTES] = {};
    flac_build_synthetic_header(descriptor, synthetic_header);

    decoder->source = source;
    flac_apply_stream_descriptor(decoder, descriptor);
    ret = flac_decoder_prepare_runtime(
        decoder,
        workspace,
        absolute_source_offset,
        synthetic_header,
        sizeof(synthetic_header),
        point.sample_number);
    if (ret != ESP_OK) {
        return ret;
    }

    // SEEKTABLE 只负责跳到目标之前的合法帧；剩余距离在 I2S/DAC 启动前直接丢弃 PCM，
    // 因此旧位置和预滚 PCM 都不会进入硬件，Playback Clock 可精确落在 target_frame。
    ret = flac_decoder_discard_to_frame(decoder, target_frame);
    if (ret != ESP_OK) {
        flac_decoder_close(decoder);
        return ret;
    }

#if APP_DIAG_AUDIO_SEEK
    ESP_LOGI(TAG,
        "FLAC_SEEK_TRACE: target=%llu seekpoint=%llu stream_offset=%llu absolute=%llu discard=%llu seekpoints=%lu",
        static_cast<unsigned long long>(target_frame),
        static_cast<unsigned long long>(point.sample_number),
        static_cast<unsigned long long>(point.stream_offset),
        static_cast<unsigned long long>(absolute_source_offset),
        static_cast<unsigned long long>(target_frame - point.sample_number),
        static_cast<unsigned long>(descriptor.seektable_length_bytes / FLAC_SEEKPOINT_BYTES));
#endif

    if (out_actual_frame != nullptr) {
        *out_actual_frame = target_frame;
    }
    if (out_source_offset != nullptr) {
        *out_source_offset = absolute_source_offset;
    }
    return ESP_OK;
}

void flac_decoder_close(FlacDecoder *decoder)
{
    if (decoder == nullptr) {
        return;
    }

    // Source 在正式播放期间只由预取任务读取；关闭解码器前先停止预取任务，避免清理阶段继续访问底层 I/O。
    flac_prefetch_destroy(decoder);
    if (decoder->simple_handle != nullptr) {
        esp_audio_simple_dec_close(static_cast<esp_audio_simple_dec_handle_t>(decoder->simple_handle));
        decoder->simple_handle = nullptr;
    }
    // Source 生命周期由 PcmDecoder 所有；确认预取任务已退出后，本 Codec 不再访问底层 Source。
    // decoder input/PCM 若来自 AudioTask 共享 workspace，切歌时保留供下一 codec 复用。
    if (decoder->workspace == nullptr) {
        flac_free_buffer(decoder->input_buffer);
        flac_free_buffer(decoder->decoded_buffer);
    }

#if APP_DIAG_FLAC_PERFORMANCE
    portENTER_CRITICAL(&g_flac_perf_snapshot_mux);
    g_flac_perf_snapshot.active = false;
    ++g_flac_perf_snapshot.sequence;
    portEXIT_CRITICAL(&g_flac_perf_snapshot_mux);
#endif

    *decoder = {};
}

bool flac_decoder_is_open(const FlacDecoder *decoder)
{
    return decoder != nullptr && audio_source_is_open(decoder->source) && decoder->simple_handle != nullptr;
}

bool flac_decoder_is_eof(const FlacDecoder *decoder)
{
    return decoder != nullptr && decoder->eof && decoder->decoded_offset >= decoder->decoded_size;
}

bool flac_decoder_has_seektable(const FlacDecoder *decoder)
{
    return decoder != nullptr &&
        decoder->seektable_length_bytes >= FLAC_SEEKPOINT_BYTES &&
        decoder->seektable_length_bytes % FLAC_SEEKPOINT_BYTES == 0U;
}
