#include "flac_decoder.h"
#include "flac_eos_policy.h"
#include "pcm_i2s_convert.h"

#include <limits.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_types.h"
#include "esp_flac_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "../audio_rate_profile.h"
#if APP_DIAG_FLAC_PERFORMANCE
#include "../sources/buffered_http_audio_source.h"
#endif

static const char *TAG = "FLAC";

// Simple Decoder 每次最多取 32KB 连续压缩数据，输入工作区与 PCM 输出区都放在 PSRAM。
// SD 读取不再由 AudioTask 同步执行，而是由独立低优先级预取任务填充 PSRAM 环形缓冲。
// 预取缓存按采样率扩展，未来开放 96/192kHz 时优先增加 PSRAM 吞吐余量，不堆内部 DMA RAM。
static constexpr size_t FLAC_INPUT_BUFFER_BYTES = 32768;
// NAS metadata 顺序解析阶段允许网络 ring 短暂补充，但不能让 AudioTask 无限卡在坏链路上。
static constexpr uint32_t FLAC_STREAMING_METADATA_WAIT_MS = 5000U;
// NAS FLAC metadata 解析后的首块 PCM 预解码可等网络短暂补充；正式播放不使用这个窗口。
static constexpr uint32_t FLAC_STREAMING_PREPLAY_WAIT_MS = 1000U;
// 在 Simple Decoder parser 模式下，尽量让下一完整 FLAC 压缩帧连续落在同一个输入窗口中。
// STREAMINFO 的 max_frame_size 再留少量保护字节，避免窗口边界导致一次 PCM refill 需要两轮 process。
static constexpr size_t FLAC_INPUT_FRAME_GUARD_BYTES = 64;
static constexpr size_t FLAC_PREFETCH_RING_48K_BYTES = 192 * 1024;
static constexpr size_t FLAC_PREFETCH_RING_96K_BYTES = 256 * 1024;
static constexpr size_t FLAC_PREFETCH_RING_192K_BYTES = 256 * 1024;
static constexpr size_t FLAC_PREFETCH_READ_48K_BYTES = 8192;
static constexpr size_t FLAC_PREFETCH_READ_96K_BYTES = 32768;
static constexpr size_t FLAC_PREFETCH_READ_192K_BYTES = 32768;
static constexpr size_t FLAC_PREFETCH_START_48K_BYTES = 64 * 1024;
static constexpr size_t FLAC_PREFETCH_START_96K_BYTES = 128 * 1024;
// 176.4/192k 长测中 256KB ring 最低仍约 198KB、starve=0，128KB 起播预充偏保守。
// Stage 9.5.4 将高采样率起播目标降到 64KB，预计减少约两次 32KB SD 读取的等待；
// 稳态 ring、SD read chunk 和解码窗口均保持不变。
static constexpr size_t FLAC_PREFETCH_START_192K_BYTES = 64 * 1024;
static constexpr uint32_t FLAC_PREFETCH_TASK_STACK_BYTES = 3072;
static constexpr UBaseType_t FLAC_PREFETCH_TASK_PRIORITY = 4;
static constexpr BaseType_t FLAC_PREFETCH_TASK_CORE = 1;
// P1.2.15: cooperative waits must be expressed in RTOS ticks, not sub-tick milliseconds.
// On a 100Hz FreeRTOS tick, pdMS_TO_TICKS(1) becomes 0 and does not truly block the task.
static constexpr TickType_t FLAC_PREFETCH_COOPERATIVE_BLOCK_TICKS = 1;
// P1.5R.2：播放进入稳态后，不再固定“每4次读取休眠1 tick”。
// ring 水位越低，允许连续补充的读取次数越多；但 Emergency 也保留硬上限，
// 最迟连续8次真实 SD 读取后必须阻塞1 tick，避免重新引入 P1.2.9 的 IDLE1 WDT。
static constexpr uint32_t FLAC_PREFETCH_COOPERATIVE_READ_BATCH = 4;  // 起播阶段保持 P1.2.15 行为
static constexpr uint32_t FLAC_PREFETCH_QOS_EMERGENCY_PERCENT = 70;
static constexpr uint32_t FLAC_PREFETCH_QOS_RECOVERY_PERCENT = 90;
static constexpr uint32_t FLAC_PREFETCH_QOS_PLENTY_PERCENT = 92;
static constexpr uint32_t FLAC_PREFETCH_QOS_BATCH_EMERGENCY = 8;
static constexpr uint32_t FLAC_PREFETCH_QOS_BATCH_RECOVERY = 6;
static constexpr uint32_t FLAC_PREFETCH_QOS_BATCH_NORMAL = 4;
static constexpr uint32_t FLAC_PREFETCH_QOS_BATCH_PLENTY = 2;
static constexpr TickType_t FLAC_PREFETCH_RECEIVE_WAIT = pdMS_TO_TICKS(20);
// I2S 启动前的首块预解码和 Seek discard 都不受实时播放预算约束。
// 允许等待完整一次较慢的 SD 读取，避免预取环的短暂空窗被误判为解码失败。
static constexpr TickType_t FLAC_PREFETCH_PREPLAY_RECEIVE_WAIT = pdMS_TO_TICKS(100);
static constexpr TickType_t FLAC_PREFETCH_START_WAIT = pdMS_TO_TICKS(750);
static constexpr TickType_t FLAC_PREFETCH_STOP_WARN_WAIT = pdMS_TO_TICKS(1000);
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

// Cache only the current NAS FLAC track's sparse SEEKTABLE in PSRAM. The first
// seek parses metadata once; later seeks can open the target Range directly.
// CRC8-validated frame positions observed during no-SEEKTABLE Range probes.
// Kept as diagnostics only; the fast approximate path uses the linear estimate
// and validates the destination frame on each seek. Keep the small array in PSRAM.
static constexpr uint32_t FLAC_NAS_NO_TABLE_ANCHOR_CAPACITY = 24U;
struct FlacNasObservedAnchor
{
    uint64_t sample = 0;
    uint64_t file_offset = 0;
};

struct FlacNasSeekCache
{
    FlacStreamDescriptor descriptor = {};
    FlacSeekPoint *points = nullptr;
    uint32_t count = 0;
    uint32_t capacity = 0;
    FlacNasObservedAnchor *observed = nullptr;
    uint32_t observed_count = 0;
    uint32_t observed_next = 0;
    bool valid = false;
};

static FlacNasSeekCache g_nas_flac_seek_cache = {};

void flac_decoder_clear_nas_seek_cache()
{
    if (g_nas_flac_seek_cache.points != nullptr) {
        heap_caps_free(g_nas_flac_seek_cache.points);
    }
    if (g_nas_flac_seek_cache.observed != nullptr) {
        heap_caps_free(g_nas_flac_seek_cache.observed);
    }
    g_nas_flac_seek_cache = {};
}

static void flac_nas_remember_observed_anchor(uint64_t sample, uint64_t file_offset)
{
    FlacNasSeekCache &cache = g_nas_flac_seek_cache;
    const FlacStreamDescriptor &d = cache.descriptor;
    if (!cache.valid || cache.count != 0U || d.total_frames == 0U ||
        sample >= d.total_frames || file_offset < d.audio_data_offset_bytes ||
        file_offset >= d.file_size_bytes) return;
    if (cache.observed == nullptr) {
        cache.observed = static_cast<FlacNasObservedAnchor *>(heap_caps_calloc(
            FLAC_NAS_NO_TABLE_ANCHOR_CAPACITY, sizeof(FlacNasObservedAnchor),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (cache.observed == nullptr) return;  // The uncached path remains valid.
    }
    for (uint32_t i = 0; i < cache.observed_count; ++i) {
        if (cache.observed[i].sample == sample) {
            cache.observed[i].file_offset = file_offset;
            return;
        }
    }
    const uint32_t slot = cache.observed_next;
    cache.observed[slot] = {sample, file_offset};
    cache.observed_next = (slot + 1U) % FLAC_NAS_NO_TABLE_ANCHOR_CAPACITY;
    if (cache.observed_count < FLAC_NAS_NO_TABLE_ANCHOR_CAPACITY) ++cache.observed_count;
    ESP_LOGD(TAG, "NAS_FLAC_NO_TABLE_CACHE: store sample=%llu offset=%llu count=%lu",
        static_cast<unsigned long long>(sample),
        static_cast<unsigned long long>(file_offset),
        static_cast<unsigned long>(cache.observed_count));
}

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

enum class FlacPrefetchQosState : uint8_t
{
    Emergency = 0,
    Recovery,
    Normal,
    Plenty,
};

struct FlacPrefetchContext
{
    AudioSource *source = nullptr;
    StaticSemaphore_t data_ready_storage = {};
    SemaphoreHandle_t data_ready = nullptr;
    StaticSemaphore_t done_storage = {};
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t task = nullptr;
    uint8_t *ring_storage = nullptr;
    size_t ring_read_pos = 0;
    size_t ring_write_pos = 0;
    size_t ring_buffered_bytes = 0;
    uint8_t prefix[FLAC_SYNTHETIC_HEADER_BYTES] = {};
    size_t prefix_size = 0;
    size_t prefix_offset = 0;
    size_t ring_bytes = 0;
    size_t read_chunk_bytes = 0;
    size_t start_target_bytes = 0;
    uint32_t sample_rate_hz = 0;
    bool storage_competes = false;
    volatile BaseType_t core_id = -1;
    volatile bool stop_requested = false;
    volatile bool eof = false;
    volatile bool io_error = false;
    volatile esp_err_t io_error_code = ESP_OK;

    // P1.5R.2 Adaptive Prefetch QoS。起播预充完成前保持 P1.2.15 固定批次，
    // 正式播放后才按 ring 水位动态决定下一次 cooperative block 的批次。
    volatile bool adaptive_qos_active = false;
    volatile bool qos_tracking_started = false;
    volatile bool qos_pressure_active = false;
    volatile FlacPrefetchQosState qos_state = FlacPrefetchQosState::Normal;
    volatile size_t qos_min_buffered_bytes = 0;
    volatile uint32_t qos_emergency_entries = 0;
    volatile uint32_t qos_recovered_count = 0;
    volatile uint32_t qos_max_consecutive_reads = 0;
    volatile uint32_t qos_cooperative_blocks = 0;
#if APP_DIAG_FLAC_PERFORMANCE
    volatile UBaseType_t stack_hwm = 0;
    uint64_t perf_read_total_us = 0;
    uint32_t perf_read_calls = 0;
    uint32_t perf_read_max_us = 0;
    uint32_t perf_read_over_10ms = 0;
#endif
};

static bool g_flac_backend_registered = false;

// R45.15：FLAC 预取改为单生产者/单消费者字节环。
// 只在极短临界区保护读写位置与已缓存字节数；SD fread 和 PSRAM memcpy 均不持锁。
static portMUX_TYPE g_flac_prefetch_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static size_t flac_prefetch_ring_buffered(const FlacPrefetchContext *context)
{
    if (context == nullptr || context->ring_storage == nullptr || context->ring_bytes == 0) {
        return 0;
    }

    portENTER_CRITICAL(&g_flac_prefetch_ring_mux);
    const size_t buffered = context->ring_buffered_bytes;
    portEXIT_CRITICAL(&g_flac_prefetch_ring_mux);
    return buffered;
}

static size_t flac_prefetch_ring_free(const FlacPrefetchContext *context)
{
    if (context == nullptr || context->ring_storage == nullptr || context->ring_bytes == 0) {
        return 0;
    }

    portENTER_CRITICAL(&g_flac_prefetch_ring_mux);
    const size_t free_bytes = context->ring_bytes - context->ring_buffered_bytes;
    portEXIT_CRITICAL(&g_flac_prefetch_ring_mux);
    return free_bytes;
}

static size_t flac_prefetch_ring_write_span(FlacPrefetchContext *context, uint8_t **out_ptr)
{
    if (out_ptr != nullptr) {
        *out_ptr = nullptr;
    }
    if (context == nullptr || out_ptr == nullptr || context->ring_storage == nullptr || context->ring_bytes == 0) {
        return 0;
    }

    portENTER_CRITICAL(&g_flac_prefetch_ring_mux);
    const size_t free_bytes = context->ring_bytes - context->ring_buffered_bytes;
    const size_t tail_bytes = context->ring_bytes - context->ring_write_pos;
    const size_t span = free_bytes < tail_bytes ? free_bytes : tail_bytes;
    if (span > 0) {
        *out_ptr = context->ring_storage + context->ring_write_pos;
    }
    portEXIT_CRITICAL(&g_flac_prefetch_ring_mux);
    return span;
}

static void flac_prefetch_ring_commit_write(FlacPrefetchContext *context, size_t bytes)
{
    if (context == nullptr || bytes == 0 || context->ring_bytes == 0) {
        return;
    }

    portENTER_CRITICAL(&g_flac_prefetch_ring_mux);
    context->ring_write_pos = (context->ring_write_pos + bytes) % context->ring_bytes;
    context->ring_buffered_bytes += bytes;
    portEXIT_CRITICAL(&g_flac_prefetch_ring_mux);

    if (context->data_ready != nullptr) {
        xSemaphoreGive(context->data_ready);
    }
}

static size_t flac_prefetch_ring_write_copy(
    FlacPrefetchContext *context,
    const uint8_t *source,
    size_t bytes
)
{
    if (context == nullptr || source == nullptr || bytes == 0) {
        return 0;
    }

    size_t written = 0;
    while (written < bytes) {
        uint8_t *write_ptr = nullptr;
        const size_t span = flac_prefetch_ring_write_span(context, &write_ptr);
        if (span == 0 || write_ptr == nullptr) {
            break;
        }
        const size_t chunk = (bytes - written) < span ? (bytes - written) : span;
        memcpy(write_ptr, source + written, chunk);
        flac_prefetch_ring_commit_write(context, chunk);
        written += chunk;
    }
    return written;
}

static size_t flac_prefetch_ring_read_once(
    FlacPrefetchContext *context,
    uint8_t *destination,
    size_t capacity
)
{
    if (context == nullptr || destination == nullptr || capacity == 0 ||
        context->ring_storage == nullptr || context->ring_bytes == 0) {
        return 0;
    }

    size_t read_pos = 0;
    size_t received = 0;
    portENTER_CRITICAL(&g_flac_prefetch_ring_mux);
    read_pos = context->ring_read_pos;
    received = capacity < context->ring_buffered_bytes ? capacity : context->ring_buffered_bytes;
    portEXIT_CRITICAL(&g_flac_prefetch_ring_mux);
    if (received == 0) {
        return 0;
    }

    const size_t first = received < (context->ring_bytes - read_pos)
        ? received
        : (context->ring_bytes - read_pos);
    memcpy(destination, context->ring_storage + read_pos, first);
    if (first < received) {
        memcpy(destination + first, context->ring_storage, received - first);
    }

    portENTER_CRITICAL(&g_flac_prefetch_ring_mux);
    context->ring_read_pos = (context->ring_read_pos + received) % context->ring_bytes;
    context->ring_buffered_bytes -= received;
    portEXIT_CRITICAL(&g_flac_prefetch_ring_mux);
    return received;
}

static size_t flac_prefetch_ring_receive(
    FlacPrefetchContext *context,
    uint8_t *destination,
    size_t capacity,
    TickType_t wait_ticks
)
{
    const TickType_t start_tick = xTaskGetTickCount();
    while (true) {
        const size_t received = flac_prefetch_ring_read_once(context, destination, capacity);
        if (received > 0 || wait_ticks == 0 || context == nullptr ||
            context->eof || context->io_error || context->stop_requested) {
            return received;
        }

        const TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (elapsed >= wait_ticks) {
            return 0;
        }
        const TickType_t remaining = wait_ticks - elapsed;
        if (context->data_ready == nullptr || xSemaphoreTake(context->data_ready, remaining) != pdTRUE) {
            return 0;
        }
        // 二值信号量可能是此前提交留下的旧通知；重新检查 ring，必要时继续等待剩余预算。
    }
}

// P1.2.8：后台封面 I/O 只需要知道“当前 FLAC ring 是否有足够安全余量”。
// 用独立 POD 快照避免 Artwork/System 直接碰 FlacPrefetchContext 生命周期。
static portMUX_TYPE g_flac_storage_window_mux = portMUX_INITIALIZER_UNLOCKED;
static FlacStorageWindowSnapshot g_flac_storage_window = {};

static void flac_storage_window_publish(const FlacPrefetchContext *context)
{
    FlacStorageWindowSnapshot snapshot = {};
    if (context != nullptr && context->ring_storage != nullptr) {
        snapshot.active = true;
        snapshot.storage_competes = context->storage_competes;
        snapshot.sample_rate_hz = context->sample_rate_hz;
        snapshot.buffered_bytes = static_cast<uint32_t>(flac_prefetch_ring_buffered(context));
        snapshot.capacity_bytes = static_cast<uint32_t>(context->ring_bytes);
        snapshot.pressure_active = context->qos_pressure_active;
        snapshot.qos_level = static_cast<uint8_t>(context->qos_state);
        snapshot.min_buffered_bytes = static_cast<uint32_t>(context->qos_min_buffered_bytes);
        snapshot.emergency_entries = context->qos_emergency_entries;
        snapshot.recovered_count = context->qos_recovered_count;
    }

    portENTER_CRITICAL(&g_flac_storage_window_mux);
    g_flac_storage_window = snapshot;
    portEXIT_CRITICAL(&g_flac_storage_window_mux);
}

bool flac_decoder_get_storage_window(FlacStorageWindowSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }
    portENTER_CRITICAL(&g_flac_storage_window_mux);
    *out_snapshot = g_flac_storage_window;
    portEXIT_CRITICAL(&g_flac_storage_window_mux);
    return true;
}


bool flac_decoder_get_prefetch_runtime(
    const FlacDecoder *decoder,
    FlacPrefetchRuntimeSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }
    *out_snapshot = {};
    if (decoder == nullptr) {
        return false;
    }

    const FlacPrefetchContext *context = static_cast<const FlacPrefetchContext *>(decoder->prefetch_context);
    if (context == nullptr || context->ring_storage == nullptr) {
        return false;
    }

    out_snapshot->active = true;
    out_snapshot->adaptive_qos_active = context->adaptive_qos_active;
    out_snapshot->pressure_active = context->qos_pressure_active;
    out_snapshot->io_error = context->io_error;
    out_snapshot->eof = context->eof;
    out_snapshot->qos_level = static_cast<uint8_t>(context->qos_state);
    out_snapshot->sample_rate_hz = context->sample_rate_hz;
    out_snapshot->buffered_bytes = static_cast<uint32_t>(flac_prefetch_ring_buffered(context));
    out_snapshot->capacity_bytes = static_cast<uint32_t>(context->ring_bytes);
    out_snapshot->min_buffered_bytes = static_cast<uint32_t>(context->qos_min_buffered_bytes);
    out_snapshot->emergency_entries = context->qos_emergency_entries;
    out_snapshot->recovered_count = context->qos_recovered_count;
    out_snapshot->max_consecutive_reads = context->qos_max_consecutive_reads;
    out_snapshot->cooperative_blocks = context->qos_cooperative_blocks;
    return true;
}

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

static uint32_t flac_prefetch_qos_percent(size_t buffered_bytes, size_t capacity_bytes)
{
    if (capacity_bytes == 0) {
        return 0;
    }
    const uint64_t scaled = static_cast<uint64_t>(buffered_bytes) * 100ULL;
    return static_cast<uint32_t>((scaled + capacity_bytes / 2U) / capacity_bytes);
}

static FlacPrefetchQosState flac_prefetch_qos_state_for_level(
    size_t buffered_bytes,
    size_t capacity_bytes
)
{
    const uint32_t percent = flac_prefetch_qos_percent(buffered_bytes, capacity_bytes);
    if (percent < FLAC_PREFETCH_QOS_EMERGENCY_PERCENT) {
        return FlacPrefetchQosState::Emergency;
    }
    if (percent < FLAC_PREFETCH_QOS_RECOVERY_PERCENT) {
        return FlacPrefetchQosState::Recovery;
    }
    if (percent < FLAC_PREFETCH_QOS_PLENTY_PERCENT) {
        return FlacPrefetchQosState::Normal;
    }
    return FlacPrefetchQosState::Plenty;
}

static uint32_t flac_prefetch_qos_batch_for_state(FlacPrefetchQosState state)
{
    switch (state) {
        case FlacPrefetchQosState::Emergency:
            return FLAC_PREFETCH_QOS_BATCH_EMERGENCY;
        case FlacPrefetchQosState::Recovery:
            return FLAC_PREFETCH_QOS_BATCH_RECOVERY;
        case FlacPrefetchQosState::Plenty:
            return FLAC_PREFETCH_QOS_BATCH_PLENTY;
        case FlacPrefetchQosState::Normal:
        default:
            return FLAC_PREFETCH_QOS_BATCH_NORMAL;
    }
}

static FlacPrefetchQosState flac_prefetch_qos_observe(
    FlacPrefetchContext *context,
    size_t buffered_bytes
)
{
    if (context == nullptr || !context->adaptive_qos_active || context->ring_bytes == 0) {
        return FlacPrefetchQosState::Normal;
    }

    const FlacPrefetchQosState state = flac_prefetch_qos_state_for_level(
        buffered_bytes,
        context->ring_bytes
    );

    if (!context->qos_tracking_started) {
        // 起播目标本来就可能只有 ring 的25%/50%，不能把这段“有意的低水位”
        // 计入稳态最小值。首次恢复到 >=90% 后再开始记录运行期 QoS。
        context->qos_state = state;
        if (state == FlacPrefetchQosState::Normal || state == FlacPrefetchQosState::Plenty) {
            context->qos_tracking_started = true;
            context->qos_min_buffered_bytes = buffered_bytes;
        }
        return state;
    }

    if (buffered_bytes < context->qos_min_buffered_bytes) {
        context->qos_min_buffered_bytes = buffered_bytes;
    }

    // R.36.2.2：Emergency 提前到 <70%，恢复提高到 >=90%，给 LVGL 大刷新留出更大的预取余量；
    // 进入/恢复阈值继续分离，避免水位在边界附近抖动时刷日志。
    if (
        state == FlacPrefetchQosState::Emergency &&
        context->qos_state != FlacPrefetchQosState::Emergency &&
        !context->qos_pressure_active
    ) {
        context->qos_pressure_active = true;
        context->qos_emergency_entries = context->qos_emergency_entries + 1U;
        ESP_LOGW(TAG,
            "FLAC 预取进入 Emergency：ring=%u/%uB (%u%%)，切换到最多%u次连续读取",
            static_cast<unsigned>(buffered_bytes),
            static_cast<unsigned>(context->ring_bytes),
            static_cast<unsigned>(flac_prefetch_qos_percent(buffered_bytes, context->ring_bytes)),
            static_cast<unsigned>(FLAC_PREFETCH_QOS_BATCH_EMERGENCY));
    } else if (
        context->qos_pressure_active &&
        (state == FlacPrefetchQosState::Normal || state == FlacPrefetchQosState::Plenty)
    ) {
        context->qos_pressure_active = false;
        context->qos_recovered_count = context->qos_recovered_count + 1U;
        ESP_LOGI(TAG,
            "FLAC 预取已恢复 Normal：ring=%u/%uB (%u%%)",
            static_cast<unsigned>(buffered_bytes),
            static_cast<unsigned>(context->ring_bytes),
            static_cast<unsigned>(flac_prefetch_qos_percent(buffered_bytes, context->ring_bytes)));
    }

    context->qos_state = state;
    return state;
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
    decoder->perf_prefetch_min_buffered_bytes = prefetch != nullptr && prefetch->ring_storage != nullptr
        ? flac_prefetch_ring_buffered(prefetch)
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

    snapshot.ring_buffered_bytes = context != nullptr && context->ring_storage != nullptr
        ? static_cast<uint32_t>(flac_prefetch_ring_buffered(context))
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

    // NAS bypasses the local FLAC prefetch context; report its actual HTTP ring.
    if(context == nullptr && decoder->source != nullptr){
        BufferedHttpAudioSourceStats http = {};
        if(buffered_http_audio_source_get_stats(decoder->source, &http)){
            snapshot.network_source = true;
            snapshot.ring_buffered_bytes = http.ring_buffered_bytes;
            snapshot.ring_capacity_bytes = http.ring_capacity_bytes;
            snapshot.ring_min_buffered_bytes = http.ring_min_buffered_bytes;
            snapshot.prefetch_stack_hwm = http.task_stack_hwm;
        }
    }

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
        context->ring_storage == nullptr ||
        context->data_ready == nullptr
    ) {
        if (context != nullptr) {
            context->io_error = true;
            context->io_error_code = ESP_ERR_INVALID_STATE;
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
        context->io_error_code = ESP_ERR_INVALID_STATE;
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
    uint32_t cooperative_read_count = 0;
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
            const size_t sent = flac_prefetch_ring_write_copy(
                context,
                context->prefix + context->prefix_offset,
                remaining_prefix
            );
            context->prefix_offset += sent;
            flac_storage_window_publish(context);
            if (sent == 0) {
                vTaskDelay(FLAC_PREFETCH_COOPERATIVE_BLOCK_TICKS);
            }
            continue;
        }

        const size_t buffered_before = flac_prefetch_ring_buffered(context);
        if (context->adaptive_qos_active) {
            flac_prefetch_qos_observe(context, buffered_before);
        }

        const size_t free_bytes = flac_prefetch_ring_free(context);
        if (free_bytes < context->read_chunk_bytes) {
            flac_storage_window_publish(context);
            // ring 本身已经逼近满水位，这次容量等待就是一次真实 cooperative block。
            // 清零连续读取计数，避免刚有空间后又因为旧计数立即二次 sleep。
            cooperative_read_count = 0;
            vTaskDelay(FLAC_PREFETCH_COOPERATIVE_BLOCK_TICKS);
            continue;
        }

        uint8_t *write_ptr = nullptr;
        const size_t write_span = flac_prefetch_ring_write_span(context, &write_ptr);
        const size_t read_request = write_span < context->read_chunk_bytes
            ? write_span
            : context->read_chunk_bytes;
        if (read_request == 0 || write_ptr == nullptr) {
            vTaskDelay(FLAC_PREFETCH_COOPERATIVE_BLOCK_TICKS);
            continue;
        }

#if APP_DIAG_FLAC_PERFORMANCE
        const int64_t read_begin_us = esp_timer_get_time();
#endif
        size_t bytes_read = 0;
        const esp_err_t source_ret = audio_source_read(
            context->source,
            write_ptr,
            read_request,
            &bytes_read
        );
#if APP_DIAG_FLAC_PERFORMANCE
        const int64_t read_end_us = esp_timer_get_time();
        const uint64_t read_us64 = read_end_us >= read_begin_us
            ? static_cast<uint64_t>(read_end_us - read_begin_us)
            : 0ULL;
        const uint32_t read_us = read_us64 > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(read_us64);
        flac_prefetch_record_read(context, read_us);
#endif

        if (bytes_read > 0) {
            flac_prefetch_ring_commit_write(context, bytes_read);
            flac_storage_window_publish(context);

            // P1.5R.2：起播预充阶段仍保持 P1.2.15 固定4次读取后阻塞1 tick；
            // 正式播放后按 ring 水位动态调整批次。低水位允许更连续地补粮，
            // 高水位更积极让出 Core1，但任何状态都保留真实阻塞，防止 IDLE1 WDT 回归。
            uint32_t cooperative_batch = FLAC_PREFETCH_COOPERATIVE_READ_BATCH;
            if (context->adaptive_qos_active) {
                const size_t buffered_after = flac_prefetch_ring_buffered(context);
                const FlacPrefetchQosState qos_state = flac_prefetch_qos_observe(
                    context,
                    buffered_after
                );
                cooperative_batch = flac_prefetch_qos_batch_for_state(qos_state);
            }

            ++cooperative_read_count;
            if (
                context->adaptive_qos_active &&
                cooperative_read_count > context->qos_max_consecutive_reads
            ) {
                context->qos_max_consecutive_reads = cooperative_read_count;
            }
            if (cooperative_read_count >= cooperative_batch) {
                cooperative_read_count = 0;
                if (context->adaptive_qos_active) {
                    context->qos_cooperative_blocks = context->qos_cooperative_blocks + 1U;
                }
                vTaskDelay(FLAC_PREFETCH_COOPERATIVE_BLOCK_TICKS);
            }
        }

        if (source_ret != ESP_OK) {
            if (!context->stop_requested) {
                context->io_error = true;
                context->io_error_code = source_ret;
                ESP_LOGE(TAG, "FLAC 预取任务读取 Source 失败：source=%s ret=%s",
                    audio_source_name(context->source), esp_err_to_name(source_ret));
            }
            break;
        }
        if (bytes_read < read_request) {
            if (audio_source_eof(context->source)) {
                context->eof = true;
            } else if (!context->stop_requested) {
                context->io_error = true;
                context->io_error_code = ESP_ERR_INVALID_STATE;
                ESP_LOGE(TAG, "FLAC 预取任务发生非 EOF 短读");
            }
            break;
        }
    }

#if APP_DIAG_FLAC_PERFORMANCE
    context->stack_hwm = uxTaskGetStackHighWaterMark(nullptr);
#endif
    // EOF/故障/停止都唤醒可能正在等待数据的 AudioTask，避免无意义等满剩余超时。
    if (context->data_ready != nullptr) {
        xSemaphoreGive(context->data_ready);
    }
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
        if (xSemaphoreTake(context->done, FLAC_PREFETCH_STOP_WARN_WAIT) != pdTRUE) {
            // 预取任务可能正在持有全局 SD 递归锁执行 Source read。外部强删任务不会执行
            // StorageSdLockGuard 析构，可能把全局 SD 锁永久留在已删除任务名下。
            // 超过正常退出窗口后只告警，并继续等待任务自行离开 I/O 临界区。
            ESP_LOGW(TAG, "FLAC预取任务退出超过1000ms；继续等待安全退出，禁止在SD I/O中强删任务");
            (void)xSemaphoreTake(context->done, portMAX_DELAY);
        }
        context->task = nullptr;
    }

    if (context->adaptive_qos_active && context->qos_tracking_started) {
        ESP_LOGI(TAG,
            "FLAC 预取 QoS 汇总：min=%u/%uB (%u%%) emergency=%u recovered=%u max_batch=%u blocks=%u",
            static_cast<unsigned>(context->qos_min_buffered_bytes),
            static_cast<unsigned>(context->ring_bytes),
            static_cast<unsigned>(flac_prefetch_qos_percent(
                context->qos_min_buffered_bytes,
                context->ring_bytes
            )),
            static_cast<unsigned>(context->qos_emergency_entries),
            static_cast<unsigned>(context->qos_recovered_count),
            static_cast<unsigned>(context->qos_max_consecutive_reads),
            static_cast<unsigned>(context->qos_cooperative_blocks));
    }

    flac_free_buffer(context->ring_storage);
    context->ring_storage = nullptr;
    decoder->prefetch_context = nullptr;
    flac_storage_window_publish(nullptr);
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
    context->sample_rate_hz = decoder->sample_rate_hz;
    context->storage_competes = strcmp(audio_source_name(decoder->source), "SD_FILE") == 0;
    if (decoder->prefetch_prefix_size > sizeof(context->prefix)) {
        decoder->prefetch_context = context;
        flac_prefetch_destroy(decoder);
        return ESP_ERR_INVALID_SIZE;
    }
    context->prefix_size = decoder->prefetch_prefix_size;
    if (context->prefix_size > 0) {
        memcpy(context->prefix, decoder->prefetch_prefix, context->prefix_size);
    }
    context->ring_storage = flac_alloc_buffer(context->ring_bytes);
    if (context->ring_storage == nullptr) {
        decoder->prefetch_context = context;
        flac_prefetch_destroy(decoder);
        return ESP_ERR_NO_MEM;
    }

    context->data_ready = xSemaphoreCreateBinaryStatic(&context->data_ready_storage);
    context->done = xSemaphoreCreateBinaryStatic(&context->done_storage);
    if (context->data_ready == nullptr || context->done == nullptr) {
        decoder->prefetch_context = context;
        flac_prefetch_destroy(decoder);
        return ESP_ERR_NO_MEM;
    }

    decoder->prefetch_context = context;
    flac_storage_window_publish(context);
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
        flac_prefetch_ring_buffered(context) < target &&
        !context->eof &&
        !context->io_error &&
        xTaskGetTickCount() - start_tick < FLAC_PREFETCH_START_WAIT
    ) {
        vTaskDelay(FLAC_PREFETCH_COOPERATIVE_BLOCK_TICKS);
    }

    const size_t primed = flac_prefetch_ring_buffered(context);
    flac_storage_window_publish(context);
    if (context->io_error || primed < minimum) {
        const bool io_error = context->io_error;
        const esp_err_t io_error_code = context->io_error_code;
        ESP_LOGE(TAG, "FLAC 预取启动失败：已缓存=%uB，最低需要=%uB",
            static_cast<unsigned>(primed),
            static_cast<unsigned>(minimum));
        flac_prefetch_destroy(decoder);
        return io_error
            ? (io_error_code != ESP_OK ? io_error_code : ESP_ERR_INVALID_STATE)
            : ESP_ERR_TIMEOUT;
    }

    // 起播预充已经达到最低要求后才启用自适应 QoS，避免改变已经验证过的启动路径。
    // active 最后写入；PrefetchTask 看到 true 时，其余统计初值已经完整。
    context->qos_state = flac_prefetch_qos_state_for_level(primed, context->ring_bytes);
    context->qos_tracking_started = false;
    context->qos_pressure_active = false;
    context->qos_min_buffered_bytes = 0;
    context->qos_emergency_entries = 0;
    context->qos_recovered_count = 0;
    context->qos_max_consecutive_reads = 0;
    context->qos_cooperative_blocks = 0;
    context->adaptive_qos_active = true;
    ESP_LOGI(TAG,
        "Adaptive Prefetch QoS 已启用：ring=%uKB，起始=%uB (%u%%)，阈值=<%u/%u/%u%%，batch=%u/%u/%u/%u",
        static_cast<unsigned>(context->ring_bytes / 1024U),
        static_cast<unsigned>(primed),
        static_cast<unsigned>(flac_prefetch_qos_percent(primed, context->ring_bytes)),
        static_cast<unsigned>(FLAC_PREFETCH_QOS_EMERGENCY_PERCENT),
        static_cast<unsigned>(FLAC_PREFETCH_QOS_RECOVERY_PERCENT),
        static_cast<unsigned>(FLAC_PREFETCH_QOS_PLENTY_PERCENT),
        static_cast<unsigned>(FLAC_PREFETCH_QOS_BATCH_EMERGENCY),
        static_cast<unsigned>(FLAC_PREFETCH_QOS_BATCH_RECOVERY),
        static_cast<unsigned>(FLAC_PREFETCH_QOS_BATCH_NORMAL),
        static_cast<unsigned>(FLAC_PREFETCH_QOS_BATCH_PLENTY));

#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG,
        "FLAC压缩流预取已就绪：%luHz，核心=%d，PSRAM环形=%uKB，SD直写块=%uB，起播缓存=%uB，任务栈=%uB",
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

static esp_err_t flac_decode_streaminfo_payload(FlacStreamDescriptor *descriptor)
{
    if (descriptor == nullptr) return ESP_ERR_INVALID_ARG;
    const uint8_t *info = descriptor->streaminfo_payload;
    descriptor->max_block_size = static_cast<uint16_t>(
        (static_cast<uint16_t>(info[2]) << 8) | info[3]
    );
    descriptor->min_frame_size =
        (static_cast<uint32_t>(info[4]) << 16) |
        (static_cast<uint32_t>(info[5]) << 8) |
        static_cast<uint32_t>(info[6]);
    descriptor->max_frame_size =
        (static_cast<uint32_t>(info[7]) << 16) |
        (static_cast<uint32_t>(info[8]) << 8) |
        static_cast<uint32_t>(info[9]);
    descriptor->sample_rate_hz =
        (static_cast<uint32_t>(info[10]) << 12) |
        (static_cast<uint32_t>(info[11]) << 4) |
        (static_cast<uint32_t>(info[12]) >> 4);
    descriptor->channels = static_cast<uint16_t>(((info[12] >> 1) & 0x07U) + 1U);
    descriptor->bits_per_sample = static_cast<uint16_t>(
        (((static_cast<uint16_t>(info[12]) & 0x01U) << 4) | (info[13] >> 4)) + 1U
    );
    descriptor->total_frames =
        (static_cast<uint64_t>(info[13] & 0x0FU) << 32) |
        (static_cast<uint64_t>(info[14]) << 24) |
        (static_cast<uint64_t>(info[15]) << 16) |
        (static_cast<uint64_t>(info[16]) << 8) |
        static_cast<uint64_t>(info[17]);

    if (descriptor->max_block_size == 0 || descriptor->sample_rate_hz == 0 ||
        descriptor->channels == 0) {
        ESP_LOGE(TAG, "STREAMINFO 参数无效：block=%u rate=%lu channels=%u",
            static_cast<unsigned>(descriptor->max_block_size),
            static_cast<unsigned long>(descriptor->sample_rate_hz),
            static_cast<unsigned>(descriptor->channels));
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t flac_streaming_read_exact(AudioSource *source, void *buffer, size_t bytes)
{
    if (!audio_source_is_open(source) || buffer == nullptr) return ESP_ERR_INVALID_ARG;
    size_t total = 0U;
    const int64_t start_us = esp_timer_get_time();
    while (total < bytes) {
        size_t got = 0U;
        const esp_err_t ret = audio_source_read(
            source, static_cast<uint8_t *>(buffer) + total, bytes - total, &got);
        if (ret == ESP_OK && got > 0U) {
            total += got;
            continue;
        }
        if (ret != ESP_ERR_TIMEOUT) {
            return ret != ESP_OK ? ret : (audio_source_eof(source) ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_STATE);
        }
        const int64_t now_us = esp_timer_get_time();
        if (now_us <= start_us ||
            static_cast<uint64_t>(now_us - start_us) >=
                static_cast<uint64_t>(FLAC_STREAMING_METADATA_WAIT_MS) * 1000ULL) {
            return ESP_ERR_TIMEOUT;
        }
        taskYIELD();
    }
    return ESP_OK;
}

static esp_err_t flac_streaming_discard(AudioSource *source, uint64_t bytes)
{
    uint8_t scratch[256] = {};
    while (bytes > 0U) {
        const size_t chunk = bytes < sizeof(scratch) ? static_cast<size_t>(bytes) : sizeof(scratch);
        const esp_err_t ret = flac_streaming_read_exact(source, scratch, chunk);
        if (ret != ESP_OK) return ret;
        bytes -= chunk;
    }
    return ESP_OK;
}

static esp_err_t flac_parse_stream_descriptor_streaming(
    AudioSource *source,
    FlacStreamDescriptor *out_descriptor,
    FlacNasSeekPlan *seek_plan = nullptr, uint64_t seek_target_ms = 0ULL)
{
    if (!audio_source_is_open(source) || out_descriptor == nullptr ||
        !audio_source_has_capability(source, AUDIO_SOURCE_CAP_READ) ||
        !audio_source_has_capability(source, AUDIO_SOURCE_CAP_STREAMING) ||
        !audio_source_has_capability(source, AUDIO_SOURCE_CAP_EOF)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_descriptor = {};
    if (seek_plan != nullptr) {
        *seek_plan = {};
        flac_decoder_clear_nas_seek_cache();
    }

    uint8_t first4[4] = {};
    esp_err_t ret = flac_streaming_read_exact(source, first4, sizeof(first4));
    if (ret != ESP_OK) return ret;

    uint64_t cursor = 0U;
    if (memcmp(first4, "ID3", 3) == 0) {
        uint8_t id3_tail[6] = {};
        ret = flac_streaming_read_exact(source, id3_tail, sizeof(id3_tail));
        if (ret != ESP_OK) return ret;
        uint8_t id3_header[10] = {};
        memcpy(id3_header, first4, sizeof(first4));
        memcpy(id3_header + 4, id3_tail, sizeof(id3_tail));
        if ((id3_header[6] | id3_header[7] | id3_header[8] | id3_header[9]) & 0x80U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t tag_size = flac_read_synchsafe_u28(&id3_header[6]);
        const bool has_footer = (id3_header[5] & 0x10U) != 0U;
        ret = flac_streaming_discard(source, static_cast<uint64_t>(tag_size) + (has_footer ? 10ULL : 0ULL));
        if (ret != ESP_OK) return ret;
        cursor = 10ULL + tag_size + (has_footer ? 10ULL : 0ULL);
        ret = flac_streaming_read_exact(source, first4, sizeof(first4));
        if (ret != ESP_OK) return ret;
#if APP_DIAG_AUDIO_CODEC
        ESP_LOGI(TAG, "NAS FLAC检测到前置 ID3v2：顺序跳过=%llu字节",
            static_cast<unsigned long long>(cursor));
#endif
    }
    if (memcmp(first4, "fLaC", 4) != 0) {
        ESP_LOGE(TAG, "NAS FLAC未找到标准 fLaC 文件头");
        return ESP_ERR_INVALID_RESPONSE;
    }
    out_descriptor->flac_offset_bytes = cursor;
    if (seek_plan != nullptr &&
        audio_source_size(source, &out_descriptor->file_size_bytes) != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    cursor += 4ULL;

    uint8_t block_header[4] = {};
    ret = flac_streaming_read_exact(source, block_header, sizeof(block_header));
    if (ret != ESP_OK) return ret;
    cursor += sizeof(block_header);
    bool is_last = (block_header[0] & 0x80U) != 0U;
    const uint8_t block_type = block_header[0] & 0x7FU;
    const uint32_t block_length =
        (static_cast<uint32_t>(block_header[1]) << 16) |
        (static_cast<uint32_t>(block_header[2]) << 8) |
        static_cast<uint32_t>(block_header[3]);
    if (block_type != 0U || block_length != FLAC_STREAMINFO_BYTES) {
        ESP_LOGE(TAG, "NAS FLAC首个元数据块不是标准 STREAMINFO：type=%u length=%lu",
            static_cast<unsigned>(block_type), static_cast<unsigned long>(block_length));
        return ESP_ERR_INVALID_RESPONSE;
    }
    ret = flac_streaming_read_exact(
        source, out_descriptor->streaminfo_payload, FLAC_STREAMINFO_BYTES);
    if (ret != ESP_OK) return ret;
    cursor += FLAC_STREAMINFO_BYTES;
    ret = flac_decode_streaminfo_payload(out_descriptor);
    if (ret != ESP_OK) return ret;

    uint64_t chosen_sample = 0ULL;
    uint64_t chosen_offset = 0ULL;
    bool valid_seekpoint = false;
    bool seektable_processed = false;
    uint64_t wanted_sample = 0ULL;
    if (seek_plan != nullptr) {
        if (out_descriptor->sample_rate_hz == 0U ||
            seek_target_ms > UINT64_MAX / out_descriptor->sample_rate_hz) {
            return ESP_ERR_INVALID_ARG;
        }
        wanted_sample = (seek_target_ms * out_descriptor->sample_rate_hz) / 1000ULL;
        if (wanted_sample == 0U ||
            (out_descriptor->total_frames != 0U && wanted_sample >= out_descriptor->total_frames)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    while (!is_last) {
        ret = flac_streaming_read_exact(source, block_header, sizeof(block_header));
        if (ret != ESP_OK) return ret;
        cursor += sizeof(block_header);
        is_last = (block_header[0] & 0x80U) != 0U;
        const uint32_t metadata_length =
            (static_cast<uint32_t>(block_header[1]) << 16) |
            (static_cast<uint32_t>(block_header[2]) << 8) |
            static_cast<uint32_t>(block_header[3]);
        const uint8_t type = block_header[0] & 0x7FU;
        if (type == 3U && out_descriptor->seektable_length_bytes == 0U &&
            metadata_length >= FLAC_SEEKPOINT_BYTES &&
            metadata_length % FLAC_SEEKPOINT_BYTES == 0U) {
            out_descriptor->seektable_offset_bytes = cursor;
            out_descriptor->seektable_length_bytes = metadata_length;
        }
        if (seek_plan != nullptr && type == 3U && !seektable_processed &&
            metadata_length >= FLAC_SEEKPOINT_BYTES &&
            metadata_length % FLAC_SEEKPOINT_BYTES == 0U &&
            metadata_length <= 4096U * FLAC_SEEKPOINT_BYTES) {
            seektable_processed = true;
            const uint32_t entry_count = metadata_length / FLAC_SEEKPOINT_BYTES;
            g_nas_flac_seek_cache.points = static_cast<FlacSeekPoint *>(
                heap_caps_malloc(static_cast<size_t>(entry_count) * sizeof(FlacSeekPoint),
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            g_nas_flac_seek_cache.capacity = g_nas_flac_seek_cache.points != nullptr ? entry_count : 0U;
            uint8_t entry[FLAC_SEEKPOINT_BYTES] = {};
            for (uint32_t n = 0; n < entry_count; ++n) {
                ret = flac_streaming_read_exact(source, entry, sizeof(entry));
                if (ret != ESP_OK) {
                    flac_decoder_clear_nas_seek_cache();
                    return ret;
                }
                const uint64_t sample = flac_read_be64(entry);
                const uint64_t offset = flac_read_be64(entry + 8);
                const uint16_t frame_samples = flac_read_be16(entry + 16);
                if (sample == FLAC_SEEKPOINT_PLACEHOLDER || frame_samples == 0U ||
                    (out_descriptor->total_frames && sample >= out_descriptor->total_frames)) continue;
                valid_seekpoint = true;
                if (g_nas_flac_seek_cache.points != nullptr &&
                    g_nas_flac_seek_cache.count < g_nas_flac_seek_cache.capacity) {
                    FlacSeekPoint &cached = g_nas_flac_seek_cache.points[g_nas_flac_seek_cache.count++];
                    cached.sample_number = sample;
                    cached.stream_offset = offset;
                    cached.frame_samples = frame_samples;
                }
                if (sample <= wanted_sample && sample >= chosen_sample) {
                    chosen_sample = sample;
                    chosen_offset = offset;
                }
            }
        } else {
            ret = flac_streaming_discard(source, metadata_length);
            if (ret != ESP_OK) return ret;
        }
        cursor += metadata_length;
    }
    out_descriptor->audio_data_offset_bytes = cursor;
    if (seek_plan != nullptr && cursor < out_descriptor->file_size_bytes &&
        out_descriptor->total_frames > 0U &&
        ((!seektable_processed || !valid_seekpoint) ||
         (g_nas_flac_seek_cache.points != nullptr && g_nas_flac_seek_cache.count > 0U))) {
        g_nas_flac_seek_cache.descriptor = *out_descriptor;
        g_nas_flac_seek_cache.valid = true;
        ESP_LOGD(TAG, "NAS_FLAC_SEEK_CACHE: stored=1 points=%lu bytes=%u audio_offset=%llu",
            static_cast<unsigned long>(g_nas_flac_seek_cache.count),
            static_cast<unsigned>(g_nas_flac_seek_cache.count * sizeof(FlacSeekPoint)),
            static_cast<unsigned long long>(out_descriptor->audio_data_offset_bytes));
    }
    if (seek_plan != nullptr) {
        if (cursor >= out_descriptor->file_size_bytes ||
            (valid_seekpoint && seektable_processed &&
             (g_nas_flac_seek_cache.points == nullptr || g_nas_flac_seek_cache.count == 0U)) ||
            (valid_seekpoint && seektable_processed &&
             (chosen_offset >= out_descriptor->file_size_bytes - cursor ||
              chosen_sample > wanted_sample ||
              wanted_sample - chosen_sample > static_cast<uint64_t>(out_descriptor->sample_rate_hz) * 30ULL))) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        memcpy(seek_plan->streaminfo, out_descriptor->streaminfo_payload, FLAC_STREAMINFO_BYTES);
        seek_plan->file_bytes = out_descriptor->file_size_bytes;
        seek_plan->audio_offset = cursor;
        seek_plan->seek_offset = cursor + chosen_offset;
        seek_plan->anchor_sample = chosen_sample;
        seek_plan->target_sample = wanted_sample;
        seek_plan->requested_target_sample = wanted_sample;
        // Preserve metadata capability across the Range reopen. The new streaming decoder
        // starts at an audio frame and receives a synthetic header, so its descriptor would
        // otherwise lose the original SEEKTABLE length and reject every subsequent SEEK.
        seek_plan->seektable_offset = out_descriptor->seektable_offset_bytes;
        seek_plan->seektable_length_bytes = out_descriptor->seektable_length_bytes;
        seek_plan->sample_rate_hz = out_descriptor->sample_rate_hz;
        if (!valid_seekpoint || !seektable_processed) {
            // Fraction-of-file offset is only a first guess; NEVER decode it directly.
            // The following bounded Range scan must verify a real frame's sample number.
            const uint64_t audio_bytes = out_descriptor->file_size_bytes - cursor;
            seek_plan->no_seektable = true;
            seek_plan->scan_min_offset = cursor;
            seek_plan->scan_max_offset = out_descriptor->file_size_bytes;
            seek_plan->scan_min_sample = 0U;
            seek_plan->scan_max_sample = out_descriptor->total_frames;
            const uint64_t remainder = audio_bytes % out_descriptor->total_frames;
            if (remainder != 0U && wanted_sample > UINT64_MAX / remainder) return ESP_ERR_NOT_SUPPORTED;
            seek_plan->scan_estimate_offset = cursor +
                (audio_bytes / out_descriptor->total_frames) * wanted_sample +
                (remainder * wanted_sample) / out_descriptor->total_frames;
            // Avoid multiply overflow from huge files: computed term is bounded by audio_bytes.
            seek_plan->scan_required = true;
            (void)flac_decoder_set_nas_seek_scan_window(seek_plan, 64U * 1024U);
            ESP_LOGD(TAG, "NAS_FLAC_NO_TABLE: target=%llu estimate=%llu audio=%llu bytes=%llu",
                static_cast<unsigned long long>(wanted_sample),
                static_cast<unsigned long long>(seek_plan->scan_estimate_offset),
                static_cast<unsigned long long>(cursor),
                static_cast<unsigned long long>(audio_bytes));
        }
    }

#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG,
        "NAS FLAC STREAMINFO：%luHz / %ubit / %u声道 total=%llu max_block=%u frame=%lu~%luB audio_offset=%llu",
        static_cast<unsigned long>(out_descriptor->sample_rate_hz),
        static_cast<unsigned>(out_descriptor->bits_per_sample),
        static_cast<unsigned>(out_descriptor->channels),
        static_cast<unsigned long long>(out_descriptor->total_frames),
        static_cast<unsigned>(out_descriptor->max_block_size),
        static_cast<unsigned long>(out_descriptor->min_frame_size),
        static_cast<unsigned long>(out_descriptor->max_frame_size),
        static_cast<unsigned long long>(out_descriptor->audio_data_offset_bytes));
#endif
    return ESP_OK;
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

    ret = flac_decode_streaminfo_payload(out_descriptor);
    if (ret != ESP_OK) {
        return ret;
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

static void flac_capture_stream_descriptor(
    const FlacDecoder *decoder,
    FlacStreamDescriptor *out_descriptor)
{
    if (decoder == nullptr || out_descriptor == nullptr) {
        return;
    }
    *out_descriptor = {};
    out_descriptor->file_size_bytes = decoder->file_size_bytes;
    out_descriptor->flac_offset_bytes = decoder->flac_offset_bytes;
    out_descriptor->audio_data_offset_bytes = decoder->audio_data_offset_bytes;
    out_descriptor->seektable_offset_bytes = decoder->seektable_offset_bytes;
    out_descriptor->seektable_length_bytes = decoder->seektable_length_bytes;
    memcpy(out_descriptor->streaminfo_payload, decoder->streaminfo_payload, FLAC_STREAMINFO_BYTES);
    out_descriptor->max_block_size = decoder->max_block_size;
    out_descriptor->min_frame_size = decoder->min_frame_size;
    out_descriptor->max_frame_size = decoder->max_frame_size;
    out_descriptor->sample_rate_hz = decoder->sample_rate_hz;
    out_descriptor->channels = decoder->channels;
    out_descriptor->bits_per_sample = decoder->bits_per_sample;
    out_descriptor->total_frames = decoder->total_frames;
}

static constexpr size_t FLAC_FRAME_HEADER_MAX_BYTES = 32;

static uint8_t flac_frame_header_crc8(const uint8_t *data, size_t size)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80U) != 0U
                ? static_cast<uint8_t>((crc << 1U) ^ 0x07U)
                : static_cast<uint8_t>(crc << 1U);
        }
    }
    return crc;
}

static bool flac_parse_utf8_uint64(
    const uint8_t *data,
    size_t size,
    uint64_t *out_value,
    size_t *out_consumed)
{
    if (data == nullptr || size == 0U || out_value == nullptr || out_consumed == nullptr) {
        return false;
    }

    const uint8_t first = data[0];
    if ((first & 0x80U) == 0U) {
        *out_value = first;
        *out_consumed = 1U;
        return true;
    }

    uint8_t length = 0U;
    uint8_t mask = 0x80U;
    while ((first & mask) != 0U && length < 8U) {
        ++length;
        mask >>= 1U;
    }
    if (length < 2U || length > 7U || length > size) {
        return false;
    }

    const uint8_t value_bits = static_cast<uint8_t>(7U - length);
    uint64_t value = value_bits == 0U
        ? 0U
        : static_cast<uint64_t>(first & static_cast<uint8_t>((1U << value_bits) - 1U));
    for (uint8_t i = 1U; i < length; ++i) {
        if ((data[i] & 0xC0U) != 0x80U) {
            return false;
        }
        value = (value << 6U) | static_cast<uint64_t>(data[i] & 0x3FU);
    }

    *out_value = value;
    *out_consumed = length;
    return true;
}

static bool flac_parse_frame_sample_number(
    const uint8_t *data,
    size_t size,
    const FlacStreamDescriptor &descriptor,
    uint64_t *out_sample_number)
{
    if (data == nullptr || size < 6U || out_sample_number == nullptr) {
        return false;
    }

    // 14-bit FLAC sync=0x3FFE，随后 reserved bit 必须为0；blocking strategy 位允许0/1。
    if (data[0] != 0xFFU || (data[1] & 0xFEU) != 0xF8U) {
        return false;
    }

    const bool variable_block = (data[1] & 0x01U) != 0U;
    const uint8_t block_size_code = static_cast<uint8_t>(data[2] >> 4U);
    const uint8_t sample_rate_code = static_cast<uint8_t>(data[2] & 0x0FU);
    const uint8_t channel_assignment = static_cast<uint8_t>(data[3] >> 4U);
    const uint8_t bits_code = static_cast<uint8_t>((data[3] >> 1U) & 0x07U);
    if (block_size_code == 0U || sample_rate_code == 0x0FU || channel_assignment > 10U ||
        bits_code == 3U || bits_code == 7U || (data[3] & 0x01U) != 0U) {
        return false;
    }

    const uint16_t header_channels = channel_assignment <= 7U
        ? static_cast<uint16_t>(channel_assignment + 1U)
        : 2U;
    if (header_channels != descriptor.channels) {
        return false;
    }

    uint16_t header_bits = descriptor.bits_per_sample;
    switch (bits_code) {
        case 0U: break;
        case 1U: header_bits = 8U; break;
        case 2U: header_bits = 12U; break;
        case 4U: header_bits = 16U; break;
        case 5U: header_bits = 20U; break;
        case 6U: header_bits = 24U; break;
        default: return false;
    }
    if (header_bits != descriptor.bits_per_sample) {
        return false;
    }

    size_t cursor = 4U;
    uint64_t coded_number = 0U;
    size_t coded_bytes = 0U;
    if (!flac_parse_utf8_uint64(data + cursor, size - cursor, &coded_number, &coded_bytes)) {
        return false;
    }
    cursor += coded_bytes;

    if (block_size_code == 6U) {
        cursor += 1U;
    } else if (block_size_code == 7U) {
        cursor += 2U;
    }
    if (sample_rate_code == 12U) {
        cursor += 1U;
    } else if (sample_rate_code == 13U || sample_rate_code == 14U) {
        cursor += 2U;
    }
    if (cursor >= size || flac_frame_header_crc8(data, cursor) != data[cursor]) {
        return false;
    }

    uint64_t sample_number = coded_number;
    if (!variable_block) {
        if (descriptor.max_block_size == 0U ||
            coded_number > UINT64_MAX / static_cast<uint64_t>(descriptor.max_block_size)) {
            return false;
        }
        sample_number = coded_number * static_cast<uint64_t>(descriptor.max_block_size);
    }
    if (descriptor.total_frames != 0U && sample_number >= descriptor.total_frames) {
        return false;
    }

    *out_sample_number = sample_number;
    return true;
}

static bool flac_refine_seekpoint_by_frame_scan(
    AudioSource *source,
    AudioDecodeWorkspace *workspace,
    const FlacStreamDescriptor &descriptor,
    uint64_t target_frame,
    uint64_t scan_start_stream_offset,
    uint64_t scan_end_stream_offset,
    uint64_t max_sample_number,
    FlacSeekPoint *in_out_point,
    uint64_t *out_scanned_bytes)
{
    if (out_scanned_bytes != nullptr) {
        *out_scanned_bytes = 0U;
    }
    if (!audio_source_is_open(source) || workspace == nullptr || in_out_point == nullptr ||
        scan_start_stream_offset >= scan_end_stream_offset ||
        scan_end_stream_offset <= in_out_point->stream_offset ||
        max_sample_number <= target_frame ||
        descriptor.audio_data_offset_bytes > descriptor.file_size_bytes) {
        return false;
    }

    uint8_t *scan = nullptr;
    if (audio_decode_workspace_reserve_input(workspace, FLAC_INPUT_BUFFER_BYTES, &scan) != ESP_OK ||
        scan == nullptr || FLAC_INPUT_BUFFER_BYTES <= FLAC_FRAME_HEADER_MAX_BYTES) {
        return false;
    }

    const uint64_t audio_bytes = descriptor.file_size_bytes - descriptor.audio_data_offset_bytes;
    if (scan_end_stream_offset > audio_bytes) {
        scan_end_stream_offset = audio_bytes;
    }

    FlacSeekPoint best = *in_out_point;
    if (scan_start_stream_offset <= best.stream_offset) {
        scan_start_stream_offset = best.stream_offset + 1U;
    }
    uint64_t buffer_stream_offset = scan_start_stream_offset;
    uint64_t scanned_bytes = 0U;
    size_t buffered_bytes = 0U;

    // 这里只做顺序帧头扫描。旧实现为了保留跨 32KB 边界的帧头，每轮都重新 fseek；
    // SD 上大量小范围 fseek 的固定成本远高于顺序读。现在只定位一次，之后保留尾部
    // FLAC_FRAME_HEADER_MAX_BYTES 到下一轮开头，既保持跨块解析能力，也避免重复 seek。
    const uint64_t absolute_start = descriptor.audio_data_offset_bytes + buffer_stream_offset;
    if (absolute_start > static_cast<uint64_t>(INT64_MAX) ||
        audio_source_seek(
            source,
            static_cast<int64_t>(absolute_start),
            AudioSourceSeekOrigin::Begin) != ESP_OK) {
        return false;
    }

    const uint64_t header_tail = audio_bytes - scan_end_stream_offset < FLAC_FRAME_HEADER_MAX_BYTES
        ? audio_bytes - scan_end_stream_offset
        : FLAC_FRAME_HEADER_MAX_BYTES;
    const uint64_t scan_read_end = scan_end_stream_offset + header_tail;

    while (buffer_stream_offset < scan_end_stream_offset) {
        const uint64_t source_position = buffer_stream_offset + buffered_bytes;
        if (source_position < scan_read_end && buffered_bytes < FLAC_INPUT_BUFFER_BYTES) {
            const uint64_t remaining = scan_read_end - source_position;
            const size_t capacity = FLAC_INPUT_BUFFER_BYTES - buffered_bytes;
            const size_t request = remaining < capacity
                ? static_cast<size_t>(remaining)
                : capacity;
            size_t bytes_read = 0U;
            if (request != 0U &&
                (audio_source_read(source, scan + buffered_bytes, request, &bytes_read) != ESP_OK ||
                 bytes_read == 0U)) {
                break;
            }
            buffered_bytes += bytes_read;
        }
        if (buffered_bytes < 6U) {
            break;
        }

        const uint64_t remaining_scan = scan_end_stream_offset - buffer_stream_offset;
        size_t inspect = remaining_scan < buffered_bytes
            ? static_cast<size_t>(remaining_scan)
            : buffered_bytes;
        if (buffer_stream_offset + buffered_bytes < scan_read_end) {
            if (buffered_bytes <= FLAC_FRAME_HEADER_MAX_BYTES) {
                break;
            }
            const size_t safe_inspect = buffered_bytes - FLAC_FRAME_HEADER_MAX_BYTES;
            if (inspect > safe_inspect) {
                inspect = safe_inspect;
            }
        }
        if (inspect == 0U) {
            break;
        }

        for (size_t i = 0U; i < inspect; ++i) {
            if (scan[i] != 0xFFU || i + 1U >= buffered_bytes ||
                (scan[i + 1U] & 0xFEU) != 0xF8U) {
                continue;
            }

            uint64_t sample_number = 0U;
            if (!flac_parse_frame_sample_number(scan + i, buffered_bytes - i, descriptor, &sample_number) ||
                sample_number <= best.sample_number || sample_number >= max_sample_number) {
                continue;
            }
            if (sample_number > target_frame) {
                if (out_scanned_bytes != nullptr) {
                    *out_scanned_bytes = scanned_bytes + i;
                }
                if (best.sample_number > in_out_point->sample_number) {
                    *in_out_point = best;
                    return true;
                }
                return false;
            }

            best.sample_number = sample_number;
            best.stream_offset = buffer_stream_offset + i;
        }

        scanned_bytes += inspect;
        buffer_stream_offset += inspect;
        buffered_bytes -= inspect;
        if (buffered_bytes != 0U) {
            memmove(scan, scan + inspect, buffered_bytes);
        }
    }

    if (out_scanned_bytes != nullptr) {
        *out_scanned_bytes = scanned_bytes;
    }
    if (best.sample_number <= in_out_point->sample_number) {
        return false;
    }
    *in_out_point = best;
    return true;
}

// 无索引本地 FLAC：按 STREAMINFO 总采样数估算偏移，CRC8 校验候选帧头。
// 不持久化索引、不新增任务；限制扫描量，异常文件不执行超长 PCM 丢弃。
static bool flac_no_table_seekable(const FlacStreamDescriptor &d)
{
    return d.total_frames != 0U && d.max_block_size != 0U &&
        d.audio_data_offset_bytes < d.file_size_bytes;
}

static esp_err_t flac_find_no_table_seekpoint(
    AudioSource *source, AudioDecodeWorkspace *workspace,
    const FlacStreamDescriptor &descriptor, uint64_t target_frame,
    FlacSeekPoint *out_point, uint64_t *out_scanned_bytes)
{
    if (!flac_no_table_seekable(descriptor) || source == nullptr || workspace == nullptr ||
        out_point == nullptr || target_frame == 0U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint64_t audio_bytes = descriptor.file_size_bytes - descriptor.audio_data_offset_bytes;
    constexpr uint64_t window = 256U * 1024U;
    constexpr uint32_t max_windows = 48U;
    const uint64_t estimate = static_cast<uint64_t>(
        (static_cast<double>(audio_bytes) * static_cast<double>(target_frame)) /
        static_cast<double>(descriptor.total_frames));
    uint64_t start = estimate > window / 2U ? estimate - window / 2U : 0U;
    FlacSeekPoint best = {};
    uint64_t scanned_total = 0U;

    for (uint32_t attempt = 0U; attempt < max_windows; ++attempt) {
        const uint64_t end = audio_bytes - start < window ? audio_bytes : start + window;
        if (start >= end) break;
        FlacSeekPoint candidate = best;
        uint64_t scanned = 0U;
        const bool found = flac_refine_seekpoint_by_frame_scan(
            source, workspace, descriptor, target_frame, start, end,
            descriptor.total_frames, &candidate, &scanned);
        scanned_total += scanned;
        if (found) best = candidate;

        // 不依赖文件偏移宣称精准：锚点足够近，解码器才会丢弃少量 PCM 到目标。
        if (target_frame >= best.sample_number &&
            target_frame - best.sample_number <=
                static_cast<uint64_t>(descriptor.max_block_size) * 2U) {
            *out_point = best;
            if (out_scanned_bytes) *out_scanned_bytes = scanned_total;
            return ESP_OK;
        }

        if (found) {
            // 仍位于目标之前，向后寻找更近的候选帧。
            if (end == audio_bytes) break;
            start = end;
        } else if (best.sample_number == 0U) {
            // 估算偏移可能已越过目标，逐窗向文件前部回退。
            if (start == 0U) break;
            start = start > window ? start - window : 0U;
        } else {
            // 前一窗口曾发现有效锚点，当前位置没有更靠近目标的帧。
            break;
        }
    }
    if (out_scanned_bytes) *out_scanned_bytes = scanned_total;
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t flac_select_seekpoint(
    AudioSource *source,
    const FlacStreamDescriptor &descriptor,
    uint64_t target_frame,
    FlacSeekPoint *out_point,
    FlacSeekPoint *out_next_point,
    bool *out_has_next)
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
    if (out_next_point != nullptr) *out_next_point = {};
    if (out_has_next != nullptr) *out_has_next = false;

    esp_err_t ret = audio_source_seek(
        source,
        static_cast<int64_t>(descriptor.seektable_offset_bytes),
        AudioSourceSeekOrigin::Begin);
    if (ret != ESP_OK) {
        return ret;
    }

    // 即使首个显式 seekpoint 晚于目标，也允许以第一帧 sample=0/offset=0 作为隐式基点。
    FlacSeekPoint best = {};
    FlacSeekPoint next = {};
    bool has_next = false;
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
        } else if (sample_number > target_frame && (!has_next || sample_number < next.sample_number)) {
            next.sample_number = sample_number;
            next.stream_offset = stream_offset;
            next.frame_samples = frame_samples;
            has_next = true;
        }
    }

    if (!found_valid_entry) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *out_point = best;
    if (out_next_point != nullptr && has_next) *out_next_point = next;
    if (out_has_next != nullptr) *out_has_next = has_next;
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
        if (decoder->sample_rate_hz == 192000U) {
            ESP_LOGW(TAG,
                "192kHz FLAC 已按稳定性策略禁用：%ubit/%u声道；176.4kHz 及以下 FLAC 仍支持",
                static_cast<unsigned>(decoder->bits_per_sample),
                static_cast<unsigned>(decoder->channels));
        } else {
            ESP_LOGW(TAG, "当前 PCM 硬件档位不支持 FLAC=%luHz/%ubit/%u声道",
                static_cast<unsigned long>(decoder->sample_rate_hz),
                static_cast<unsigned>(decoder->bits_per_sample),
                static_cast<unsigned>(decoder->channels));
        }
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
    // 硬件启动前的预解码/Seek discard 不存在 DMA underrun 截止时间。此阶段若预取环耗尽，
    // 允许等待后台完成下一次 SD 读取，而不是套用正式播放的短实时预算。
    if (decoder != nullptr && decoder->preplay_decode_active) {
        return FLAC_PREFETCH_PREPLAY_RECEIVE_WAIT;
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
        if (remaining == 0 &&
            !fakepod_flac::pending_tail(decoder->total_frames, decoder->frames_read, decoder->max_block_size)) {
            decoder->eof = true;
        }
        return ESP_OK;
    }

    const size_t target = flac_input_window_target(decoder);

    if (decoder->streaming_source) {
        if (!audio_source_is_open(decoder->source)) return ESP_ERR_INVALID_STATE;
        if (remaining >= target) return ESP_OK;

        const size_t initial_needed = target - remaining;
        size_t tail_capacity = decoder->input_size < decoder->input_capacity
            ? decoder->input_capacity - decoder->input_size : 0U;
        if (tail_capacity < initial_needed && remaining > 0U && decoder->input_offset > 0U) {
            memmove(decoder->input_buffer, decoder->input_buffer + decoder->input_offset, remaining);
#if APP_DIAG_FLAC_PERFORMANCE
            ++decoder->perf_input_compact_calls;
            decoder->perf_input_compact_bytes += remaining;
#endif
            decoder->input_offset = 0U;
            decoder->input_size = remaining;
        } else if (remaining == 0U) {
            decoder->input_offset = 0U;
            decoder->input_size = 0U;
        }

        const int64_t direct_wait_started_us = esp_timer_get_time();
        while ((decoder->input_size - decoder->input_offset) < target) {
            const size_t available_now = decoder->input_size - decoder->input_offset;
            const size_t needed = target - available_now;
            const size_t tail_free = decoder->input_capacity - decoder->input_size;
            const size_t request = needed < tail_free ? needed : tail_free;
            if (request == 0U) return ESP_ERR_INVALID_SIZE;

            size_t got = 0U;
            const esp_err_t ret = audio_source_read(
                decoder->source, decoder->input_buffer + decoder->input_size, request, &got);
            if (got > 0U) {
                decoder->input_size += got;
#if APP_DIAG_FLAC_PERFORMANCE
                decoder->perf_input_topup_bytes += got;
                ++decoder->perf_input_topup_calls;
                if (got > decoder->perf_input_topup_max_bytes) {
                    decoder->perf_input_topup_max_bytes = static_cast<uint32_t>(got);
                }
#endif
                if (out_received != nullptr) *out_received = true;
            }
            if (ret != ESP_OK) {
                if (ret == ESP_ERR_TIMEOUT && available_now > 0U) break;
                if (ret == ESP_ERR_TIMEOUT && decoder->preplay_decode_active && available_now == 0U) {
                    const int64_t now_us = esp_timer_get_time();
                    if (now_us > direct_wait_started_us &&
                        static_cast<uint64_t>(now_us - direct_wait_started_us) <
                            static_cast<uint64_t>(FLAC_STREAMING_PREPLAY_WAIT_MS) * 1000ULL) {
                        taskYIELD();
                        continue;
                    }
                }
                return ret;
            }
            if (got == 0U) {
                if (audio_source_eof(decoder->source)) {
                    decoder->input_chunk_eos = true;
                    break;
                }
                if (available_now == 0U) return ESP_ERR_TIMEOUT;
                break;
            }
        }
        decoder->input_chunk_eos = audio_source_eof(decoder->source);
        if (decoder->input_size == 0U && decoder->input_chunk_eos &&
            !fakepod_flac::pending_tail(decoder->total_frames, decoder->frames_read, decoder->max_block_size))
            decoder->eof = true;
        return ESP_OK;
    }

    FlacPrefetchContext *context = flac_prefetch_context(decoder);
    if (context == nullptr || context->ring_storage == nullptr) {
        ESP_LOGE(TAG, "FLAC 压缩流预取上下文不存在");
        return ESP_ERR_INVALID_STATE;
    }

    // 已经有足够连续输入时直接交给 parser，不做无意义 memmove，也不从 ring 复制新数据。
    // 只有当前尾部不足以覆盖 max_frame_size + guard 时才补窗。
    if (remaining >= target) {
        decoder->input_chunk_eos =
            context->eof && flac_prefetch_ring_buffered(context) == 0;
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
        const size_t buffered_before = flac_prefetch_ring_buffered(context);
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
        const size_t received = flac_prefetch_ring_receive(
            context,
            decoder->input_buffer + decoder->input_size,
            receive_capacity,
            receive_wait
        );
        flac_storage_window_publish(context);
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
            const esp_err_t io_error_code = context->io_error_code;
            const esp_err_t error = io_error_code != ESP_OK ? io_error_code : ESP_ERR_INVALID_STATE;
            ESP_LOGE(TAG, "FLAC 预取层报告 SD 读取失败：%s", esp_err_to_name(error));
            return error;
        }
        if (context->eof) {
            decoder->input_chunk_eos = true;
            break;
        }
        if (must_wait) {
            // 瞬时欠载属于播放策略，不代表 decoder 已损坏。由 AudioTask 根据 PrefetchTask
            // 的 active/io_error/eof 状态决定是否进入有界恢复，或升级为真实 AUDIO_FAULT。
            return ESP_ERR_TIMEOUT;
        }

        // 有未消费尾部时，即使当前 ring 瞬间没有新数据也继续交给 parser，
        // 不让“窗口补满”优化反过来成为新的实时阻塞点。
        break;
    }

    decoder->input_chunk_eos =
        context->eof && flac_prefetch_ring_buffered(context) == 0;
    if (decoder->input_size == 0 && decoder->input_chunk_eos &&
        !fakepod_flac::pending_tail(decoder->total_frames, decoder->frames_read, decoder->max_block_size)) {
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
        const bool tail_flush = raw.eos && raw.len == 0U &&
            fakepod_flac::pending_tail(decoder->total_frames, decoder->frames_read, decoder->max_block_size);

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
        if (tail_flush) {
            // A single successful empty-EOS process drains the cached tail.
            // EOF may be true while decoded_size is still being delivered.
            decoder->eof = true;
            ESP_LOGI(TAG, "FLAC_EOS_FLUSH: pending=%llu decoded_bytes=%lu",
                (unsigned long long)(decoder->total_frames - decoder->frames_read),
                (unsigned long)out.decoded_size);
            if (out.decoded_size == 0U) {
                ESP_LOGE(TAG, "FLAC尾部缺少声明样本：EOS刷新无PCM，拒绝静默截断");
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        if (out.decoded_size > 0) {
            ret = flac_verify_runtime_info(decoder);
            if (ret != ESP_OK) {
                return ret;
            }
            if (tail_flush) {
                const size_t frame_bytes = (decoder->bits_per_sample / 8U) * decoder->channels;
                const uint64_t pending = decoder->total_frames - decoder->frames_read;
                if (frame_bytes == 0U || out.decoded_size % frame_bytes != 0U ||
                    out.decoded_size / frame_bytes != pending) {
                    ESP_LOGE(TAG, "FLAC尾部刷新样本数不匹配，拒绝补零或重复PCM");
                    return ESP_ERR_INVALID_RESPONSE;
                }
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
                if (fakepod_flac::pending_tail(decoder->total_frames, decoder->frames_read, decoder->max_block_size)) {
                    ESP_LOGE(TAG, "FLAC尾部解析无进度且仍缺样本，拒绝当成正常EOF");
                    return ESP_ERR_INVALID_RESPONSE;
                }
                decoder->eof = true;
                return ESP_OK;
            }
            ESP_LOGE(TAG, "FLAC 解码无输入消耗也无 PCM 输出，拒绝死循环");
            return ESP_ERR_INVALID_STATE;
        }

        if (decoder->input_offset >= decoder->input_size && decoder->input_chunk_eos) {
            if (fakepod_flac::pending_tail(decoder->total_frames, decoder->frames_read, decoder->max_block_size)) {
                // The parser may have consumed the final bytes without emitting
                // its cached short frame yet. Next pass supplies empty EOS.
                continue;
            }
            decoder->eof = true;
            return ESP_OK;
        }
    }

    return ESP_OK;
}

static esp_err_t flac_decoder_prepare_runtime(
    FlacDecoder *decoder,
    AudioDecodeWorkspace *workspace,
    uint64_t source_start_offset,
    const uint8_t *prefix,
    size_t prefix_size,
    uint64_t start_frame,
    bool direct_streaming = false)
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
    decoder->streaming_source = direct_streaming;
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
    if (direct_streaming) {
        decoder->input_offset = 0U;
        decoder->input_size = 0U;
        decoder->input_chunk_eos = false;
        if (prefix_size > decoder->input_capacity) {
            flac_decoder_close(decoder);
            return ESP_ERR_INVALID_SIZE;
        }
        if (prefix_size > 0U && prefix != nullptr) {
            memcpy(decoder->input_buffer, prefix, prefix_size);
            decoder->input_size = prefix_size;
        }
    } else {
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
    decoder->preplay_decode_active = true;
    ret = flac_decode_next_output(decoder);
    decoder->preplay_decode_active = false;
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
#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(TAG, "乐鑫 FLAC 解码后端注册成功");
#endif
#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(TAG,
        "Cooperative Tick：RTOS=%uHz tick=%ums pdMS_TO_TICKS(1)=%u，每%u个SD块主动阻塞1tick",
        static_cast<unsigned>(configTICK_RATE_HZ),
        static_cast<unsigned>(portTICK_PERIOD_MS),
        static_cast<unsigned>(pdMS_TO_TICKS(1)),
        static_cast<unsigned>(FLAC_PREFETCH_COOPERATIVE_READ_BATCH));
#endif
    return ESP_OK;
}

static esp_err_t flac_decoder_prepare_open_descriptor(
    FlacDecoder *decoder,
    AudioSource *source,
    FlacStreamDescriptor *out_descriptor)
{
    if (decoder == nullptr || !audio_source_is_open(source) || out_descriptor == nullptr) {
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

    ret = flac_parse_stream_descriptor(source, out_descriptor);
    if (ret != ESP_OK) {
        flac_decoder_close(decoder);
        return ret;
    }
    flac_apply_stream_descriptor(decoder, *out_descriptor);
    return ESP_OK;
}

esp_err_t flac_decoder_open(FlacDecoder *decoder, AudioSource *source, AudioDecodeWorkspace *workspace)
{
    FlacStreamDescriptor descriptor = {};
    esp_err_t ret = flac_decoder_prepare_open_descriptor(decoder, source, &descriptor);
    if (ret != ESP_OK) {
        return ret;
    }

    // 普通起播仍把完整原始 FLAC（含全部 metadata）交给乐鑫 parser，行为与既有稳定路径一致。
    return flac_decoder_prepare_runtime(
        decoder,
        workspace,
        descriptor.flac_offset_bytes,
        nullptr,
        0,
        0);
}

esp_err_t flac_decoder_open_streaming(
    FlacDecoder *decoder,
    AudioSource *source,
    AudioDecodeWorkspace *workspace)
{
    if (decoder == nullptr || !audio_source_is_open(source)) return ESP_ERR_INVALID_ARG;
    flac_decoder_close(decoder);
    flac_decoder_clear_nas_seek_cache();
    esp_err_t ret = flac_decoder_register_backend();
    if (ret != ESP_OK) return ret;

    decoder->source = source;
    FlacStreamDescriptor descriptor = {};
    ret = flac_parse_stream_descriptor_streaming(source, &descriptor);
    if (ret != ESP_OK) {
        flac_decoder_close(decoder);
        return ret;
    }
    flac_apply_stream_descriptor(decoder, descriptor);

    uint8_t synthetic_header[FLAC_SYNTHETIC_HEADER_BYTES] = {};
    flac_build_synthetic_header(descriptor, synthetic_header);
    ret = flac_decoder_prepare_runtime(
        decoder,
        workspace,
        descriptor.audio_data_offset_bytes,
        synthetic_header,
        sizeof(synthetic_header),
        0U,
        true);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
            "NAS FLAC直接流式解码已就绪：%luHz/%ubit/%u声道 input=%uB pcm=%uB，无FlacPrefetch",
            static_cast<unsigned long>(decoder->sample_rate_hz),
            static_cast<unsigned>(decoder->bits_per_sample),
            static_cast<unsigned>(decoder->channels),
            static_cast<unsigned>(decoder->input_capacity),
            static_cast<unsigned>(decoder->decoded_capacity));
    }
    return ret;
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
                // 本次调用若已经复制出有效 PCM，不能因为下一压缩块暂时迟到就把这些帧丢掉。
                // 先把已产出的 PCM 交给 AudioTask；若下一轮仍欠载，再进入统一的有界恢复。
                if (ret == ESP_ERR_TIMEOUT && produced > 0) {
                    *out_frames = produced;
                    return ESP_OK;
                }
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
        fakepod_pcm::to_i2s32(src, out_interleaved_stereo + produced * 2U,
            copy_frames, decoder->bits_per_sample, decoder->channels);

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

esp_err_t flac_decoder_set_nas_seek_scan_window(FlacNasSeekPlan *plan, uint32_t guard_bytes)
{
    if (plan == nullptr || guard_bytes < 1024U || plan->scan_min_offset < plan->audio_offset ||
        plan->scan_max_offset <= plan->scan_min_offset || plan->scan_estimate_offset < plan->scan_min_offset ||
        plan->scan_estimate_offset > plan->scan_max_offset) {
        if (plan != nullptr) plan->scan_required = false;
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t guard = guard_bytes;
    const uint64_t start = plan->scan_estimate_offset > guard
        ? plan->scan_estimate_offset - guard : plan->scan_min_offset;
    const uint64_t end = plan->scan_estimate_offset > UINT64_MAX - guard
        ? plan->scan_max_offset : plan->scan_estimate_offset + guard;
    plan->scan_start_offset = start > plan->scan_min_offset ? start : plan->scan_min_offset;
    plan->scan_end_offset = end < plan->scan_max_offset ? end : plan->scan_max_offset;
    const uint64_t length = plan->scan_end_offset > plan->scan_start_offset
        ? plan->scan_end_offset - plan->scan_start_offset : 0U;
    plan->scan_required = length >= 64U && length <= 512ULL * 1024ULL;
    return plan->scan_required ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t flac_decoder_plan_nas_seek_cached(uint64_t target_ms, FlacNasSeekPlan *out_plan)
{
    if (out_plan == nullptr || target_ms == 0U || !g_nas_flac_seek_cache.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    const FlacStreamDescriptor &d = g_nas_flac_seek_cache.descriptor;
    if (d.sample_rate_hz == 0U || target_ms > UINT64_MAX / d.sample_rate_hz) return ESP_ERR_INVALID_ARG;
    const uint64_t target_sample = (target_ms * d.sample_rate_hz) / 1000ULL;
    if (target_sample == 0U || (d.total_frames != 0U && target_sample >= d.total_frames)) return ESP_ERR_INVALID_ARG;

    if (g_nas_flac_seek_cache.points == nullptr || g_nas_flac_seek_cache.count == 0U) {
        if (d.audio_data_offset_bytes >= d.file_size_bytes || d.total_frames == 0U) return ESP_ERR_NOT_SUPPORTED;
        *out_plan = {};
        memcpy(out_plan->streaminfo, d.streaminfo_payload, FLAC_STREAMINFO_BYTES);
        out_plan->file_bytes = d.file_size_bytes;
        out_plan->audio_offset = d.audio_data_offset_bytes;
        out_plan->target_sample = target_sample;
        out_plan->requested_target_sample = target_sample;
        out_plan->sample_rate_hz = d.sample_rate_hz;
        out_plan->no_seektable = true;
        out_plan->scan_min_offset = d.audio_data_offset_bytes;
        out_plan->scan_max_offset = d.file_size_bytes;
        out_plan->scan_min_sample = 0U;
        out_plan->scan_max_sample = d.total_frames;
        const uint64_t audio_bytes = d.file_size_bytes - d.audio_data_offset_bytes;
        const uint64_t remainder = audio_bytes % d.total_frames;
        if (remainder != 0U && target_sample > UINT64_MAX / remainder) return ESP_ERR_NOT_SUPPORTED;
        out_plan->scan_estimate_offset = d.audio_data_offset_bytes +
            (audio_bytes / d.total_frames) * target_sample +
            (remainder * target_sample) / d.total_frames;
        out_plan->requested_target_sample = target_sample;
        ESP_LOGD(TAG, "NAS_FLAC_FAST_SEEK: estimate=linear target=%llu offset=%llu audio=%llu bytes=%llu",
            static_cast<unsigned long long>(target_sample),
            static_cast<unsigned long long>(out_plan->scan_estimate_offset),
            static_cast<unsigned long long>(d.audio_data_offset_bytes),
            static_cast<unsigned long long>(audio_bytes));
        // Begin with a wider local window. Fast approximate seek does not rely on
        // dynamic-anchor cache hits; a verified frame close to the requested time wins.
        return flac_decoder_set_nas_seek_scan_window(out_plan, 128U * 1024U);
    }

    const FlacSeekPoint *previous = nullptr;
    const FlacSeekPoint *next = nullptr;
    for (uint32_t i = 0; i < g_nas_flac_seek_cache.count; ++i) {
        const FlacSeekPoint &p = g_nas_flac_seek_cache.points[i];
        if (p.sample_number <= target_sample &&
            (previous == nullptr || p.sample_number > previous->sample_number)) previous = &p;
        if (p.sample_number > target_sample &&
            (next == nullptr || p.sample_number < next->sample_number)) next = &p;
    }
    if (previous == nullptr || d.audio_data_offset_bytes >= d.file_size_bytes ||
        previous->stream_offset >= d.file_size_bytes - d.audio_data_offset_bytes ||
        target_sample - previous->sample_number > static_cast<uint64_t>(d.sample_rate_hz) * 30ULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out_plan = {};
    memcpy(out_plan->streaminfo, d.streaminfo_payload, FLAC_STREAMINFO_BYTES);
    out_plan->file_bytes = d.file_size_bytes;
    out_plan->audio_offset = d.audio_data_offset_bytes;
    out_plan->seek_offset = d.audio_data_offset_bytes + previous->stream_offset;
    out_plan->anchor_sample = previous->sample_number;
    out_plan->target_sample = target_sample;
    out_plan->requested_target_sample = target_sample;
    out_plan->seektable_offset = d.seektable_offset_bytes;
    out_plan->seektable_length_bytes = d.seektable_length_bytes;
    out_plan->sample_rate_hz = d.sample_rate_hz;

    // Interpolation chooses only a bounded scan window. The final anchor must be a real
    // frame header with a valid FLAC header CRC8, never the interpolated byte position.
    const uint64_t block_limit = static_cast<uint64_t>(d.max_block_size != 0U ? d.max_block_size : 4096U) * 2ULL;
    const uint64_t audio_bytes = d.file_size_bytes - d.audio_data_offset_bytes;
    const uint64_t next_sample = next != nullptr ? next->sample_number : d.total_frames;
    const uint64_t next_stream_offset = next != nullptr ? next->stream_offset : audio_bytes;
    if (target_sample - previous->sample_number > block_limit &&
        next_sample > previous->sample_number && next_stream_offset > previous->stream_offset &&
        next_stream_offset <= audio_bytes && next_sample >= target_sample) {
        const uint64_t sample_span = next_sample - previous->sample_number;
        const uint64_t byte_span = next_stream_offset - previous->stream_offset;
        const uint64_t sample_delta = target_sample - previous->sample_number;
        if (sample_span != 0U && byte_span != 0U && sample_delta <= UINT64_MAX / byte_span) {
            const uint64_t previous_abs = d.audio_data_offset_bytes + previous->stream_offset;
            const uint64_t next_abs = d.audio_data_offset_bytes + next_stream_offset;
            const uint64_t estimate = previous_abs + (byte_span * sample_delta) / sample_span;
            out_plan->scan_estimate_offset = estimate;
            out_plan->scan_min_offset = previous_abs;
            out_plan->scan_max_offset = next_abs;
            out_plan->scan_min_sample = previous->sample_number;
            out_plan->scan_max_sample = next_sample;
            // Start with a small +/-64KB window. The caller expands it only if
            // the local frame scan cannot find a close enough valid frame.
            (void)flac_decoder_set_nas_seek_scan_window(out_plan, 64U * 1024U);
        }
    }
    ESP_LOGD(TAG,
        "NAS_FLAC_SEEK_CACHE: hit=1 points=%lu target=%llu anchor=%llu gap_ms=%llu offset=%llu refine=%u scan=%llu..%llu estimate=%llu",
        static_cast<unsigned long>(g_nas_flac_seek_cache.count),
        static_cast<unsigned long long>(target_sample),
        static_cast<unsigned long long>(previous->sample_number),
        static_cast<unsigned long long>(((target_sample - previous->sample_number) * 1000ULL) / d.sample_rate_hz),
        static_cast<unsigned long long>(out_plan->seek_offset), out_plan->scan_required ? 1U : 0U,
        static_cast<unsigned long long>(out_plan->scan_start_offset),
        static_cast<unsigned long long>(out_plan->scan_end_offset),
        static_cast<unsigned long long>(out_plan->scan_estimate_offset));
    return ESP_OK;
}

esp_err_t flac_decoder_calibrate_nas_seek_no_table(FlacNasSeekPlan *plan)
{
    if (plan == nullptr || !plan->no_seektable || !plan->observed_valid ||
        plan->sample_rate_hz == 0U || plan->scan_max_sample == 0U ||
        plan->scan_max_offset <= plan->scan_min_offset) return ESP_ERR_NOT_SUPPORTED;
    // Correct the byte guess using an actual CRC8-validated FLAC frame header.
    // Proportional extrapolation is bounded; subsequent probes re-calibrate it.
    const uint64_t audio_bytes = plan->scan_max_offset - plan->scan_min_offset;
    const uint64_t bytes_per_sample = audio_bytes / plan->scan_max_sample;
    const uint64_t delta = plan->target_sample > plan->observed_sample
        ? plan->target_sample - plan->observed_sample
        : plan->observed_sample - plan->target_sample;
    if (delta > UINT64_MAX / (bytes_per_sample + 1U)) return ESP_ERR_NOT_SUPPORTED;
    const uint64_t remainder = audio_bytes % plan->scan_max_sample;
    if (remainder != 0U && delta > UINT64_MAX / remainder) return ESP_ERR_NOT_SUPPORTED;
    const uint64_t shift = delta * bytes_per_sample +
        (delta * remainder) / plan->scan_max_sample;
    const uint64_t candidate = plan->target_sample > plan->observed_sample
        ? (shift > UINT64_MAX - plan->observed_offset ? UINT64_MAX : plan->observed_offset + shift)
        : (shift > plan->observed_offset ? 0U : plan->observed_offset - shift);
    uint64_t clamped = candidate < plan->scan_min_offset ? plan->scan_min_offset
        : (candidate >= plan->scan_max_offset ? plan->scan_max_offset - 1U : candidate);
    bool shifted_out_of_window = false;
    // A proportional correction can land inside the exact window that just missed.
    // Do not terminate the seek in that case: move one bounded window beyond the
    // scanned region in the direction indicated by the observed frame sample.
    if (clamped >= plan->scan_start_offset && clamped < plan->scan_end_offset) {
        constexpr uint64_t kNoTableProbeStep = 64U * 1024U;
        if (plan->observed_sample <= plan->target_sample) {
            clamped = plan->scan_end_offset > UINT64_MAX - kNoTableProbeStep
                ? plan->scan_max_offset - 1U : plan->scan_end_offset + kNoTableProbeStep;
        } else {
            clamped = plan->scan_start_offset > plan->scan_min_offset + kNoTableProbeStep
                ? plan->scan_start_offset - kNoTableProbeStep : plan->scan_min_offset;
        }
        if (clamped >= plan->scan_max_offset) clamped = plan->scan_max_offset - 1U;
        shifted_out_of_window = clamped < plan->scan_start_offset || clamped >= plan->scan_end_offset;
    }
    ESP_LOGD(TAG, "NAS_FLAC_NO_TABLE: calibrate observed=%llu at=%llu next=%llu target=%llu shift=%u",
        static_cast<unsigned long long>(plan->observed_sample),
        static_cast<unsigned long long>(plan->observed_offset),
        static_cast<unsigned long long>(clamped),
        static_cast<unsigned long long>(plan->target_sample), shifted_out_of_window ? 1U : 0U);
    if (clamped >= plan->scan_start_offset && clamped < plan->scan_end_offset)
        return ESP_ERR_NOT_FOUND;
    plan->scan_estimate_offset = clamped;
    plan->observed_valid = false;
    return ESP_OK;
}

esp_err_t flac_decoder_refine_nas_seek_range(
    AudioSource *source, FlacNasSeekPlan *in_out_plan,
    uint8_t **out_replay_buffer, size_t *out_replay_bytes, uint64_t *out_scanned_bytes)
{
    if (out_replay_buffer != nullptr) *out_replay_buffer = nullptr;
    if (out_replay_bytes != nullptr) *out_replay_bytes = 0U;
    if (out_scanned_bytes != nullptr) *out_scanned_bytes = 0U;
    if (!audio_source_is_open(source) || in_out_plan == nullptr || out_replay_buffer == nullptr ||
        out_replay_bytes == nullptr || !in_out_plan->scan_required ||
        in_out_plan->scan_start_offset < in_out_plan->audio_offset ||
        in_out_plan->scan_end_offset <= in_out_plan->scan_start_offset ||
        in_out_plan->scan_end_offset > in_out_plan->file_bytes || !g_nas_flac_seek_cache.valid) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t length64 = in_out_plan->scan_end_offset - in_out_plan->scan_start_offset;
    constexpr size_t kMaxScanBytes = 512U * 1024U;
    if (length64 < 64U || length64 > kMaxScanBytes) return ESP_ERR_INVALID_SIZE;
    const size_t scan_length = static_cast<size_t>(length64);
    uint8_t *scan = static_cast<uint8_t *>(heap_caps_malloc(scan_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scan == nullptr) return ESP_ERR_NO_MEM;

    size_t filled = 0U;
    size_t scan_cursor = 0U;
    uint32_t empty_retries = 0U;
    bool reached_after_target = false;
    FlacSeekPoint best = {};
    size_t best_index = 0U;
    bool found = false;
    uint64_t nearest_distance = UINT64_MAX;
    esp_err_t ret = ESP_OK;
    const FlacStreamDescriptor &d = g_nas_flac_seek_cache.descriptor;

    // Read and inspect incrementally. Stop shortly after the first valid frame beyond
    // target rather than always downloading the full 512KB window. Keep 32 bytes of tail
    // so a FLAC header split across two reads is still validated as one candidate.
    while (filled < scan_length && empty_retries < 50U) {
        const size_t chunk = scan_length - filled < FLAC_INPUT_BUFFER_BYTES
            ? scan_length - filled : FLAC_INPUT_BUFFER_BYTES;
        size_t got = 0U;
        ret = audio_source_read(source, scan + filled, chunk, &got);
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) break;
        if (got == 0U) {
            if (audio_source_eof(source)) break;
            ++empty_retries;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        filled += got;
        empty_retries = 0U;
        const size_t safe_end = filled == scan_length || audio_source_eof(source)
            ? filled : (filled > FLAC_FRAME_HEADER_MAX_BYTES ? filled - FLAC_FRAME_HEADER_MAX_BYTES : 0U);
        while (scan_cursor < safe_end) {
            const size_t i = scan_cursor++;
            if (i + 6U > filled || scan[i] != 0xFFU || (scan[i + 1U] & 0xFEU) != 0xF8U) continue;
            uint64_t sample = 0U;
            if (!flac_parse_frame_sample_number(scan + i, filled - i, d, &sample)) continue;
            if (in_out_plan->no_seektable) {
                const uint64_t distance = sample > in_out_plan->target_sample
                    ? sample - in_out_plan->target_sample : in_out_plan->target_sample - sample;
                if (distance < nearest_distance) {
                    nearest_distance = distance;
                    in_out_plan->observed_valid = true;
                    in_out_plan->observed_sample = sample;
                    in_out_plan->observed_offset = in_out_plan->scan_start_offset + i;
                }
            }
            if (sample > in_out_plan->target_sample) {
                reached_after_target = true;
                break;
            }
            if (sample <= in_out_plan->scan_min_sample || sample >= in_out_plan->scan_max_sample) continue;
            if (!found || sample > best.sample_number) {
                best.sample_number = sample;
                best.stream_offset = (in_out_plan->scan_start_offset - in_out_plan->audio_offset) + i;
                best.frame_samples = 0U;
                best_index = i;
                found = true;
            }
        }
        if (reached_after_target) break;
    }

    if (out_scanned_bytes != nullptr) *out_scanned_bytes = filled;
    // Persist the nearest CRC8-validated frame even when this scan misses.
    // Subsequent SEEK requests can interpolate between such real observations.
    if (in_out_plan->no_seektable && in_out_plan->observed_valid) {
        flac_nas_remember_observed_anchor(in_out_plan->observed_sample,
            in_out_plan->observed_offset);
    }
    if ((ret != ESP_OK && ret != ESP_ERR_TIMEOUT) || filled < 64U) {
        heap_caps_free(scan);
        return ret != ESP_OK && ret != ESP_ERR_TIMEOUT ? ret : ESP_ERR_NOT_FOUND;
    }
    // If the window ended before seeing a frame after target, inspect the final tail.
    // The previous loop deliberately deferred this tail until more bytes arrived.
    while (!reached_after_target && scan_cursor + 6U <= filled) {
        const size_t i = scan_cursor++;
        if (scan[i] != 0xFFU || (scan[i + 1U] & 0xFEU) != 0xF8U) continue;
        uint64_t sample = 0U;
        if (!flac_parse_frame_sample_number(scan + i, filled - i, d, &sample)) continue;
        if (in_out_plan->no_seektable) {
            const uint64_t distance = sample > in_out_plan->target_sample
                ? sample - in_out_plan->target_sample : in_out_plan->target_sample - sample;
            if (distance < nearest_distance) {
                nearest_distance = distance;
                in_out_plan->observed_valid = true;
                in_out_plan->observed_sample = sample;
                in_out_plan->observed_offset = in_out_plan->scan_start_offset + i;
            }
        }
        if (sample > in_out_plan->target_sample) {
            reached_after_target = true;
            break;
        }
        if (sample <= in_out_plan->scan_min_sample || sample >= in_out_plan->scan_max_sample) continue;
        if (!found || sample > best.sample_number) {
            best.sample_number = sample;
            best.stream_offset = (in_out_plan->scan_start_offset - in_out_plan->audio_offset) + i;
            best.frame_samples = 0U;
            best_index = i;
            found = true;
        }
    }

    const uint64_t max_gap = in_out_plan->no_seektable
        ? static_cast<uint64_t>(in_out_plan->sample_rate_hz) * 2ULL
        : static_cast<uint64_t>(d.max_block_size != 0U ? d.max_block_size : 4096U) * 2ULL;
    if (in_out_plan->no_seektable && in_out_plan->observed_valid && nearest_distance <= max_gap &&
        in_out_plan->observed_offset >= in_out_plan->scan_start_offset &&
        in_out_plan->observed_offset < in_out_plan->scan_start_offset + filled) {
        // Approximate mode: the frame header was validated (including CRC8), and
        // the nearest frame is within the accepted 2-second error budget. Start there
        // instead of decoding/discarding PCM to an exact sample boundary.
        best.sample_number = in_out_plan->observed_sample;
        best.stream_offset = in_out_plan->observed_offset - in_out_plan->audio_offset;
        best.frame_samples = 0U;
        best_index = static_cast<size_t>(in_out_plan->observed_offset - in_out_plan->scan_start_offset);
        found = true;
    }
    if (!found || best_index >= filled ||
        (!in_out_plan->no_seektable && (in_out_plan->target_sample < best.sample_number ||
        in_out_plan->target_sample - best.sample_number > max_gap))) {
        ESP_LOGD(TAG, "NAS_FLAC_SEEK_REFINE: found=0 scanned=%uB estimate=%llu fallback_anchor=%llu nearest_delta_ms=%llu",
            static_cast<unsigned>(filled), static_cast<unsigned long long>(in_out_plan->scan_estimate_offset),
            static_cast<unsigned long long>(in_out_plan->anchor_sample),
            static_cast<unsigned long long>(nearest_distance == UINT64_MAX ? UINT64_MAX :
                (nearest_distance * 1000ULL) / (in_out_plan->sample_rate_hz != 0U ? in_out_plan->sample_rate_hz : 1U)));
        heap_caps_free(scan);
        return ESP_ERR_NOT_FOUND;
    }

    const uint64_t frame_absolute = in_out_plan->audio_offset + best.stream_offset;
    if (in_out_plan->no_seektable) {
        flac_nas_remember_observed_anchor(best.sample_number, frame_absolute);
        in_out_plan->target_sample = best.sample_number;
        in_out_plan->anchor_sample = best.sample_number;
    }
    const size_t replay_bytes = filled - best_index;
    memmove(scan, scan + best_index, replay_bytes);
    in_out_plan->seek_offset = frame_absolute;
    in_out_plan->anchor_sample = best.sample_number;
    *out_replay_buffer = scan;
    *out_replay_bytes = replay_bytes;
    const uint64_t requested_sample = in_out_plan->requested_target_sample != 0U
        ? in_out_plan->requested_target_sample : in_out_plan->target_sample;
    const uint64_t snap_delta = requested_sample > best.sample_number
        ? requested_sample - best.sample_number : best.sample_number - requested_sample;
    ESP_LOGD(TAG,
        "NAS_FLAC_SEEK_REFINE: found=1 mode=%s requested=%llu actual=%llu error_ms=%llu frame_offset=%llu anchor=%llu discarded=%llu scanned=%uB replay=%uB",
        in_out_plan->no_seektable ? "approx" : "precise",
        static_cast<unsigned long long>(requested_sample),
        static_cast<unsigned long long>(best.sample_number),
        static_cast<unsigned long long>((snap_delta * 1000ULL) / in_out_plan->sample_rate_hz),
        static_cast<unsigned long long>(frame_absolute), static_cast<unsigned long long>(best.sample_number),
        static_cast<unsigned long long>(in_out_plan->no_seektable ? 0U : in_out_plan->target_sample - best.sample_number),
        static_cast<unsigned>(filled), static_cast<unsigned>(replay_bytes));
    return ESP_OK;
}

esp_err_t flac_decoder_plan_nas_seek(AudioSource *source, uint64_t target_ms,
                                      FlacNasSeekPlan *out_plan)
{
    if (source == nullptr || out_plan == nullptr || target_ms == 0U) return ESP_ERR_INVALID_ARG;
    FlacStreamDescriptor descriptor = {};
    const esp_err_t ret = flac_parse_stream_descriptor_streaming(
        source, &descriptor, out_plan, target_ms);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG,
            "NAS_FLAC_SEEK_PLAN: target=%llu anchor=%llu gap_ms=%llu offset=%llu audio=%llu points=%lu",
            static_cast<unsigned long long>(out_plan->target_sample),
            static_cast<unsigned long long>(out_plan->anchor_sample),
            static_cast<unsigned long long>(((out_plan->target_sample - out_plan->anchor_sample) * 1000ULL) / out_plan->sample_rate_hz),
            static_cast<unsigned long long>(out_plan->seek_offset),
            static_cast<unsigned long long>(out_plan->audio_offset),
            static_cast<unsigned long>(descriptor.seektable_length_bytes / FLAC_SEEKPOINT_BYTES));
    }
    return ret;
}

esp_err_t flac_decoder_open_streaming_at_nas_seek(FlacDecoder *decoder, AudioSource *source,
    AudioDecodeWorkspace *workspace, const FlacNasSeekPlan &plan)
{
    if (decoder == nullptr || source == nullptr || !audio_source_is_open(source) ||
        plan.sample_rate_hz == 0U || plan.target_sample < plan.anchor_sample ||
        (!plan.no_seektable && (plan.seektable_length_bytes < FLAC_SEEKPOINT_BYTES ||
        plan.seektable_length_bytes % FLAC_SEEKPOINT_BYTES != 0U ||
        plan.seektable_offset >= plan.audio_offset ||
        plan.seektable_length_bytes > plan.audio_offset - plan.seektable_offset)) ||
        plan.seek_offset < plan.audio_offset || plan.seek_offset >= plan.file_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    flac_decoder_close(decoder);
    esp_err_t ret = flac_decoder_register_backend();
    if (ret != ESP_OK) return ret;
    FlacStreamDescriptor descriptor = {};
    descriptor.file_size_bytes = plan.file_bytes;
    descriptor.audio_data_offset_bytes = plan.audio_offset;
    descriptor.seektable_offset_bytes = plan.seektable_offset;
    descriptor.seektable_length_bytes = plan.seektable_length_bytes;
    memcpy(descriptor.streaminfo_payload, plan.streaminfo, FLAC_STREAMINFO_BYTES);
    ret = flac_decode_streaminfo_payload(&descriptor);
    if (ret != ESP_OK || descriptor.sample_rate_hz != plan.sample_rate_hz ||
        (descriptor.total_frames != 0U && plan.target_sample >= descriptor.total_frames)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    decoder->source = source;
    flac_apply_stream_descriptor(decoder, descriptor);
    uint8_t synthetic_header[FLAC_SYNTHETIC_HEADER_BYTES] = {};
    flac_build_synthetic_header(descriptor, synthetic_header);
    ret = flac_decoder_prepare_runtime(decoder, workspace, plan.seek_offset,
        synthetic_header, sizeof(synthetic_header), plan.anchor_sample, true);
    if (ret == ESP_OK) {
        decoder->preplay_decode_active = true;
        ret = flac_decoder_discard_to_frame(decoder, plan.target_sample);
        decoder->preplay_decode_active = false;
    }
    if (ret != ESP_OK) {
        flac_decoder_close(decoder);
    } else {
        ESP_LOGI(TAG, "NAS_FLAC_SEEK: target=%llu anchor=%llu discarded=%llu offset=%llu retained_seekpoints=%lu",
            static_cast<unsigned long long>(plan.target_sample),
            static_cast<unsigned long long>(plan.anchor_sample),
            static_cast<unsigned long long>(plan.target_sample - plan.anchor_sample),
            static_cast<unsigned long long>(plan.seek_offset),
            static_cast<unsigned long>(plan.seektable_length_bytes / FLAC_SEEKPOINT_BYTES));
    }
    return ret;
}

static esp_err_t flac_decoder_prepare_seek_runtime(
    FlacDecoder *decoder,
    AudioDecodeWorkspace *workspace,
    const FlacStreamDescriptor &descriptor,
    uint64_t target_frame,
    uint64_t *out_source_offset)
{
    if (decoder == nullptr || decoder->source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool has_seektable = descriptor.seektable_length_bytes >= FLAC_SEEKPOINT_BYTES &&
        descriptor.seektable_length_bytes % FLAC_SEEKPOINT_BYTES == 0U;
    if (!has_seektable && !flac_no_table_seekable(descriptor)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (descriptor.audio_data_offset_bytes >= descriptor.file_size_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

#if APP_DIAG_AUDIO_SEEK
    const int64_t seek_stage_begin_us = esp_timer_get_time();
#endif
    FlacSeekPoint point = {};
    FlacSeekPoint next_point = {};
    bool has_next_point = false;
    uint64_t no_table_scan_bytes = 0U;
    esp_err_t ret = has_seektable
        ? flac_select_seekpoint(
            decoder->source, descriptor, target_frame, &point, &next_point, &has_next_point)
        : flac_find_no_table_seekpoint(
            decoder->source, workspace, descriptor, target_frame, &point, &no_table_scan_bytes);
#if APP_DIAG_AUDIO_SEEK
    const int64_t seekpoint_done_us = esp_timer_get_time();
#endif
    if (ret != ESP_OK) {
        return ret;
    }

    // SEEKTABLE 只给稀疏锚点；在相邻锚点之间只扫描压缩帧头，不解 PCM。
    // 先利用前后 seekpoint 的 sample/byte 比例估算目标附近字节位置，再向前留出
    // 128~256KB 保护窗口做局部扫描。估算只用于缩短扫描区间，最终锚点仍必须通过
    // FLAC 帧头 sample/frame number + CRC8 校验；局部扫描失败时自动回退完整区间扫描。
#if APP_DIAG_AUDIO_SEEK
    const FlacSeekPoint table_point = point;
#endif
    uint64_t refine_scanned_bytes = no_table_scan_bytes;
    bool refine_used_estimate = !has_seektable;
#if APP_DIAG_AUDIO_SEEK
    bool refine_fell_back = false;
#endif
    const uint64_t audio_bytes = descriptor.file_size_bytes - descriptor.audio_data_offset_bytes;
    const uint64_t scan_end = has_next_point ? next_point.stream_offset : audio_bytes;
    const uint64_t max_sample = has_next_point ? next_point.sample_number : descriptor.total_frames;
    const bool discard_exceeds_one_block = descriptor.max_block_size == 0U ||
        target_frame - point.sample_number > descriptor.max_block_size;
    if (has_seektable && discard_exceeds_one_block && max_sample > target_frame &&
        scan_end > point.stream_offset + 1U) {
        uint64_t scan_start = point.stream_offset + 1U;
        const uint64_t sample_span = max_sample - point.sample_number;
        const uint64_t target_delta = target_frame - point.sample_number;
        const uint64_t byte_span = scan_end - point.stream_offset;

        if (sample_span != 0U && target_delta < sample_span && byte_span != 0U &&
            target_delta <= UINT64_MAX / byte_span) {
            const uint64_t estimated_delta = (byte_span * target_delta) / sample_span;
            uint64_t guard_bytes = 128U * 1024U;
            if (descriptor.max_frame_size != 0U) {
                const uint64_t frame_guard =
                    static_cast<uint64_t>(descriptor.max_frame_size) * 8U;
                if (frame_guard > guard_bytes) {
                    guard_bytes = frame_guard;
                }
            }
            const uint64_t max_guard_bytes = 256U * 1024U;
            if (guard_bytes > max_guard_bytes) {
                guard_bytes = max_guard_bytes;
            }

            if (estimated_delta > guard_bytes + 1U) {
                const uint64_t estimated_start =
                    point.stream_offset + estimated_delta - guard_bytes;
                if (estimated_start > scan_start && estimated_start < scan_end) {
                    scan_start = estimated_start;
                    refine_used_estimate = true;
                }
            }
        }

        FlacSeekPoint refined_point = point;
        uint64_t local_scanned_bytes = 0U;
        bool refined = flac_refine_seekpoint_by_frame_scan(
            decoder->source, workspace, descriptor, target_frame, scan_start, scan_end, max_sample,
            &refined_point, &local_scanned_bytes);
        refine_scanned_bytes = local_scanned_bytes;

        if (!refined && refine_used_estimate) {
            // 局部窗口若因码率波动落在目标之后，恢复旧的完整区间扫描，
            // 保证 Phase 2 已有的正确性与性能下限不被新估算路径破坏。
#if APP_DIAG_AUDIO_SEEK
            refine_fell_back = true;
#endif
            refined_point = point;
            uint64_t fallback_scanned_bytes = 0U;
            refined = flac_refine_seekpoint_by_frame_scan(
                decoder->source, workspace, descriptor, target_frame, point.stream_offset + 1U,
                scan_end, max_sample, &refined_point, &fallback_scanned_bytes);
            refine_scanned_bytes += fallback_scanned_bytes;
        }

        if (refined) {
            point = refined_point;
        }
    }
#if APP_DIAG_AUDIO_SEEK
    const int64_t refine_done_us = esp_timer_get_time();
#endif
    if (point.stream_offset > descriptor.file_size_bytes - descriptor.audio_data_offset_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t absolute_source_offset = descriptor.audio_data_offset_bytes + point.stream_offset;
    if (absolute_source_offset >= descriptor.file_size_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t synthetic_header[FLAC_SYNTHETIC_HEADER_BYTES] = {};
    flac_build_synthetic_header(descriptor, synthetic_header);

    flac_apply_stream_descriptor(decoder, descriptor);
    ret = flac_decoder_prepare_runtime(
        decoder,
        workspace,
        absolute_source_offset,
        synthetic_header,
        sizeof(synthetic_header),
        point.sample_number);
#if APP_DIAG_AUDIO_SEEK
    const int64_t runtime_ready_us = esp_timer_get_time();
#endif
    if (ret != ESP_OK) {
        return ret;
    }

    // 定位锚点只负责跳到目标之前的合法帧；剩余距离在 I2S/DAC 启动前直接丢弃 PCM。
    // 此阶段没有 DMA underrun 截止时间，允许在预取环暂时耗尽时按起播预算等待后台补充。
    decoder->preplay_decode_active = true;
    ret = flac_decoder_discard_to_frame(decoder, target_frame);
    decoder->preplay_decode_active = false;
#if APP_DIAG_AUDIO_SEEK
    const int64_t discard_done_us = esp_timer_get_time();
#endif
    if (ret != ESP_OK) {
        flac_decoder_close(decoder);
        return ret;
    }

#if APP_DIAG_AUDIO_SEEK
    ESP_LOGI(TAG,
        "FLAC_SEEK_STAGE: select=%lldms refine=%lldms runtime=%lldms discard=%lldms total=%lldms",
        static_cast<long long>((seekpoint_done_us - seek_stage_begin_us) / 1000LL),
        static_cast<long long>((refine_done_us - seekpoint_done_us) / 1000LL),
        static_cast<long long>((runtime_ready_us - refine_done_us) / 1000LL),
        static_cast<long long>((discard_done_us - runtime_ready_us) / 1000LL),
        static_cast<long long>((discard_done_us - seek_stage_begin_us) / 1000LL));
    ESP_LOGI(TAG,
        "FLAC_SEEK_TRACE: target=%llu table=%llu refined=%llu saved=%llu scan=%lluB estimate=%u fallback=%u stream_offset=%llu absolute=%llu discard=%llu seekpoints=%lu",
        static_cast<unsigned long long>(target_frame),
        static_cast<unsigned long long>(table_point.sample_number),
        static_cast<unsigned long long>(point.sample_number),
        static_cast<unsigned long long>(point.sample_number - table_point.sample_number),
        static_cast<unsigned long long>(refine_scanned_bytes),
        refine_used_estimate ? 1U : 0U,
        refine_fell_back ? 1U : 0U,
        static_cast<unsigned long long>(point.stream_offset),
        static_cast<unsigned long long>(absolute_source_offset),
        static_cast<unsigned long long>(target_frame - point.sample_number),
        static_cast<unsigned long>(descriptor.seektable_length_bytes / FLAC_SEEKPOINT_BYTES));
#endif

    if (out_source_offset != nullptr) {
        *out_source_offset = absolute_source_offset;
    }
    return ESP_OK;
}

esp_err_t flac_decoder_open_at_ms(
    FlacDecoder *decoder,
    AudioSource *source,
    AudioDecodeWorkspace *workspace,
    uint64_t target_ms,
    uint64_t *out_target_frame,
    uint64_t *out_source_offset)
{
    if (out_target_frame != nullptr) {
        *out_target_frame = 0;
    }
    if (out_source_offset != nullptr) {
        *out_source_offset = 0;
    }

    FlacStreamDescriptor descriptor = {};
    esp_err_t ret = flac_decoder_prepare_open_descriptor(decoder, source, &descriptor);
    if (ret != ESP_OK) {
        return ret;
    }
    if (descriptor.sample_rate_hz == 0 ||
        target_ms > UINT64_MAX / static_cast<uint64_t>(descriptor.sample_rate_hz)) {
        flac_decoder_close(decoder);
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t target_frame =
        (target_ms * static_cast<uint64_t>(descriptor.sample_rate_hz)) / 1000ULL;
    if (descriptor.total_frames != 0 && target_frame >= descriptor.total_frames) {
        flac_decoder_close(decoder);
        return ESP_ERR_INVALID_ARG;
    }

    if (target_frame == 0) {
        ret = flac_decoder_prepare_runtime(
            decoder,
            workspace,
            descriptor.flac_offset_bytes,
            nullptr,
            0,
            0);
        if (ret == ESP_OK && out_source_offset != nullptr) {
            *out_source_offset = descriptor.flac_offset_bytes;
        }
    } else {
        ret = flac_decoder_prepare_seek_runtime(
            decoder,
            workspace,
            descriptor,
            target_frame,
            out_source_offset);
    }
    if (ret != ESP_OK) {
        flac_decoder_close(decoder);
        return ret;
    }
    if (out_target_frame != nullptr) {
        *out_target_frame = target_frame;
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
    if (decoder->streaming_source ||
        (!flac_decoder_has_seektable(decoder) && !flac_decoder_can_seek_without_table(decoder))) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (decoder->total_frames != 0 && target_frame >= decoder->total_frames) {
        return ESP_ERR_INVALID_ARG;
    }

    AudioSource *source = decoder->source;
    AudioDecodeWorkspace *workspace = decoder->workspace;
    FlacStreamDescriptor descriptor = {};
    flac_capture_stream_descriptor(decoder, &descriptor);

    // 先安全停止旧 Prefetch，再直接复用已经解析过的 STREAMINFO/SEEKTABLE 描述。
    // 不重新扫描 metadata，也不在同一次 Seek 内建立两套 Prefetch。
    flac_decoder_close(decoder);
    decoder->source = source;

    const esp_err_t ret = flac_decoder_prepare_seek_runtime(
        decoder,
        workspace,
        descriptor,
        target_frame,
        out_source_offset);
    if (ret != ESP_OK) {
        flac_decoder_close(decoder);
        return ret;
    }

    if (out_actual_frame != nullptr) {
        *out_actual_frame = target_frame;
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

bool flac_decoder_can_seek_without_table(const FlacDecoder *decoder)
{
    return decoder != nullptr && !decoder->streaming_source &&
        decoder->total_frames > 0U && decoder->max_block_size > 0U &&
        decoder->audio_data_offset_bytes < decoder->file_size_bytes;
}

bool flac_decoder_has_seektable(const FlacDecoder *decoder)
{
    return decoder != nullptr &&
        decoder->seektable_length_bytes >= FLAC_SEEKPOINT_BYTES &&
        decoder->seektable_length_bytes % FLAC_SEEKPOINT_BYTES == 0U;
}
