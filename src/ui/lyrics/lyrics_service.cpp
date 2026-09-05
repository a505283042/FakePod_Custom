#include "lyrics_service.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "app_diag_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "flac_decoder.h"
#include "media_catalog_v2.h"
#include "storage_io.h"

namespace {

static const char *TAG = "歌词服务";

static constexpr uint32_t LYRICS_TASK_STACK = 4096U;
static constexpr UBaseType_t LYRICS_TASK_PRIORITY = 1U;
static constexpr BaseType_t LYRICS_TASK_CORE = 1;
static constexpr size_t LYRICS_FILE_MAX_BYTES = 256U * 1024U;
static constexpr size_t LYRICS_READ_CHUNK_BYTES = 2048U;
static constexpr uint32_t LYRICS_FLAC_SAFE_PERCENT = 92U;
static constexpr TickType_t LYRICS_RETRY_DELAY_TICKS = 1U;
static constexpr size_t LYRICS_MAX_TIMESTAMPS_PER_TEXT_LINE = 16U;

struct LyricsLoadRequest
{
    uint32_t request_id = 0;
    uint32_t catalog_generation = 0;
    uint32_t track_index = UINT32_MAX;
};

struct LyricsLineRow
{
    uint32_t time_ms = 0;
    uint32_t text_off = 0;
};

struct LyricsDocument
{
    uint32_t catalog_generation = 0;
    uint32_t track_index = UINT32_MAX;
    LyricsLineRow *lines = nullptr;
    uint32_t line_count = 0;
    char *text_pool = nullptr;
    uint32_t text_pool_size = 0;
};

static QueueHandle_t g_queue = nullptr;
static SemaphoreHandle_t g_mutex = nullptr;
static TaskHandle_t g_task = nullptr;
static bool g_ready = false;
static volatile bool g_storage_handoff = false;
static volatile bool g_sd_file_open = false;
static uint32_t g_next_request_id = 1U;
static uint32_t g_latest_request_id = 0U;
static LyricsLoadState g_state = LyricsLoadState::Idle;
static esp_err_t g_result = ESP_OK;
static uint32_t g_revision = 1U;
static uint32_t g_loading_generation = 0U;
static uint32_t g_loading_track = UINT32_MAX;
static LyricsDocument g_document = {};

static void *lyrics_psram_alloc(size_t bytes)
{
    if (bytes == 0U) {
        return nullptr;
    }
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        memory = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return memory;
}

static void lyrics_document_release(LyricsDocument *doc)
{
    if (doc == nullptr) {
        return;
    }
    heap_caps_free(doc->lines);
    heap_caps_free(doc->text_pool);
    *doc = {};
}

static bool lyrics_request_is_latest(uint32_t request_id)
{
    if (g_mutex == nullptr) {
        return false;
    }
    if (xSemaphoreTake(g_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool latest = !g_storage_handoff && request_id == g_latest_request_id;
    xSemaphoreGive(g_mutex);
    return latest;
}

static void lyrics_publish_state(
    const LyricsLoadRequest &request,
    LyricsLoadState state,
    esp_err_t result,
    LyricsDocument *document)
{
    if (g_mutex == nullptr || xSemaphoreTake(g_mutex, portMAX_DELAY) != pdTRUE) {
        if (document != nullptr) {
            lyrics_document_release(document);
        }
        return;
    }

    if (request.request_id != g_latest_request_id) {
        xSemaphoreGive(g_mutex);
        if (document != nullptr) {
            lyrics_document_release(document);
        }
        return;
    }

    if (document != nullptr) {
        lyrics_document_release(&g_document);
        g_document = *document;
        *document = {};
    } else if (state != LyricsLoadState::Ready) {
        lyrics_document_release(&g_document);
    }

    g_state = state;
    g_result = result;
    g_loading_generation = request.catalog_generation;
    g_loading_track = request.track_index;
    ++g_revision;
    xSemaphoreGive(g_mutex);
}

static void lyrics_mark_loading(const LyricsLoadRequest &request)
{
    if (g_mutex == nullptr || xSemaphoreTake(g_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (request.request_id == g_latest_request_id) {
        g_state = LyricsLoadState::Loading;
        g_result = ESP_OK;
        g_loading_generation = request.catalog_generation;
        g_loading_track = request.track_index;
        ++g_revision;
    }
    xSemaphoreGive(g_mutex);
}

static bool lyrics_storage_window_open()
{
    FlacStorageWindowSnapshot window = {};
    if (!flac_decoder_get_storage_window(&window) || !window.active || !window.storage_competes) {
        return true;
    }
    if (window.capacity_bytes == 0U) {
        return false;
    }
    return static_cast<uint64_t>(window.buffered_bytes) * 100ULL >=
        static_cast<uint64_t>(window.capacity_bytes) * LYRICS_FLAC_SAFE_PERCENT;
}

static bool lyrics_copy_external_lrc_path(uint32_t track_index, uint32_t generation, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0U || !media_catalog_v2_ready() ||
        media_catalog_v2_generation() != generation) {
        return false;
    }

    MediaTrackViewV2 track = {};
    if (!media_catalog_v2_get_track_view(track_index, &track) || track.row == nullptr ||
        track.generation != generation) {
        return false;
    }

    const MusicCatalogV2 *catalog = media_catalog_v2_current();
    if (catalog == nullptr) {
        return false;
    }

    // P1.4.1 明确只启用 External + Synced LRC。Catalog 中其他歌词 locator 保留给后续版本。
    for (uint16_t i = 0; i < track.row->lyrics_ref_count; ++i) {
        const uint32_t index = track.row->lyrics_ref_start + i;
        if (index >= catalog->lyrics_ref_count) {
            break;
        }
        const LyricsRefV2 &ref = catalog->lyrics_refs[index];
        if (ref.source != MediaLyricsSourceV2::ExternalFile || ref.kind != MediaLyricsKindV2::Synced) {
            continue;
        }
        const char *path = media_catalog_v2_pool_str(catalog, ref.path_off);
        if (path == nullptr || path[0] == '\0') {
            continue;
        }
        const size_t length = strlen(path);
        if (length + 1U > out_size) {
            return false;
        }
        memcpy(out, path, length + 1U);
        return true;
    }
    return false;
}

static bool lyrics_utf8_validate(const uint8_t *data, size_t size)
{
    if (data == nullptr) {
        return false;
    }
    size_t i = 0U;
    while (i < size) {
        const uint8_t c = data[i];
        if (c < 0x80U) {
            ++i;
            continue;
        }
        size_t need = 0U;
        uint32_t minimum = 0U;
        uint32_t codepoint = 0U;
        if ((c & 0xE0U) == 0xC0U) {
            need = 1U; minimum = 0x80U; codepoint = c & 0x1FU;
        } else if ((c & 0xF0U) == 0xE0U) {
            need = 2U; minimum = 0x800U; codepoint = c & 0x0FU;
        } else if ((c & 0xF8U) == 0xF0U) {
            need = 3U; minimum = 0x10000U; codepoint = c & 0x07U;
        } else {
            return false;
        }
        if (i + need >= size) {
            return false;
        }
        for (size_t n = 0U; n < need; ++n) {
            const uint8_t next = data[i + n + 1U];
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFU ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
        i += need + 1U;
    }
    return true;
}

static esp_err_t lyrics_read_file(
    const LyricsLoadRequest &request,
    const char *path,
    char **out_data,
    size_t *out_size)
{
    if (path == nullptr || out_data == nullptr || out_size == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = nullptr;
    *out_size = 0U;

    struct stat info = {};
    for (;;) {
        if (!lyrics_request_is_latest(request.request_id)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!lyrics_storage_window_open()) {
            vTaskDelay(LYRICS_RETRY_DELAY_TICKS);
            continue;
        }
        StorageSdLockGuard guard(0);
        if (!guard) {
            vTaskDelay(LYRICS_RETRY_DELAY_TICKS);
            continue;
        }
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
            return ESP_ERR_NOT_FOUND;
        }
        break;
    }

    if (info.st_size <= 0 || static_cast<uint64_t>(info.st_size) > LYRICS_FILE_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t size = static_cast<size_t>(info.st_size);
    char *data = static_cast<char *>(lyrics_psram_alloc(size + 1U));
    if (data == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    FILE *file = nullptr;
    for (;;) {
        if (!lyrics_request_is_latest(request.request_id)) {
            heap_caps_free(data);
            return ESP_ERR_INVALID_STATE;
        }
        if (!lyrics_storage_window_open()) {
            vTaskDelay(LYRICS_RETRY_DELAY_TICKS);
            continue;
        }
        StorageSdLockGuard guard(0);
        if (!guard) {
            vTaskDelay(LYRICS_RETRY_DELAY_TICKS);
            continue;
        }
        if (!lyrics_request_is_latest(request.request_id)) {
            break;
        }
        file = fopen(path, "rb");
        if (file != nullptr) g_sd_file_open = true;
        break;
    }
    if (file == nullptr) {
        heap_caps_free(data);
        return ESP_ERR_NOT_FOUND;
    }

    size_t loaded = 0U;
    esp_err_t result = ESP_OK;
    while (loaded < size) {
        if (!lyrics_request_is_latest(request.request_id)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        if (!lyrics_storage_window_open()) {
            vTaskDelay(LYRICS_RETRY_DELAY_TICKS);
            continue;
        }

        const size_t remaining = size - loaded;
        const size_t wanted = remaining > LYRICS_READ_CHUNK_BYTES ? LYRICS_READ_CHUNK_BYTES : remaining;
        size_t got = 0U;
        {
            StorageSdLockGuard guard(0);
            if (!guard) {
                vTaskDelay(LYRICS_RETRY_DELAY_TICKS);
                continue;
            }
            got = fread(data + loaded, 1U, wanted, file);
        }
        if (got == 0U) {
            result = ESP_FAIL;
            break;
        }
        loaded += got;
        // 真正阻塞至少 1 RTOS tick，避免 LyricsTask 在 Core1 形成新的 ready-loop。
        vTaskDelay(1);
    }

    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) {
            fclose(file);
            g_sd_file_open = false;
        }
    }

    if (result != ESP_OK || loaded != size) {
        heap_caps_free(data);
        return result != ESP_OK ? result : ESP_FAIL;
    }
    data[size] = '\0';
    *out_data = data;
    *out_size = size;
    return ESP_OK;
}

static bool parse_uint_component(const char *&p, const char *end, uint32_t *value)
{
    if (p == nullptr || value == nullptr || p >= end || *p < '0' || *p > '9') {
        return false;
    }
    uint32_t parsed = 0U;
    while (p < end && *p >= '0' && *p <= '9') {
        parsed = parsed * 10U + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    *value = parsed;
    return true;
}

static bool lyrics_parse_timestamp_tag(const char *begin, const char *end, uint32_t *out_ms)
{
    if (begin == nullptr || end == nullptr || out_ms == nullptr || begin >= end) {
        return false;
    }
    const char *p = begin;
    uint32_t minutes = 0U;
    uint32_t seconds = 0U;
    if (!parse_uint_component(p, end, &minutes) || p >= end || *p != ':') {
        return false;
    }
    ++p;
    if (!parse_uint_component(p, end, &seconds) || seconds >= 60U) {
        return false;
    }

    uint32_t fraction_ms = 0U;
    if (p < end && (*p == '.' || *p == ':')) {
        ++p;
        uint32_t fraction = 0U;
        uint32_t digits = 0U;
        while (p < end && *p >= '0' && *p <= '9' && digits < 3U) {
            fraction = fraction * 10U + static_cast<uint32_t>(*p - '0');
            ++p;
            ++digits;
        }
        while (p < end && *p >= '0' && *p <= '9') {
            ++p; // 超过毫秒精度的尾数直接忽略。
        }
        if (digits == 1U) fraction_ms = fraction * 100U;
        else if (digits == 2U) fraction_ms = fraction * 10U;
        else if (digits == 3U) fraction_ms = fraction;
    }
    if (p != end) {
        return false;
    }

    const uint64_t total = static_cast<uint64_t>(minutes) * 60000ULL +
        static_cast<uint64_t>(seconds) * 1000ULL + fraction_ms;
    if (total > UINT32_MAX) {
        return false;
    }
    *out_ms = static_cast<uint32_t>(total);
    return true;
}

static bool lyrics_parse_offset_tag(const char *begin, const char *end, int32_t *out_offset_ms)
{
    static constexpr char kPrefix[] = "offset:";
    const size_t prefix_len = sizeof(kPrefix) - 1U;
    if (begin == nullptr || end == nullptr || out_offset_ms == nullptr ||
        static_cast<size_t>(end - begin) <= prefix_len || strncmp(begin, kPrefix, prefix_len) != 0) {
        return false;
    }
    const char *p = begin + prefix_len;
    int32_t sign = 1;
    if (p < end && (*p == '+' || *p == '-')) {
        sign = *p == '-' ? -1 : 1;
        ++p;
    }
    if (p >= end) {
        return false;
    }
    int64_t value = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        if (value > INT32_MAX) {
            return false;
        }
        ++p;
    }
    if (p != end) {
        return false;
    }
    *out_offset_ms = static_cast<int32_t>(value) * sign;
    return true;
}

static const char *lyrics_line_end(const char *p, const char *end)
{
    while (p < end && *p != '\r' && *p != '\n') {
        ++p;
    }
    return p;
}

static const char *lyrics_next_line(const char *p, const char *end)
{
    while (p < end && (*p == '\r' || *p == '\n')) {
        ++p;
    }
    return p;
}

static size_t lyrics_count_timestamps(const char *data, size_t size, int32_t *out_offset_ms)
{
    const char *p = data;
    const char *end = data + size;
    size_t count = 0U;
    int32_t global_offset = 0;
    while (p < end) {
        const char *line_end = lyrics_line_end(p, end);
        const char *cursor = p;
        while (cursor < line_end && *cursor == '[') {
            const char *close = static_cast<const char *>(memchr(cursor + 1, ']', static_cast<size_t>(line_end - cursor - 1)));
            if (close == nullptr) {
                break;
            }
            uint32_t timestamp = 0U;
            if (lyrics_parse_timestamp_tag(cursor + 1, close, &timestamp)) {
                ++count;
            } else {
                int32_t parsed_offset = 0;
                if (lyrics_parse_offset_tag(cursor + 1, close, &parsed_offset)) {
                    global_offset = parsed_offset;
                }
            }
            cursor = close + 1;
        }
        p = lyrics_next_line(line_end, end);
    }
    if (out_offset_ms != nullptr) {
        *out_offset_ms = global_offset;
    }
    return count;
}

static int lyrics_line_compare(const void *left, const void *right)
{
    const LyricsLineRow *a = static_cast<const LyricsLineRow *>(left);
    const LyricsLineRow *b = static_cast<const LyricsLineRow *>(right);
    if (a->time_ms < b->time_ms) return -1;
    if (a->time_ms > b->time_ms) return 1;
    return 0;
}

static esp_err_t lyrics_parse_lrc(char *data, size_t size, LyricsDocument *out_document)
{
    if (data == nullptr || size == 0U || out_document == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
    size_t utf8_offset = 0U;
    if (size >= 3U && bytes[0] == 0xEFU && bytes[1] == 0xBBU && bytes[2] == 0xBFU) {
        utf8_offset = 3U;
    } else if (size >= 2U && ((bytes[0] == 0xFFU && bytes[1] == 0xFEU) ||
                              (bytes[0] == 0xFEU && bytes[1] == 0xFFU))) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!lyrics_utf8_validate(bytes + utf8_offset, size - utf8_offset)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    int32_t global_offset_ms = 0;
    const size_t timestamp_count = lyrics_count_timestamps(data + utf8_offset, size - utf8_offset, &global_offset_ms);
    if (timestamp_count == 0U || timestamp_count > UINT32_MAX) {
        return ESP_ERR_NOT_FOUND;
    }

    LyricsLineRow *rows = static_cast<LyricsLineRow *>(lyrics_psram_alloc(timestamp_count * sizeof(LyricsLineRow)));
    char *text_pool = static_cast<char *>(lyrics_psram_alloc(size + 4U));
    if (rows == nullptr || text_pool == nullptr) {
        heap_caps_free(rows);
        heap_caps_free(text_pool);
        return ESP_ERR_NO_MEM;
    }
    memset(rows, 0, timestamp_count * sizeof(LyricsLineRow));
    text_pool[0] = '\0';
    uint32_t text_used = 1U;
    uint32_t row_used = 0U;

    const char *p = data + utf8_offset;
    const char *end = data + size;
    while (p < end && row_used < timestamp_count) {
        const char *line_end = lyrics_line_end(p, end);
        const char *cursor = p;
        uint32_t timestamps[LYRICS_MAX_TIMESTAMPS_PER_TEXT_LINE] = {};
        size_t timestamp_used = 0U;

        while (cursor < line_end && *cursor == '[') {
            const char *close = static_cast<const char *>(memchr(cursor + 1, ']', static_cast<size_t>(line_end - cursor - 1)));
            if (close == nullptr) {
                break;
            }
            uint32_t timestamp = 0U;
            if (lyrics_parse_timestamp_tag(cursor + 1, close, &timestamp)) {
                if (timestamp_used < LYRICS_MAX_TIMESTAMPS_PER_TEXT_LINE) {
                    timestamps[timestamp_used++] = timestamp;
                }
            }
            cursor = close + 1;
        }

        if (timestamp_used > 0U) {
            while (cursor < line_end && (*cursor == ' ' || *cursor == '\t')) {
                ++cursor;
            }
            const char *text_end = line_end;
            while (text_end > cursor && (text_end[-1] == ' ' || text_end[-1] == '\t')) {
                --text_end;
            }
            const size_t text_len = static_cast<size_t>(text_end - cursor);
            if (text_used + text_len + 1U <= size + 4U) {
                const uint32_t text_off = text_used;
                if (text_len > 0U) {
                    memcpy(text_pool + text_used, cursor, text_len);
                }
                text_pool[text_used + text_len] = '\0';
                text_used += static_cast<uint32_t>(text_len + 1U);

                for (size_t i = 0U; i < timestamp_used && row_used < timestamp_count; ++i) {
                    int64_t adjusted = static_cast<int64_t>(timestamps[i]) + global_offset_ms;
                    if (adjusted < 0) adjusted = 0;
                    if (adjusted > UINT32_MAX) adjusted = UINT32_MAX;
                    rows[row_used].time_ms = static_cast<uint32_t>(adjusted);
                    rows[row_used].text_off = text_off;
                    ++row_used;
                }
            }
        }
        p = lyrics_next_line(line_end, end);
    }

    if (row_used == 0U) {
        heap_caps_free(rows);
        heap_caps_free(text_pool);
        return ESP_ERR_NOT_FOUND;
    }

    qsort(rows, row_used, sizeof(LyricsLineRow), lyrics_line_compare);

    // 同一时间戳只保留最后一条，避免某些 LRC 重复标签让当前行来回抖动。
    uint32_t compact = 0U;
    for (uint32_t i = 0U; i < row_used; ++i) {
        if (compact > 0U && rows[compact - 1U].time_ms == rows[i].time_ms) {
            rows[compact - 1U] = rows[i];
        } else {
            rows[compact++] = rows[i];
        }
    }

    out_document->lines = rows;
    out_document->line_count = compact;
    out_document->text_pool = text_pool;
    out_document->text_pool_size = text_used;
    return ESP_OK;
}

static const char *lyrics_document_text(const LyricsDocument &document, uint32_t offset)
{
    if (document.text_pool == nullptr || offset >= document.text_pool_size) {
        return "";
    }
    return document.text_pool + offset;
}

static void lyrics_task(void *arg)
{
    (void)arg;
    LyricsLoadRequest request = {};
    for (;;) {
        if (xQueueReceive(g_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        lyrics_mark_loading(request);

        char path[512] = {};
        if (!lyrics_copy_external_lrc_path(request.track_index, request.catalog_generation, path, sizeof(path))) {
            ESP_LOGI(TAG, "当前曲没有 External Synced LRC：track=%lu generation=%lu",
                static_cast<unsigned long>(request.track_index),
                static_cast<unsigned long>(request.catalog_generation));
            lyrics_publish_state(request, LyricsLoadState::NoLyrics, ESP_ERR_NOT_FOUND, nullptr);
            continue;
        }

        ESP_LOGI(TAG, "开始读取 LRC：track=%lu path=%s",
            static_cast<unsigned long>(request.track_index), path);

        char *file_data = nullptr;
        size_t file_size = 0U;
        esp_err_t ret = lyrics_read_file(request, path, &file_data, &file_size);
        if (ret != ESP_OK) {
            if (ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "LRC 读取失败：track=%lu err=%s",
                    static_cast<unsigned long>(request.track_index), esp_err_to_name(ret));
                lyrics_publish_state(request, LyricsLoadState::Failed, ret, nullptr);
            }
            heap_caps_free(file_data);
            continue;
        }

        LyricsDocument parsed = {};
        ret = lyrics_parse_lrc(file_data, file_size, &parsed);
        heap_caps_free(file_data);
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "LRC 编码暂不支持：track=%lu；仅支持 UTF-8/UTF-8 BOM",
                static_cast<unsigned long>(request.track_index));
            lyrics_publish_state(request, LyricsLoadState::Unsupported, ret, nullptr);
            continue;
        }
        if (ret != ESP_OK) {
            const LyricsLoadState state = ret == ESP_ERR_NOT_FOUND ? LyricsLoadState::NoLyrics : LyricsLoadState::Failed;
            ESP_LOGW(TAG, "LRC 解析失败：track=%lu err=%s",
                static_cast<unsigned long>(request.track_index), esp_err_to_name(ret));
            lyrics_publish_state(request, state, ret, nullptr);
            continue;
        }

        parsed.catalog_generation = request.catalog_generation;
        parsed.track_index = request.track_index;
        const uint32_t line_count = parsed.line_count;
        const uint32_t pool_size = parsed.text_pool_size;
        lyrics_publish_state(request, LyricsLoadState::Ready, ESP_OK, &parsed);
        ESP_LOGI(TAG, "LRC 已就绪：track=%lu lines=%lu text_pool=%luB",
            static_cast<unsigned long>(request.track_index),
            static_cast<unsigned long>(line_count),
            static_cast<unsigned long>(pool_size));
    }
}

} // namespace

const char *lyrics_load_state_name_cn(LyricsLoadState state)
{
    switch (state) {
        case LyricsLoadState::Idle: return "空闲";
        case LyricsLoadState::Loading: return "加载中";
        case LyricsLoadState::Ready: return "就绪";
        case LyricsLoadState::NoLyrics: return "无歌词";
        case LyricsLoadState::Unsupported: return "不支持";
        case LyricsLoadState::Failed: return "失败";
    }
    return "未知";
}

esp_err_t lyrics_service_start()
{
    if (g_ready) {
        return ESP_OK;
    }
    g_mutex = xSemaphoreCreateMutex();
    g_queue = xQueueCreate(1, sizeof(LyricsLoadRequest));
    if (g_mutex == nullptr || g_queue == nullptr) {
        if (g_queue != nullptr) vQueueDelete(g_queue);
        if (g_mutex != nullptr) vSemaphoreDelete(g_mutex);
        g_queue = nullptr;
        g_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        lyrics_task,
        "LyricsTask",
        LYRICS_TASK_STACK,
        nullptr,
        LYRICS_TASK_PRIORITY,
        &g_task,
        LYRICS_TASK_CORE);
    if (created != pdPASS) {
        vQueueDelete(g_queue);
        vSemaphoreDelete(g_mutex);
        g_queue = nullptr;
        g_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }

    g_ready = true;
#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(TAG,
        "LyricsTask 已启动：core=%d priority=%u stack=%uB chunk=%uB FLAC安全水位=%u%%，格式=External UTF-8 LRC",
        static_cast<int>(LYRICS_TASK_CORE),
        static_cast<unsigned>(LYRICS_TASK_PRIORITY),
        static_cast<unsigned>(LYRICS_TASK_STACK),
        static_cast<unsigned>(LYRICS_READ_CHUNK_BYTES),
        static_cast<unsigned>(LYRICS_FLAC_SAFE_PERCENT));
#endif
    return ESP_OK;
}

bool lyrics_service_is_ready()
{
    return g_ready;
}

bool lyrics_service_prepare_storage_handoff(TickType_t timeout_ticks)
{
    if (!g_ready) return true;

    if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_storage_handoff = true;
        // 让正在读的 request 在下一个 2KB slice 退出；队列中的旧请求也不再成为 latest。
        ++g_latest_request_id;
        if (g_latest_request_id == 0U) g_latest_request_id = 1U;
        xSemaphoreGive(g_mutex);
    } else {
        g_storage_handoff = true;
    }
    if (g_queue != nullptr) xQueueReset(g_queue);

    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        {
            StorageSdLockGuard guard(pdMS_TO_TICKS(20));
            if (guard && !g_sd_file_open) return true;
        }
        if (timeout_ticks != portMAX_DELAY && xTaskGetTickCount() - start >= timeout_ticks) {
            ESP_LOGE(TAG, "USB接管等待歌词文件关闭超时");
            return false;
        }
        vTaskDelay(1);
    }
}

void lyrics_service_resume_storage_after_handoff()
{
    g_storage_handoff = false;
}

bool lyrics_service_request_track(uint32_t track_index)
{
    if (g_storage_handoff || !g_ready || g_queue == nullptr || g_mutex == nullptr || !media_catalog_v2_ready() ||
        track_index >= media_catalog_v2_current()->track_count) {
        return false;
    }

    LyricsLoadRequest request = {};
    request.catalog_generation = media_catalog_v2_generation();
    request.track_index = track_index;

    if (xSemaphoreTake(g_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    request.request_id = g_next_request_id++;
    if (g_next_request_id == 0U) {
        g_next_request_id = 1U;
    }
    g_latest_request_id = request.request_id;
    g_state = LyricsLoadState::Loading;
    g_result = ESP_OK;
    g_loading_generation = request.catalog_generation;
    g_loading_track = request.track_index;
    ++g_revision;
    xSemaphoreGive(g_mutex);

    return xQueueOverwrite(g_queue, &request) == pdPASS;
}

bool lyrics_service_get_window(uint32_t track_index, uint64_t position_ms, LyricsWindowSnapshot *out_snapshot)
{
    if (!g_ready || g_mutex == nullptr || out_snapshot == nullptr) {
        return false;
    }
    *out_snapshot = {};

    if (xSemaphoreTake(g_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    out_snapshot->state = g_state;
    out_snapshot->result = g_result;
    out_snapshot->revision = g_revision;
    out_snapshot->catalog_generation = g_loading_generation;
    out_snapshot->track_index = g_loading_track;

    if (g_state != LyricsLoadState::Ready || g_document.track_index != track_index ||
        g_document.lines == nullptr || g_document.line_count == 0U) {
        xSemaphoreGive(g_mutex);
        return true;
    }

    out_snapshot->line_count = g_document.line_count;
    uint32_t current = UINT32_MAX;
    uint32_t low = 0U;
    uint32_t high = g_document.line_count;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2U;
        if (static_cast<uint64_t>(g_document.lines[mid].time_ms) <= position_ms) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    if (low > 0U) {
        current = low - 1U;
    }
    out_snapshot->current_line_index = current;

    // 五行窗口固定把“当前行”放在中间槽位。歌曲开头/结尾用空白占位，
    // 不让第一句突然顶到最上面，也避免最后几句上下跳位。
    const int64_t center_index = current == UINT32_MAX ? 0 : static_cast<int64_t>(current);
    for (size_t slot = 0U; slot < LYRICS_VIEW_WINDOW_LINES; ++slot) {
        const int64_t line_index_signed = center_index + static_cast<int64_t>(slot) - 2;
        if (line_index_signed < 0 || line_index_signed >= static_cast<int64_t>(g_document.line_count)) {
            continue;
        }
        const uint32_t line_index = static_cast<uint32_t>(line_index_signed);
        LyricsWindowLine &dest = out_snapshot->lines[slot];
        const LyricsLineRow &source = g_document.lines[line_index];
        dest.valid = true;
        dest.current = line_index == current;
        dest.time_ms = source.time_ms;
        const char *text = lyrics_document_text(g_document, source.text_off);
        if (text[0] == '\0') {
            text = "♪";
        }
        snprintf(dest.text, sizeof(dest.text), "%s", text);
    }

    xSemaphoreGive(g_mutex);
    return true;
}
