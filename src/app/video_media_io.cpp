#include "video_media_io.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "audio/decoders/flac_decoder.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage_io.h"

namespace VideoMediaIo
{
namespace
{

static const char *TAG = "VideoIO";

static constexpr uint32_t kMaxReadSliceBytes = 4096U;
static constexpr uint32_t kFlacSafePercent = 90U;
static constexpr TickType_t kFlacGatePollTicks = pdMS_TO_TICKS(10);
// R.40.2 的 50ms 一次性 timeout 会把正常的 TF mutex 争用直接升级成
// ESP_EXTRACTOR_ERR_READ。R.40.2.3 改成短片抢锁 + generation/window 重检。
static constexpr TickType_t kStorageLockSlice = pdMS_TO_TICKS(25);
static constexpr TickType_t kStorageRetryYieldTicks = pdMS_TO_TICKS(2);
static constexpr uint32_t kStorageLockFatalWaitMs = 1500U;
static constexpr uint32_t kStorageRetryLogIntervalMs = 250U;
static constexpr TickType_t kOpenLockTimeout = pdMS_TO_TICKS(250);

static bool generation_current(const Context *io)
{
    return io != nullptr &&
        (io->generation_check == nullptr || io->generation_check(io->generation));
}

static int32_t flac_ring_percent()
{
    FlacStorageWindowSnapshot window = {};
    if (!flac_decoder_get_storage_window(&window) || !window.active ||
        !window.storage_competes || window.capacity_bytes == 0U) {
        return -1;
    }
    return static_cast<int32_t>(
        (static_cast<uint64_t>(window.buffered_bytes) * 100ULL + window.capacity_bytes / 2ULL) /
        window.capacity_bytes);
}

static bool wait_for_storage_window(Context *io)
{
    if (io == nullptr) return false;
    int64_t wait_started_us = 0;
    while (generation_current(io)) {
        FlacStorageWindowSnapshot window = {};
        if (!flac_decoder_get_storage_window(&window) || !window.active ||
            !window.storage_competes || window.capacity_bytes == 0U) {
            break;
        }
        const uint32_t percent = static_cast<uint32_t>(
            (static_cast<uint64_t>(window.buffered_bytes) * 100ULL + window.capacity_bytes / 2ULL) /
            window.capacity_bytes);
        if (percent >= kFlacSafePercent) break;
        if (wait_started_us == 0) wait_started_us = esp_timer_get_time();
        vTaskDelay(kFlacGatePollTicks);
    }
    if (!generation_current(io)) return false;
    if (wait_started_us != 0) {
        const uint64_t waited = static_cast<uint64_t>(esp_timer_get_time() - wait_started_us);
        io->gate_wait_us_total += waited;
        ++io->gate_wait_count;
    }
    return true;
}

static uint32_t elapsed_ms(int64_t started_us)
{
    if (started_us <= 0) return 0U;
    const int64_t delta = esp_timer_get_time() - started_us;
    return delta > 0 ? static_cast<uint32_t>(delta / 1000LL) : 0U;
}

static void log_cancelled(const char *op, const Context *io, uint32_t request_or_position)
{
    ESP_LOGW(TAG, "%s取消：generation=%lu value=%lu read_calls=%lu",
        op,
        static_cast<unsigned long>(io != nullptr ? io->generation : 0U),
        static_cast<unsigned long>(request_or_position),
        static_cast<unsigned long>(io != nullptr ? io->read_calls : 0U));
}

} // namespace

void configure(Context *io, uint32_t generation, GenerationCheck generation_check)
{
    if (io == nullptr) return;
    *io = {};
    io->generation = generation;
    io->generation_check = generation_check;
}

int read_cb(void *buffer, uint32_t size, void *ctx)
{
    Context *io = static_cast<Context *>(ctx);
    if (io == nullptr || io->file == nullptr || buffer == nullptr) {
        ESP_LOGE(TAG, "READ参数错误：io=%p file=%p buffer=%p size=%lu",
            static_cast<void *>(io),
            io != nullptr ? static_cast<void *>(io->file) : nullptr,
            buffer,
            static_cast<unsigned long>(size));
        return -1;
    }
    if (size == 0U) return 0;
    if (!generation_current(io)) {
        log_cancelled("READ", io, size);
        return -1;
    }
    if (!wait_for_storage_window(io)) {
        log_cancelled("READ_GATE", io, size);
        return -1;
    }

    const int64_t started_us = esp_timer_get_time();
    uint32_t total = 0U;
    uint32_t zero_progress = 0U;
    uint8_t *out = static_cast<uint8_t *>(buffer);

    auto finish_read = [&](int result) -> int {
        const uint32_t elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
        io->read_us_total += elapsed_us;
        if (elapsed_us > io->read_us_max) io->read_us_max = elapsed_us;
        io->read_bytes += total;
        ++io->read_calls;
        return result;
    };

    if (size > kMaxReadSliceBytes) {
        ++io->aggregate_read_requests;
        if (io->aggregate_read_requests == 1U) {
            ESP_LOGI(TAG,
                "READ_AGGREGATE：extractor_request=%luB，底层按%luB TF切片补齐；每片释放storage锁并重检FLAC水位",
                static_cast<unsigned long>(size),
                static_cast<unsigned long>(kMaxReadSliceBytes));
        }
    }

    while (total < size) {
        if (!generation_current(io)) {
            log_cancelled("READ_FILL", io, size);
            return finish_read(-1);
        }
        if (!wait_for_storage_window(io)) {
            log_cancelled("READ_FILL_GATE", io, size);
            return finish_read(-1);
        }

        const uint32_t remaining = size - total;
        const uint32_t request = remaining > kMaxReadSliceBytes ? kMaxReadSliceBytes : remaining;
        const int64_t lock_wait_started_us = esp_timer_get_time();
        uint32_t retries = 0U;
        uint32_t last_log_ms = 0U;

        while (generation_current(io)) {
            bool slice_done = false;
            {
                StorageSdLockGuard guard(kStorageLockSlice);
                if (guard) {
                    const off_t pos_before = ftello(io->file);
                    errno = 0;
                    const size_t got = fread(out + total, 1U, request, io->file);
                    const int saved_errno = errno;
                    const int eof_flag = feof(io->file);
                    const int error_flag = ferror(io->file);
                    total += static_cast<uint32_t>(got);

                    if (error_flag != 0) {
                        ++io->read_errors;
                        ESP_LOGE(TAG,
                            "READ_FREAD_ERROR：pos=%lld callback=%lu slice=%lu got=%lu total=%lu errno=%d feof=%d ferror=%d retries=%lu wait=%lums ring=%ld%%",
                            static_cast<long long>(pos_before),
                            static_cast<unsigned long>(size),
                            static_cast<unsigned long>(request),
                            static_cast<unsigned long>(got),
                            static_cast<unsigned long>(total),
                            saved_errno, eof_flag, error_flag,
                            static_cast<unsigned long>(retries),
                            static_cast<unsigned long>(elapsed_ms(lock_wait_started_us)),
                            static_cast<long>(flac_ring_percent()));
                        return finish_read(-1);
                    }

                    if (retries != 0U) {
                        ++io->lock_recoveries;
                        ESP_LOGW(TAG,
                            "READ_LOCK_RECOVERED：pos=%lld callback=%lu slice=%lu got=%lu total=%lu retries=%lu wait=%lums ring=%ld%%",
                            static_cast<long long>(pos_before),
                            static_cast<unsigned long>(size),
                            static_cast<unsigned long>(request),
                            static_cast<unsigned long>(got),
                            static_cast<unsigned long>(total),
                            static_cast<unsigned long>(retries),
                            static_cast<unsigned long>(elapsed_ms(lock_wait_started_us)),
                            static_cast<long>(flac_ring_percent()));
                    }

                    if (got == 0U) {
                        if (eof_flag != 0) return finish_read(static_cast<int>(total));
                        ++zero_progress;
                        if (zero_progress >= 3U) {
                            ++io->read_errors;
                            ESP_LOGE(TAG,
                                "READ_ZERO_PROGRESS：callback=%lu total=%lu pos=%lld 连续=%lu ring=%ld%%",
                                static_cast<unsigned long>(size),
                                static_cast<unsigned long>(total),
                                static_cast<long long>(pos_before),
                                static_cast<unsigned long>(zero_progress),
                                static_cast<long>(flac_ring_percent()));
                            return finish_read(-1);
                        }
                    } else {
                        zero_progress = 0U;
                    }

                    if (got < request && eof_flag != 0) {
                        return finish_read(static_cast<int>(total));
                    }
                    if (got < request && got != 0U) {
                        ESP_LOGW(TAG,
                            "READ_STDIO_SHORT：callback=%lu slice=%lu got=%lu total=%lu feof=0 ferror=0；继续补齐",
                            static_cast<unsigned long>(size),
                            static_cast<unsigned long>(request),
                            static_cast<unsigned long>(got),
                            static_cast<unsigned long>(total));
                    }
                    slice_done = true;
                }
            }

            if (slice_done) break;

            ++retries;
            ++io->lock_retry_count;
            const uint32_t waited_ms = elapsed_ms(lock_wait_started_us);
            if (waited_ms >= kStorageLockFatalWaitMs) {
                ++io->lock_fatal_timeouts;
                ESP_LOGE(TAG,
                    "READ_LOCK_TIMEOUT：callback=%lu slice=%lu total=%lu retries=%lu wait=%lums read_calls=%lu bytes=%llu ring=%ld%%",
                    static_cast<unsigned long>(size),
                    static_cast<unsigned long>(request),
                    static_cast<unsigned long>(total),
                    static_cast<unsigned long>(retries),
                    static_cast<unsigned long>(waited_ms),
                    static_cast<unsigned long>(io->read_calls),
                    static_cast<unsigned long long>(io->read_bytes),
                    static_cast<long>(flac_ring_percent()));
                return finish_read(-1);
            }
            if (last_log_ms == 0U || waited_ms - last_log_ms >= kStorageRetryLogIntervalMs) {
                last_log_ms = waited_ms;
                ESP_LOGW(TAG,
                    "READ_LOCK_BUSY：callback=%lu slice=%lu total=%lu retries=%lu wait=%lums ring=%ld%%；继续让路重试",
                    static_cast<unsigned long>(size),
                    static_cast<unsigned long>(request),
                    static_cast<unsigned long>(total),
                    static_cast<unsigned long>(retries),
                    static_cast<unsigned long>(waited_ms),
                    static_cast<long>(flac_ring_percent()));
            }
            if (!wait_for_storage_window(io)) {
                log_cancelled("READ_RETRY_GATE", io, size);
                return finish_read(-1);
            }
            vTaskDelay(kStorageRetryYieldTicks);
        }

        if (!generation_current(io)) {
            log_cancelled("READ_RETRY", io, size);
            return finish_read(-1);
        }
        if (total < size) taskYIELD();
    }

    return finish_read(static_cast<int>(total));
}

int seek_cb(uint32_t position, void *ctx)
{
    Context *io = static_cast<Context *>(ctx);
    if (io == nullptr || io->file == nullptr || position > io->size) {
        ESP_LOGE(TAG, "SEEK参数错误：io=%p file=%p pos=%lu size=%lu",
            static_cast<void *>(io),
            io != nullptr ? static_cast<void *>(io->file) : nullptr,
            static_cast<unsigned long>(position),
            static_cast<unsigned long>(io != nullptr ? io->size : 0U));
        return -1;
    }
    if (!generation_current(io) || !wait_for_storage_window(io)) {
        log_cancelled("SEEK", io, position);
        return -1;
    }

    const int64_t lock_wait_started_us = esp_timer_get_time();
    uint32_t retries = 0U;
    uint32_t last_log_ms = 0U;
    while (generation_current(io)) {
        {
            StorageSdLockGuard guard(kStorageLockSlice);
            if (guard) {
                errno = 0;
                const int ret = fseeko(io->file, static_cast<off_t>(position), SEEK_SET);
                const int saved_errno = errno;
                if (ret != 0) {
                    ++io->seek_errors;
                    ESP_LOGE(TAG,
                        "SEEK_FSEEK_ERROR：pos=%lu errno=%d retries=%lu wait=%lums ring=%ld%%",
                        static_cast<unsigned long>(position), saved_errno,
                        static_cast<unsigned long>(retries),
                        static_cast<unsigned long>(elapsed_ms(lock_wait_started_us)),
                        static_cast<long>(flac_ring_percent()));
                    return -1;
                }
                if (retries != 0U) {
                    ++io->lock_recoveries;
                    ESP_LOGW(TAG,
                        "SEEK_LOCK_RECOVERED：pos=%lu retries=%lu wait=%lums ring=%ld%%",
                        static_cast<unsigned long>(position),
                        static_cast<unsigned long>(retries),
                        static_cast<unsigned long>(elapsed_ms(lock_wait_started_us)),
                        static_cast<long>(flac_ring_percent()));
                }
                return 0;
            }
        }

        ++retries;
        ++io->lock_retry_count;
        const uint32_t waited_ms = elapsed_ms(lock_wait_started_us);
        if (waited_ms >= kStorageLockFatalWaitMs) {
            ++io->lock_fatal_timeouts;
            ESP_LOGE(TAG,
                "SEEK_LOCK_TIMEOUT：pos=%lu retries=%lu wait=%lums ring=%ld%%",
                static_cast<unsigned long>(position),
                static_cast<unsigned long>(retries),
                static_cast<unsigned long>(waited_ms),
                static_cast<long>(flac_ring_percent()));
            return -1;
        }
        if (last_log_ms == 0U || waited_ms - last_log_ms >= kStorageRetryLogIntervalMs) {
            last_log_ms = waited_ms;
            ESP_LOGW(TAG,
                "SEEK_LOCK_BUSY：pos=%lu retries=%lu wait=%lums ring=%ld%%；继续让路重试",
                static_cast<unsigned long>(position),
                static_cast<unsigned long>(retries),
                static_cast<unsigned long>(waited_ms),
                static_cast<long>(flac_ring_percent()));
        }
        if (!wait_for_storage_window(io)) {
            log_cancelled("SEEK_RETRY_GATE", io, position);
            return -1;
        }
        vTaskDelay(kStorageRetryYieldTicks);
    }

    log_cancelled("SEEK_RETRY", io, position);
    return -1;
}

uint32_t size_cb(void *ctx)
{
    Context *io = static_cast<Context *>(ctx);
    return io != nullptr ? io->size : 0U;
}

esp_err_t open(const char *path, Context *io)
{
    if (path == nullptr || io == nullptr) return ESP_ERR_INVALID_ARG;
    if (!generation_current(io) || !wait_for_storage_window(io)) return ESP_ERR_INVALID_STATE;

    struct stat info = {};
    {
        StorageSdLockGuard guard(kOpenLockTimeout);
        if (!guard) {
            ESP_LOGE(TAG, "OPEN_LOCK_TIMEOUT：path=%s timeout=250ms ring=%ld%%",
                path, static_cast<long>(flac_ring_percent()));
            return ESP_ERR_TIMEOUT;
        }
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return ESP_ERR_NOT_FOUND;
        if (info.st_size <= 0 || static_cast<uint64_t>(info.st_size) > UINT32_MAX) return ESP_ERR_INVALID_SIZE;
        io->file = fopen(path, "rb");
    }
    if (io->file == nullptr) return ESP_ERR_NOT_FOUND;
    io->size = static_cast<uint32_t>(info.st_size);
    return ESP_OK;
}

void close(Context *io)
{
    if (io == nullptr || io->file == nullptr) return;
    StorageSdLockGuard guard(portMAX_DELAY);
    if (guard) fclose(io->file);
    io->file = nullptr;
}

esp_err_t map_extractor_error(esp_extractor_err_t err)
{
    switch (err) {
        case ESP_EXTRACTOR_ERR_OK: return ESP_OK;
        case ESP_EXTRACTOR_ERR_INV_ARG: return ESP_ERR_INVALID_ARG;
        case ESP_EXTRACTOR_ERR_NO_MEM: return ESP_ERR_NO_MEM;
        case ESP_EXTRACTOR_ERR_NOT_SUPPORTED: return ESP_ERR_NOT_SUPPORTED;
        case ESP_EXTRACTOR_ERR_NOT_FOUND: return ESP_ERR_NOT_FOUND;
        case ESP_EXTRACTOR_ERR_WRONG_HEADER: return ESP_ERR_INVALID_RESPONSE;
        // READ 已不再等价于 50ms lock timeout；真正原因由 VideoIO 的细分日志给出。
        case ESP_EXTRACTOR_ERR_READ: return ESP_FAIL;
        case ESP_EXTRACTOR_ERR_ABORTED: return ESP_ERR_INVALID_STATE;
        case ESP_EXTRACTOR_ERR_SKIPPED: return ESP_ERR_INVALID_SIZE;
        default: return ESP_FAIL;
    }
}

} // namespace VideoMediaIo
