#include "artwork_loader.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app_diag_config.h"
#include "flac_decoder.h"
#include "media_catalog_v2.h"
#include "media_library.h"
#include "storage_io.h"

static const char *TAG = "封面加载";

#if APP_DIAG_ARTWORK_LOADER
#define ARTWORK_LOAD_TRACE(...) ESP_LOGI(TAG, "ARTWORK_LOAD_TRACE: " __VA_ARGS__)
#else
#define ARTWORK_LOAD_TRACE(...) APP_DIAG_DISCARDED_LOGI(TAG, "ARTWORK_LOAD_TRACE: " __VA_ARGS__)
#endif

static constexpr uint32_t ARTWORK_TASK_STACK_BYTES = 4096U;
static constexpr UBaseType_t ARTWORK_TASK_PRIORITY = 2U;
static constexpr BaseType_t ARTWORK_TASK_CORE = 1;
// P1.2.9：压缩封面不再要求一次连续读完。ArtworkTask 只在 FLAC ring 有余量时
// 抢一个很短的 SD 窗口，每次读 2/4/8KB，随后立即释放锁；水位不足或拿不到锁时
// 只是暂停当前 Job，loaded offset 保留在 PSRAM buffer 中，下一轮继续，不从头重读。
static constexpr size_t ARTWORK_READ_CHUNK_MIN_BYTES = 2U * 1024U;
static constexpr size_t ARTWORK_READ_CHUNK_MID_BYTES = 4U * 1024U;
static constexpr size_t ARTWORK_READ_CHUNK_MAX_BYTES = 8U * 1024U;
// R.36：不再缓存历史歌曲压缩封面。2 槽仅用于“旧 fallback lease 尚未释放 + 新当前曲正在读取”
// 的瞬时交换，正常稳态在 Surface 成功后会全部清空。
static constexpr size_t ARTWORK_CACHE_SLOT_COUNT = 2U;
static constexpr size_t ARTWORK_CACHE_BUDGET_BYTES = 2U * 1024U * 1024U;
static constexpr size_t ARTWORK_MAX_COMPRESSED_BYTES = 2U * 1024U * 1024U;
// P1.5R.1：SD 锁竞争属于正常背压。锁尝试明确使用 0 tick（非阻塞），
// 失败后明确阻塞 1 个 RTOS tick。禁止再用 pdMS_TO_TICKS(1/2/3)，因为
// 在 100Hz tick 下这些值会变成 0，导致 ArtworkTask 看似 delay、实际仍 Ready。
static constexpr TickType_t ARTWORK_SD_LOCK_TRY = 0;
static constexpr TickType_t ARTWORK_SD_RETRY_DELAY = 1;
static constexpr TickType_t ARTWORK_POST_SLICE_DELAY = 1;
static constexpr TickType_t ARTWORK_PROGRESS_LOG_INTERVAL = pdMS_TO_TICKS(1000);
// R.36.3.1：Artwork 每个增量 slice 与 system_loop 统一使用 FLAC 高水位窗口。
// <90% 完全让路；90~91% 仅读 2KB，92~95% 读 4KB，>=96% 才读 8KB。
// 确保封面 I/O 始终落后于 FLAC 90% Normal / 92% Plenty 恢复窗口。
static constexpr uint32_t ARTWORK_FLAC_PAUSE_PERCENT = 90U;
static constexpr uint32_t ARTWORK_FLAC_MID_PERCENT = 92U;
static constexpr uint32_t ARTWORK_FLAC_FAST_PERCENT = 96U;

struct ArtworkLoadRequest
{
    uint32_t request_id = 0;
    uint32_t catalog_generation = 0;
    uint32_t track_index = UINT32_MAX;
    ArtworkRefV2 ref = {};
    char *source_path = nullptr;
    bool has_artwork = false;
};

struct ArtworkCacheEntry
{
    uint8_t *data = nullptr;
    size_t size = 0;
    MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t catalog_generation = 0;
    uint32_t track_index = UINT32_MAX;
    uint32_t slot_revision = 0;
    uint32_t lru_stamp = 0;
    uint16_t pin_count = 0;
    bool valid = false;
};

static QueueHandle_t g_request_queue = nullptr;
static SemaphoreHandle_t g_submit_mutex = nullptr;
static SemaphoreHandle_t g_cache_mutex = nullptr;
static TaskHandle_t g_artwork_task = nullptr;
static volatile bool g_ready = false;

static portMUX_TYPE g_request_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_next_request_id = 1U;
static uint32_t g_latest_request_id = 0U;

static portMUX_TYPE g_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static ArtworkLoaderSnapshot g_snapshot = {};
static uint32_t g_superseded_count = 0U;

static ArtworkCacheEntry g_cache[ARTWORK_CACHE_SLOT_COUNT] = {};
static uint32_t g_lru_counter = 1U;
static uint32_t g_next_slot_revision = 1U;

static void artwork_request_release(ArtworkLoadRequest *request)
{
    if (request == nullptr) {
        return;
    }
    heap_caps_free(request->source_path);
    request->source_path = nullptr;
    heap_caps_free(request);
}

static char *artwork_psram_strdup(const char *text)
{
    if (text == nullptr) {
        return nullptr;
    }
    const size_t length = strlen(text) + 1U;
    char *copy = static_cast<char *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy != nullptr) {
        memcpy(copy, text, length);
    }
    return copy;
}

static uint32_t artwork_next_request_id()
{
    portENTER_CRITICAL(&g_request_mux);
    const uint32_t id = g_next_request_id++;
    if (g_next_request_id == 0U) {
        g_next_request_id = 1U;
    }
    portEXIT_CRITICAL(&g_request_mux);
    return id;
}

static uint32_t artwork_latest_request_id()
{
    portENTER_CRITICAL(&g_request_mux);
    const uint32_t id = g_latest_request_id;
    portEXIT_CRITICAL(&g_request_mux);
    return id;
}

static void artwork_set_latest_request_id(uint32_t request_id)
{
    portENTER_CRITICAL(&g_request_mux);
    g_latest_request_id = request_id;
    portEXIT_CRITICAL(&g_request_mux);
}

static void artwork_count_superseded()
{
    portENTER_CRITICAL(&g_snapshot_mux);
    ++g_superseded_count;
    g_snapshot.superseded_count = g_superseded_count;
    portEXIT_CRITICAL(&g_snapshot_mux);
}

static void artwork_publish_state(
    ArtworkLoadState state,
    const ArtworkLoadRequest *request,
    esp_err_t result,
    bool cache_hit,
    uint32_t data_size,
    MediaArtworkFormatV2 format,
    uint16_t width,
    uint16_t height
)
{
    portENTER_CRITICAL(&g_snapshot_mux);
    g_snapshot.ready = g_ready;
    g_snapshot.state = state;
    ++g_snapshot.state_revision;
    g_snapshot.request_id = request != nullptr ? request->request_id : 0U;
    g_snapshot.catalog_generation = request != nullptr ? request->catalog_generation : 0U;
    g_snapshot.track_index = request != nullptr ? request->track_index : UINT32_MAX;
    g_snapshot.data_size = data_size;
    g_snapshot.format = format;
    g_snapshot.width = width;
    g_snapshot.height = height;
    g_snapshot.result = result;
    g_snapshot.cache_hit = cache_hit;
    g_snapshot.superseded_count = g_superseded_count;
    portEXIT_CRITICAL(&g_snapshot_mux);
}

static bool artwork_request_is_latest(const ArtworkLoadRequest *request)
{
    if (request == nullptr || request->request_id != artwork_latest_request_id()) {
        return false;
    }
    return request->catalog_generation == media_catalog_v2_generation();
}

static bool artwork_flac_slice_plan(size_t *out_chunk, FlacStorageWindowSnapshot *out_window = nullptr)
{
    if (out_chunk == nullptr) {
        return false;
    }
    *out_chunk = ARTWORK_READ_CHUNK_MAX_BYTES;

    FlacStorageWindowSnapshot window = {};
    if (!flac_decoder_get_storage_window(&window) || !window.active ||
        !window.storage_competes || window.capacity_bytes == 0U) {
        if (out_window != nullptr) *out_window = window;
        return true;
    }

    const uint64_t percent = static_cast<uint64_t>(window.buffered_bytes) * 100ULL /
        static_cast<uint64_t>(window.capacity_bytes);
    if (out_window != nullptr) *out_window = window;

    if (percent < ARTWORK_FLAC_PAUSE_PERCENT) {
        *out_chunk = 0U;
        return false;
    }
    if (percent < ARTWORK_FLAC_MID_PERCENT) {
        *out_chunk = ARTWORK_READ_CHUNK_MIN_BYTES;
    } else if (percent < ARTWORK_FLAC_FAST_PERCENT) {
        *out_chunk = ARTWORK_READ_CHUNK_MID_BYTES;
    } else {
        *out_chunk = ARTWORK_READ_CHUNK_MAX_BYTES;
    }
    return true;
}

// 等待一个真正安全的小 I/O 窗口。没有窗口不是错误：Job 保留 loaded offset，
// 只要请求仍是 latest，就持续在后台等待；切歌后 latest-wins 会立即取消旧 Job。
static bool artwork_lock_next_slice(
    const ArtworkLoadRequest *request,
    size_t *out_chunk,
    FlacStorageWindowSnapshot *out_window = nullptr
)
{
    if (out_chunk == nullptr) {
        return false;
    }
    TickType_t last_wait_log = 0;
    while (artwork_request_is_latest(request)) {
        size_t planned = 0U;
        FlacStorageWindowSnapshot window = {};
        if (!artwork_flac_slice_plan(&planned, &window) || planned == 0U) {
            if (out_window != nullptr) *out_window = window;
            const TickType_t now = xTaskGetTickCount();
            if (last_wait_log == 0 || now - last_wait_log >= ARTWORK_PROGRESS_LOG_INTERVAL) {
                last_wait_log = now;
                const uint32_t ring_percent = window.capacity_bytes == 0U ? 0U :
                    static_cast<uint32_t>(static_cast<uint64_t>(window.buffered_bytes) * 100ULL /
                                          static_cast<uint64_t>(window.capacity_bytes));
                ESP_LOGI(TAG, "封面增量等待：track=%lu ring=%lu/%luB (%lu%%)，低于%lu%%暂停读TF",
                    request != nullptr ? static_cast<unsigned long>(request->track_index) : 0UL,
                    static_cast<unsigned long>(window.buffered_bytes),
                    static_cast<unsigned long>(window.capacity_bytes),
                    static_cast<unsigned long>(ring_percent),
                    static_cast<unsigned long>(ARTWORK_FLAC_PAUSE_PERCENT));
            }
            vTaskDelay(ARTWORK_SD_RETRY_DELAY);
            continue;
        }
        if (storage_sd_lock(ARTWORK_SD_LOCK_TRY)) {
            *out_chunk = planned;
            if (out_window != nullptr) *out_window = window;
            return true;
        }
        vTaskDelay(ARTWORK_SD_RETRY_DELAY);
    }
    return false;
}

class ArtworkSdSliceGuard
{
public:
    ArtworkSdSliceGuard(const ArtworkLoadRequest *request, size_t *out_chunk,
                        FlacStorageWindowSnapshot *out_window = nullptr)
        : locked_(artwork_lock_next_slice(request, out_chunk, out_window))
    {
    }

    ~ArtworkSdSliceGuard()
    {
        if (locked_) storage_sd_unlock();
    }

    ArtworkSdSliceGuard(const ArtworkSdSliceGuard &) = delete;
    ArtworkSdSliceGuard &operator=(const ArtworkSdSliceGuard &) = delete;

    bool locked() const { return locked_; }

private:
    bool locked_ = false;
};

static void cache_release_entry_locked(ArtworkCacheEntry *entry)
{
    if (entry == nullptr || !entry->valid || entry->pin_count != 0U) {
        return;
    }
    heap_caps_free(entry->data);
    *entry = {};
}

static size_t cache_total_bytes_locked()
{
    size_t total = 0U;
    for (size_t i = 0; i < ARTWORK_CACHE_SLOT_COUNT; ++i) {
        if (g_cache[i].valid) {
            total += g_cache[i].size;
        }
    }
    return total;
}

static int cache_find_locked(uint32_t catalog_generation, uint32_t track_index)
{
    for (size_t i = 0; i < ARTWORK_CACHE_SLOT_COUNT; ++i) {
        const ArtworkCacheEntry &entry = g_cache[i];
        if (entry.valid && entry.catalog_generation == catalog_generation && entry.track_index == track_index) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static int cache_find_lru_evictable_locked(int exclude_index = -1)
{
    int selected = -1;
    uint32_t oldest = UINT32_MAX;
    for (size_t i = 0; i < ARTWORK_CACHE_SLOT_COUNT; ++i) {
        if (static_cast<int>(i) == exclude_index) {
            continue;
        }
        const ArtworkCacheEntry &entry = g_cache[i];
        if (!entry.valid) {
            return static_cast<int>(i);
        }
        if (entry.pin_count == 0U && (selected < 0 || entry.lru_stamp < oldest)) {
            selected = static_cast<int>(i);
            oldest = entry.lru_stamp;
        }
    }
    return selected;
}

static int cache_find_lru_valid_evictable_locked(int exclude_index = -1)
{
    int selected = -1;
    uint32_t oldest = UINT32_MAX;
    for (size_t i = 0; i < ARTWORK_CACHE_SLOT_COUNT; ++i) {
        if (static_cast<int>(i) == exclude_index) {
            continue;
        }
        const ArtworkCacheEntry &entry = g_cache[i];
        if (entry.valid && entry.pin_count == 0U && (selected < 0 || entry.lru_stamp < oldest)) {
            selected = static_cast<int>(i);
            oldest = entry.lru_stamp;
        }
    }
    return selected;
}

static bool cache_lookup_metadata(
    uint32_t catalog_generation,
    uint32_t track_index,
    MediaArtworkFormatV2 *out_format,
    uint16_t *out_width,
    uint16_t *out_height,
    uint32_t *out_size
)
{
    if (g_cache_mutex == nullptr || xSemaphoreTake(g_cache_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const int index = cache_find_locked(catalog_generation, track_index);
    if (index < 0) {
        xSemaphoreGive(g_cache_mutex);
        return false;
    }
    ArtworkCacheEntry &entry = g_cache[index];
    entry.lru_stamp = g_lru_counter++;
    if (g_lru_counter == 0U) g_lru_counter = 1U;
    if (out_format != nullptr) *out_format = entry.format;
    if (out_width != nullptr) *out_width = entry.width;
    if (out_height != nullptr) *out_height = entry.height;
    if (out_size != nullptr) *out_size = static_cast<uint32_t>(entry.size);
    xSemaphoreGive(g_cache_mutex);
    return true;
}

static bool cache_insert(
    const ArtworkLoadRequest *request,
    uint8_t *data,
    size_t size,
    MediaArtworkFormatV2 format,
    uint16_t width,
    uint16_t height
)
{
    if (request == nullptr || data == nullptr || size == 0U || g_cache_mutex == nullptr ||
        xSemaphoreTake(g_cache_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    // R.36：只保留当前请求需要的压缩原图。所有其它未 pin 条目在插入新当前曲前立即释放；
    // 被旧 LVGL fallback 持有的条目允许暂时跨越一次切歌，由第二交换槽承接新图。
    for (size_t i = 0; i < ARTWORK_CACHE_SLOT_COUNT; ++i) {
        if (g_cache[i].valid &&
            (g_cache[i].catalog_generation != request->catalog_generation ||
             g_cache[i].track_index != request->track_index) &&
            g_cache[i].pin_count == 0U) {
            cache_release_entry_locked(&g_cache[i]);
        }
    }

    int slot = cache_find_locked(request->catalog_generation, request->track_index);
    if (slot >= 0 && g_cache[slot].pin_count != 0U) {
        xSemaphoreGive(g_cache_mutex);
        return false;
    }
    if (slot < 0) {
        slot = cache_find_lru_evictable_locked();
    }
    if (slot < 0) {
        xSemaphoreGive(g_cache_mutex);
        return false;
    }

    if (g_cache[slot].valid) {
        cache_release_entry_locked(&g_cache[slot]);
    }

    // 2MB 仅作为“单张超大压缩封面 + 瞬时旧 lease”的硬上限，不再用于跨歌曲长期 LRU。
    while (cache_total_bytes_locked() + size > ARTWORK_CACHE_BUDGET_BYTES) {
        const int victim = cache_find_lru_valid_evictable_locked(slot);
        if (victim < 0 || victim == slot) {
            break;
        }
        cache_release_entry_locked(&g_cache[victim]);
    }
    if (cache_total_bytes_locked() + size > ARTWORK_CACHE_BUDGET_BYTES) {
        xSemaphoreGive(g_cache_mutex);
        return false;
    }

    ArtworkCacheEntry &entry = g_cache[slot];
    entry.data = data;
    entry.size = size;
    entry.format = format;
    entry.width = width;
    entry.height = height;
    entry.catalog_generation = request->catalog_generation;
    entry.track_index = request->track_index;
    entry.slot_revision = g_next_slot_revision++;
    if (g_next_slot_revision == 0U) g_next_slot_revision = 1U;
    entry.lru_stamp = g_lru_counter++;
    if (g_lru_counter == 0U) g_lru_counter = 1U;
    entry.pin_count = 0U;
    entry.valid = true;

    xSemaphoreGive(g_cache_mutex);
    return true;
}

static MediaArtworkFormatV2 artwork_detect_format(const uint8_t *data, size_t size)
{
    if (data == nullptr) {
        return MediaArtworkFormatV2::Unknown;
    }
    if (size >= 8U && memcmp(data, "\x89PNG\x0D\x0A\x1A\x0A", 8U) == 0) {
        return MediaArtworkFormatV2::Png;
    }
    if (size >= 2U && data[0] == 0xFFU && data[1] == 0xD8U) {
        return MediaArtworkFormatV2::Jpeg;
    }
    return MediaArtworkFormatV2::Unknown;
}

// ID3v2.4 unsynchronisation 的逆变换。只删除由 unsync 规则插入的 0x00，避免误删 JPEG 自身的 FF 00 stuffing。
static size_t artwork_deunsynchronise_in_place(uint8_t *data, size_t size)
{
    if (data == nullptr || size == 0U) {
        return 0U;
    }
    size_t write_pos = 0U;
    for (size_t read_pos = 0U; read_pos < size; ++read_pos) {
        const uint8_t value = data[read_pos];
        data[write_pos++] = value;
        if (value != 0xFFU || read_pos + 1U >= size || data[read_pos + 1U] != 0x00U) {
            continue;
        }

        // FF 00 00 表示原始 FF 00；FF 00 E0..FF 表示为了避免伪同步字而插入的 00。
        // 文件尾的 FF 00 同样按规范移除插入字节。
        const bool at_end = read_pos + 2U >= size;
        const uint8_t following = at_end ? 0U : data[read_pos + 2U];
        if (at_end || following == 0x00U || following >= 0xE0U) {
            ++read_pos;
        }
    }
    return write_pos;
}

static esp_err_t artwork_validate_source(const ArtworkLoadRequest *request)
{
    if (request == nullptr || !request->has_artwork || request->source_path == nullptr ||
        request->ref.data_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat info = {};
    {
        size_t ignored_chunk = 0U;
        ArtworkSdSliceGuard sd_lock(request, &ignored_chunk);
        if (!sd_lock.locked()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (stat(request->source_path, &info) != 0 || info.st_size < 0) {
            return ESP_ERR_NOT_FOUND;
        }
    }
    const uint64_t file_size = static_cast<uint64_t>(info.st_size);
    if (request->ref.data_offset > file_size ||
        static_cast<uint64_t>(request->ref.data_size) > file_size - request->ref.data_offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (request->ref.source == MediaArtworkSourceV2::ExternalFile && request->ref.source_modified_time != 0 &&
        static_cast<int64_t>(info.st_mtime) != request->ref.source_modified_time) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void artwork_close_file_cooperatively(FILE *file)
{
    if (file == nullptr) {
        return;
    }
    while (!storage_sd_lock(ARTWORK_SD_LOCK_TRY)) {
        vTaskDelay(ARTWORK_SD_RETRY_DELAY);
    }
    fclose(file);
    storage_sd_unlock();
}

static __attribute__((noinline)) esp_err_t artwork_read_blob(
    const ArtworkLoadRequest *request,
    uint8_t **out_data,
    size_t *out_size,
    MediaArtworkFormatV2 *out_format
)
{
    if (request == nullptr || out_data == nullptr || out_size == nullptr || out_format == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = nullptr;
    *out_size = 0U;
    *out_format = MediaArtworkFormatV2::Unknown;

    if (request->ref.data_size == 0U || request->ref.data_size > ARTWORK_MAX_COMPRESSED_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t source_ret = artwork_validate_source(request);
    if (source_ret != ESP_OK) {
        return source_ret;
    }
    if (request->ref.data_offset > static_cast<uint64_t>(LONG_MAX)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *data = static_cast<uint8_t *>(
        heap_caps_malloc(request->ref.data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (data == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    FILE *file = nullptr;
    {
        size_t ignored_chunk = 0U;
        ArtworkSdSliceGuard sd_lock(request, &ignored_chunk);
        if (!sd_lock.locked()) {
            heap_caps_free(data);
            return ESP_ERR_INVALID_STATE;
        }
        file = fopen(request->source_path, "rb");
        if (file == nullptr) {
            heap_caps_free(data);
            return ESP_ERR_NOT_FOUND;
        }
        if (fseek(file, static_cast<long>(request->ref.data_offset), SEEK_SET) != 0) {
            fclose(file);
            heap_caps_free(data);
            return ESP_FAIL;
        }
    }

    size_t loaded = 0U;
    TickType_t last_progress_log = xTaskGetTickCount();
    uint8_t last_progress_bucket = 0U;

    // 真正的“间隙读取”：FILE/offset 保持，但 SD mutex 绝不跨 slice 持有。
    // 每次 ring 安全时只 fread 2/4/8KB，立即释放锁；水位不足时 loaded 保持原值暂停。
    while (loaded < request->ref.data_size) {
        if (!artwork_request_is_latest(request)) {
            artwork_close_file_cooperatively(file);
            heap_caps_free(data);
            return ESP_ERR_INVALID_STATE;
        }

        size_t planned_chunk = 0U;
        FlacStorageWindowSnapshot window = {};
        {
            ArtworkSdSliceGuard sd_lock(request, &planned_chunk, &window);
            if (!sd_lock.locked()) {
                artwork_close_file_cooperatively(file);
                heap_caps_free(data);
                return ESP_ERR_INVALID_STATE;
            }

            const size_t remaining = static_cast<size_t>(request->ref.data_size) - loaded;
            const size_t chunk = remaining < planned_chunk ? remaining : planned_chunk;
            const size_t got = fread(data + loaded, 1, chunk, file);
            if (got != chunk) {
                artwork_close_file_cooperatively(file);
                heap_caps_free(data);
                return ESP_FAIL;
            }
            loaded += got;
        }

        const uint8_t progress_bucket = static_cast<uint8_t>(
            (static_cast<uint64_t>(loaded) * 4ULL) / static_cast<uint64_t>(request->ref.data_size));
        const TickType_t now = xTaskGetTickCount();
        if (progress_bucket > last_progress_bucket ||
            now - last_progress_log >= ARTWORK_PROGRESS_LOG_INTERVAL) {
            last_progress_bucket = progress_bucket;
            last_progress_log = now;
            const uint32_t ring_percent = window.capacity_bytes == 0U ? 100U :
                static_cast<uint32_t>(static_cast<uint64_t>(window.buffered_bytes) * 100ULL /
                                      static_cast<uint64_t>(window.capacity_bytes));
            ESP_LOGI(TAG, "封面增量读取：track=%lu loaded=%u/%luB chunk=%uB ring=%lu%%",
                static_cast<unsigned long>(request->track_index),
                static_cast<unsigned>(loaded),
                static_cast<unsigned long>(request->ref.data_size),
                static_cast<unsigned>(planned_chunk),
                static_cast<unsigned long>(ring_percent));
        }

        if (loaded < request->ref.data_size) {
            // 每个 slice 后必定阻塞一 tick，把 Core1/SD 窗口主动还给 FlacPrefetch/LVGL。
            vTaskDelay(ARTWORK_POST_SLICE_DELAY);
        }
    }

    artwork_close_file_cooperatively(file);

    size_t logical_size = loaded;
    if ((request->ref.flags & MEDIA_ARTWORK_REF_NEEDS_ID3_UNSYNC_V2) != 0U) {
        logical_size = artwork_deunsynchronise_in_place(data, loaded);
    }
    const MediaArtworkFormatV2 actual_format = artwork_detect_format(data, logical_size);
    if (actual_format == MediaArtworkFormatV2::Unknown) {
        heap_caps_free(data);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (request->ref.format != MediaArtworkFormatV2::Unknown && request->ref.format != actual_format) {
        ESP_LOGW(TAG, "封面 magic 与 Catalog 格式不一致：track=%lu catalog=%u actual=%u，运行时以 magic 为准",
            static_cast<unsigned long>(request->track_index),
            static_cast<unsigned>(request->ref.format),
            static_cast<unsigned>(actual_format));
    }

    *out_data = data;
    *out_size = logical_size;
    *out_format = actual_format;
    return ESP_OK;
}

static void artwork_task_main(void *)
{
    g_ready = true;
    artwork_publish_state(ArtworkLoadState::Idle, nullptr, ESP_OK, false, 0U, MediaArtworkFormatV2::Unknown, 0U, 0U);

    while (true) {
        ArtworkLoadRequest *request = nullptr;
        if (xQueueReceive(g_request_queue, &request, portMAX_DELAY) != pdTRUE || request == nullptr) {
            continue;
        }

        if (!artwork_request_is_latest(request)) {
            artwork_count_superseded();
            artwork_request_release(request);
            continue;
        }

        ARTWORK_LOAD_TRACE("DISPATCH request=%lu generation=%lu track=%lu source=%u size=%lu",
            static_cast<unsigned long>(request->request_id),
            static_cast<unsigned long>(request->catalog_generation),
            static_cast<unsigned long>(request->track_index),
            static_cast<unsigned>(request->ref.source),
            static_cast<unsigned long>(request->ref.data_size));

        if (!request->has_artwork) {
            artwork_publish_state(ArtworkLoadState::NoArtwork, request, ESP_ERR_NOT_FOUND, false, 0U,
                MediaArtworkFormatV2::Unknown, 0U, 0U);
            artwork_request_release(request);
            continue;
        }

        MediaArtworkFormatV2 cached_format = MediaArtworkFormatV2::Unknown;
        uint16_t cached_width = 0U;
        uint16_t cached_height = 0U;
        uint32_t cached_size = 0U;
        if (cache_lookup_metadata(request->catalog_generation, request->track_index,
            &cached_format, &cached_width, &cached_height, &cached_size)) {
            artwork_publish_state(ArtworkLoadState::Ready, request, ESP_OK, true, cached_size,
                cached_format, cached_width, cached_height);
            ARTWORK_LOAD_TRACE("CACHE_HIT request=%lu track=%lu bytes=%lu",
                static_cast<unsigned long>(request->request_id),
                static_cast<unsigned long>(request->track_index),
                static_cast<unsigned long>(cached_size));
            artwork_request_release(request);
            continue;
        }

        artwork_publish_state(ArtworkLoadState::Loading, request, ESP_OK, false, request->ref.data_size,
            request->ref.format, request->ref.width, request->ref.height);

        uint8_t *data = nullptr;
        size_t size = 0U;
        MediaArtworkFormatV2 actual_format = MediaArtworkFormatV2::Unknown;
        const esp_err_t load_ret = artwork_read_blob(request, &data, &size, &actual_format);

        if (!artwork_request_is_latest(request)) {
            heap_caps_free(data);
            artwork_count_superseded();
            ARTWORK_LOAD_TRACE("SUPERSEDED request=%lu track=%lu phase=after_read",
                static_cast<unsigned long>(request->request_id),
                static_cast<unsigned long>(request->track_index));
            artwork_request_release(request);
            continue;
        }

        if (load_ret != ESP_OK) {
            if (load_ret == ESP_ERR_INVALID_SIZE && request->ref.data_size > ARTWORK_MAX_COMPRESSED_BYTES) {
                ESP_LOGW(TAG, "封面过大，跳过缓存：track=%lu size=%luB 上限=%uB",
                    static_cast<unsigned long>(request->track_index),
                    static_cast<unsigned long>(request->ref.data_size),
                    static_cast<unsigned>(ARTWORK_MAX_COMPRESSED_BYTES));
            } else {
                ESP_LOGW(TAG, "封面异步读取失败：track=%lu source=%u size=%lu result=%s",
                    static_cast<unsigned long>(request->track_index),
                    static_cast<unsigned>(request->ref.source),
                    static_cast<unsigned long>(request->ref.data_size),
                    esp_err_to_name(load_ret));
            }
            artwork_publish_state(ArtworkLoadState::Failed, request, load_ret, false, 0U,
                MediaArtworkFormatV2::Unknown, request->ref.width, request->ref.height);
            artwork_request_release(request);
            continue;
        }

        if (!cache_insert(request, data, size, actual_format, request->ref.width, request->ref.height)) {
            heap_caps_free(data);
            artwork_publish_state(ArtworkLoadState::Failed, request, ESP_ERR_NO_MEM, false, 0U,
                actual_format, request->ref.width, request->ref.height);
            ESP_LOGW(TAG, "封面缓存没有可淘汰空间：track=%lu bytes=%u（可能存在被 UI 固定的条目）",
                static_cast<unsigned long>(request->track_index), static_cast<unsigned>(size));
            artwork_request_release(request);
            continue;
        }

        artwork_publish_state(ArtworkLoadState::Ready, request, ESP_OK, false, static_cast<uint32_t>(size),
            actual_format, request->ref.width, request->ref.height);
        ARTWORK_LOAD_TRACE("READY request=%lu track=%lu bytes=%u format=%u stack_hwm=%u",
            static_cast<unsigned long>(request->request_id),
            static_cast<unsigned long>(request->track_index),
            static_cast<unsigned>(size),
            static_cast<unsigned>(actual_format),
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        // data 所有权已转移给 cache。
        artwork_request_release(request);
    }
}

esp_err_t artwork_loader_start()
{
    if (g_ready || g_artwork_task != nullptr) {
        return ESP_OK;
    }
    if (!media_library_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (g_request_queue == nullptr) {
        g_request_queue = xQueueCreate(1U, sizeof(ArtworkLoadRequest *));
    }
    if (g_submit_mutex == nullptr) {
        g_submit_mutex = xSemaphoreCreateMutex();
    }
    if (g_cache_mutex == nullptr) {
        g_cache_mutex = xSemaphoreCreateMutex();
    }
    if (g_request_queue == nullptr || g_submit_mutex == nullptr || g_cache_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t task_ret = xTaskCreatePinnedToCore(
        artwork_task_main,
        "ArtworkTask",
        ARTWORK_TASK_STACK_BYTES,
        nullptr,
        ARTWORK_TASK_PRIORITY,
        &g_artwork_task,
        ARTWORK_TASK_CORE
    );
    if (task_ret != pdPASS) {
        g_artwork_task = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "R.36 ArtworkTask：核心=%ld，优先级=%u，栈=%uB，增量切片=2/4/8KB，压缩原图=2交换槽/%uKB硬上限/Surface成功即释放",
        static_cast<long>(ARTWORK_TASK_CORE),
        static_cast<unsigned>(ARTWORK_TASK_PRIORITY),
        static_cast<unsigned>(ARTWORK_TASK_STACK_BYTES),
        static_cast<unsigned>(ARTWORK_CACHE_BUDGET_BYTES / 1024U));
    ESP_LOGI(TAG, "P1.5R.1 Tick Hygiene：SD锁=non-blocking，竞争重试=1tick，slice后让步=1tick，RTOS=%uHz",
        static_cast<unsigned>(configTICK_RATE_HZ));
    return ESP_OK;
}

bool artwork_loader_is_ready()
{
    return g_ready && g_artwork_task != nullptr;
}

bool artwork_loader_request_track(uint32_t track_index, uint32_t *out_request_id)
{
    if (!artwork_loader_is_ready() || g_request_queue == nullptr || g_submit_mutex == nullptr ||
        track_index >= media_library_get_count()) {
        return false;
    }

    ArtworkLoadRequest *request = static_cast<ArtworkLoadRequest *>(
        heap_caps_calloc(1, sizeof(ArtworkLoadRequest), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (request == nullptr) {
        return false;
    }
    request->request_id = artwork_next_request_id();
    request->track_index = track_index;

    // Catalog 指针只在这里短期使用；入队前把 locator 和 source path 全部复制出来。
    const uint32_t generation_before = media_catalog_v2_generation();
    MediaArtworkViewV2 view = {};
    if (media_library_get_artwork_view(track_index, &view) && view.ref != nullptr) {
        request->catalog_generation = view.generation;
        request->ref = *view.ref;
        const char *source_path = request->ref.source == MediaArtworkSourceV2::ExternalFile
            ? view.external_path
            : media_library_get_path(track_index);
        request->source_path = artwork_psram_strdup(source_path);
        if (request->source_path == nullptr) {
            artwork_request_release(request);
            return false;
        }
        request->has_artwork = true;
    } else {
        request->catalog_generation = generation_before;
        request->has_artwork = false;
    }

    if (request->catalog_generation != generation_before || media_catalog_v2_generation() != generation_before) {
        artwork_request_release(request);
        return false;
    }

    if (xSemaphoreTake(g_submit_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        artwork_request_release(request);
        return false;
    }

    artwork_set_latest_request_id(request->request_id);

    // 队列长度固定为 1。若旧请求尚未被 ArtworkTask 取走，先显式释放旧请求对象，
    // 再用 overwrite 投递最新指针。这样队列写入本身不会存在“清掉旧请求后新请求又发送失败”的悬空 latest 状态。
    ArtworkLoadRequest *superseded = nullptr;
    if (xQueueReceive(g_request_queue, &superseded, 0) == pdTRUE && superseded != nullptr) {
        artwork_count_superseded();
        ARTWORK_LOAD_TRACE("COALESCE old_request=%lu track=%lu -> request=%lu track=%lu",
            static_cast<unsigned long>(superseded->request_id),
            static_cast<unsigned long>(superseded->track_index),
            static_cast<unsigned long>(request->request_id),
            static_cast<unsigned long>(request->track_index));
        artwork_request_release(superseded);
    }

    // FreeRTOS 明确定义 xQueueOverwrite() 用于长度 1 队列；这里即使 ArtworkTask
    // 恰好在上述 receive 与此处之间取走队列，也只会把最新 request 放回空队列。
    const BaseType_t queued = xQueueOverwrite(g_request_queue, &request);
    xSemaphoreGive(g_submit_mutex);
    if (queued != pdPASS) {
        // 对长度 1 的有效队列理论上不会失败；仅保留防御分支。
        artwork_set_latest_request_id(0U);
        artwork_request_release(request);
        return false;
    }

    if (out_request_id != nullptr) {
        *out_request_id = request->request_id;
    }
    ARTWORK_LOAD_TRACE("PUBLISH request=%lu generation=%lu track=%lu artwork=%u source=%u size=%lu",
        static_cast<unsigned long>(request->request_id),
        static_cast<unsigned long>(request->catalog_generation),
        static_cast<unsigned long>(request->track_index),
        request->has_artwork ? 1U : 0U,
        static_cast<unsigned>(request->ref.source),
        static_cast<unsigned long>(request->ref.data_size));
    return true;
}

bool artwork_loader_get_snapshot(ArtworkLoaderSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }
    portENTER_CRITICAL(&g_snapshot_mux);
    *out_snapshot = g_snapshot;
    out_snapshot->ready = g_ready;
    portEXIT_CRITICAL(&g_snapshot_mux);
    return true;
}

bool artwork_loader_get_cache_stats(ArtworkCacheStats *out_stats)
{
    if (out_stats == nullptr || g_cache_mutex == nullptr ||
        xSemaphoreTake(g_cache_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    ArtworkCacheStats stats = {};
    stats.budget_bytes = ARTWORK_CACHE_BUDGET_BYTES;
    for (size_t i = 0; i < ARTWORK_CACHE_SLOT_COUNT; ++i) {
        if (!g_cache[i].valid) continue;
        ++stats.entry_count;
        stats.bytes += g_cache[i].size;
        if (g_cache[i].pin_count != 0U) ++stats.pinned_count;
    }
    xSemaphoreGive(g_cache_mutex);
    *out_stats = stats;
    return true;
}

bool artwork_loader_acquire_cached(uint32_t track_index, ArtworkCacheLease *out_lease)
{
    if (out_lease == nullptr || !artwork_loader_is_ready() || g_cache_mutex == nullptr) {
        return false;
    }
    *out_lease = {};
    if (xSemaphoreTake(g_cache_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const uint32_t generation = media_catalog_v2_generation();
    const int index = cache_find_locked(generation, track_index);
    if (index < 0) {
        xSemaphoreGive(g_cache_mutex);
        return false;
    }
    ArtworkCacheEntry &entry = g_cache[index];
    ++entry.pin_count;
    entry.lru_stamp = g_lru_counter++;
    if (g_lru_counter == 0U) g_lru_counter = 1U;

    out_lease->data = entry.data;
    out_lease->size = entry.size;
    out_lease->format = entry.format;
    out_lease->width = entry.width;
    out_lease->height = entry.height;
    out_lease->catalog_generation = entry.catalog_generation;
    out_lease->track_index = entry.track_index;
    out_lease->slot_revision = entry.slot_revision;
    out_lease->slot_index = static_cast<uint8_t>(index);
    xSemaphoreGive(g_cache_mutex);
    return true;
}

void artwork_loader_release_cached(ArtworkCacheLease *lease)
{
    if (lease == nullptr) {
        return;
    }
    if (g_cache_mutex != nullptr && lease->slot_index < ARTWORK_CACHE_SLOT_COUNT &&
        xSemaphoreTake(g_cache_mutex, portMAX_DELAY) == pdTRUE) {
        ArtworkCacheEntry &entry = g_cache[lease->slot_index];
        if (entry.valid && entry.slot_revision == lease->slot_revision && entry.pin_count != 0U) {
            --entry.pin_count;
        }
        xSemaphoreGive(g_cache_mutex);
    }
    *lease = {};
}

void artwork_loader_discard_unpinned()
{
    if (g_cache_mutex == nullptr || xSemaphoreTake(g_cache_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    size_t released = 0U;
    for (size_t i = 0; i < ARTWORK_CACHE_SLOT_COUNT; ++i) {
        if (g_cache[i].valid && g_cache[i].pin_count == 0U) {
            released += g_cache[i].size;
            cache_release_entry_locked(&g_cache[i]);
        }
    }
    xSemaphoreGive(g_cache_mutex);
    if (released > 0U) {
        ESP_LOGI(TAG, "R.36 压缩原图已释放：%uB PSRAM_free=%u",
            static_cast<unsigned>(released),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
}
