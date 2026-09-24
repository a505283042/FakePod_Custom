#include "ebook_reader_model.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage_io.h"

namespace EbookReader
{
namespace
{

static constexpr TickType_t kStorageLockTimeout = pdMS_TO_TICKS(30);
static constexpr size_t kReadChunkBytes = 1024;
static constexpr size_t kEncodingProbeBytes = 16U * 1024U;
static constexpr size_t kDecodedPageBytes = kPageReadBytes * 3U / 2U + 8U;
static constexpr size_t kDecodedSourceMapEntries = kDecodedPageBytes + 1U;

static bool name_is_hidden(const char *name)
{
    return name == nullptr || name[0] == '\0' || name[0] == '.';
}

static int ascii_tolower(int value)
{
    return (value >= 'A' && value <= 'Z') ? value + ('a' - 'A') : value;
}

static int ascii_casecmp(const char *lhs, const char *rhs)
{
    if (lhs == nullptr) return rhs == nullptr ? 0 : -1;
    if (rhs == nullptr) return 1;
    while (*lhs != '\0' && *rhs != '\0') {
        const int a = ascii_tolower(static_cast<unsigned char>(*lhs));
        const int b = ascii_tolower(static_cast<unsigned char>(*rhs));
        if (a != b) return a < b ? -1 : 1;
        ++lhs;
        ++rhs;
    }
    if (*lhs == *rhs) return 0;
    return *lhs == '\0' ? -1 : 1;
}

static constexpr size_t kInitialDirectoryEntryCapacity = 32U;
static constexpr size_t kInitialDirectoryPoolCapacity = 2048U;

static bool directory_index_is_directory(const DirectoryEntryIndex &entry)
{
    return (entry.flags & kDirectoryEntryDirectory) != 0U;
}

static const char *directory_pool_string(
    const char *pool, size_t pool_size, uint32_t offset)
{
    if (pool == nullptr || static_cast<size_t>(offset) >= pool_size) return nullptr;
    const char *text = pool + offset;
    return memchr(text, '\0', pool_size - static_cast<size_t>(offset)) != nullptr
        ? text
        : nullptr;
}

static int directory_entry_compare(
    const DirectoryEntryIndex &lhs,
    const DirectoryEntryIndex &rhs,
    const char *pool,
    size_t pool_size)
{
    const bool lhs_dir = directory_index_is_directory(lhs);
    const bool rhs_dir = directory_index_is_directory(rhs);
    if (lhs_dir != rhs_dir) return lhs_dir ? -1 : 1;

    const char *lhs_name = directory_pool_string(pool, pool_size, lhs.name_off);
    const char *rhs_name = directory_pool_string(pool, pool_size, rhs.name_off);
    if (lhs_name == nullptr) return rhs_name == nullptr ? 0 : 1;
    if (rhs_name == nullptr) return -1;
    const int folded = ascii_casecmp(lhs_name, rhs_name);
    return folded != 0 ? folded : strcmp(lhs_name, rhs_name);
}

static bool grow_psram_block(void **block, size_t old_bytes, size_t new_bytes)
{
    if (block == nullptr || new_bytes <= old_bytes) return false;
    void *next = heap_caps_malloc(new_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (next == nullptr) return false;
    if (*block != nullptr && old_bytes > 0) memcpy(next, *block, old_bytes);
    if (*block != nullptr) heap_caps_free(*block);
    *block = next;
    return true;
}

static bool reserve_directory_entries(DirectoryScanSession *session, size_t needed)
{
    if (session == nullptr) return false;
    if (needed <= session->entry_capacity) return true;
    if (needed > SIZE_MAX / sizeof(DirectoryEntryIndex)) return false;

    size_t next_capacity = session->entry_capacity > 0
        ? session->entry_capacity
        : kInitialDirectoryEntryCapacity;
    while (next_capacity < needed) {
        if (next_capacity > SIZE_MAX / 2U) {
            next_capacity = needed;
            break;
        }
        next_capacity *= 2U;
    }
    if (next_capacity > SIZE_MAX / sizeof(DirectoryEntryIndex)) return false;

    void *entries = session->entries;
    const size_t old_bytes = session->entry_capacity * sizeof(DirectoryEntryIndex);
    const size_t new_bytes = next_capacity * sizeof(DirectoryEntryIndex);
    if (!grow_psram_block(&entries, old_bytes, new_bytes)) return false;
    session->entries = static_cast<DirectoryEntryIndex *>(entries);
    session->entry_capacity = next_capacity;
    return true;
}

static bool reserve_directory_pool(DirectoryScanSession *session, size_t needed)
{
    if (session == nullptr) return false;
    if (needed <= session->pool_capacity) return true;

    size_t next_capacity = session->pool_capacity > 0
        ? session->pool_capacity
        : kInitialDirectoryPoolCapacity;
    while (next_capacity < needed) {
        if (next_capacity > SIZE_MAX / 2U) {
            next_capacity = needed;
            break;
        }
        next_capacity *= 2U;
    }

    void *pool = session->string_pool;
    if (!grow_psram_block(&pool, session->pool_capacity, next_capacity)) return false;
    session->string_pool = static_cast<char *>(pool);
    session->pool_capacity = next_capacity;
    return true;
}

static esp_err_t append_directory_entry(
    DirectoryScanSession *session,
    const char *name,
    bool is_directory,
    uint64_t size_bytes)
{
    if (session == nullptr || name == nullptr || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t name_bytes = strlen(name) + 1U;
    if (session->pool_size > UINT32_MAX ||
        name_bytes > static_cast<size_t>(UINT32_MAX) - session->pool_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t pool_needed = session->pool_size + name_bytes;
    if (!reserve_directory_entries(session, session->count + 1U) ||
        !reserve_directory_pool(session, pool_needed)) {
        return ESP_ERR_NO_MEM;
    }

    const uint32_t name_off = static_cast<uint32_t>(session->pool_size);
    memcpy(session->string_pool + session->pool_size, name, name_bytes);
    session->pool_size = pool_needed;

    DirectoryEntryIndex &entry = session->entries[session->count++];
    entry.size_bytes = is_directory ? 0U : size_bytes;
    entry.name_off = name_off;
    entry.flags = is_directory ? kDirectoryEntryDirectory : kDirectoryEntryNone;
    return ESP_OK;
}

static void directory_heap_sift_down(
    DirectoryEntryIndex *entries,
    size_t count,
    size_t root,
    const char *pool,
    size_t pool_size)
{
    while (true) {
        const size_t left = root * 2U + 1U;
        if (left >= count) return;
        size_t largest = left;
        const size_t right = left + 1U;
        if (right < count &&
            directory_entry_compare(entries[largest], entries[right], pool, pool_size) < 0) {
            largest = right;
        }
        if (directory_entry_compare(entries[root], entries[largest], pool, pool_size) >= 0) {
            return;
        }
        const DirectoryEntryIndex tmp = entries[root];
        entries[root] = entries[largest];
        entries[largest] = tmp;
        root = largest;
    }
}

static void sort_directory_entries(
    DirectoryEntryIndex *entries, size_t count, const char *pool, size_t pool_size)
{
    if (entries == nullptr || count < 2U || pool == nullptr) return;
    for (size_t start = count / 2U; start > 0; --start) {
        directory_heap_sift_down(entries, count, start - 1U, pool, pool_size);
    }
    for (size_t end = count; end > 1U; --end) {
        const DirectoryEntryIndex tmp = entries[0];
        entries[0] = entries[end - 1U];
        entries[end - 1U] = tmp;
        directory_heap_sift_down(entries, end - 1U, 0U, pool, pool_size);
    }
}

static size_t trim_incomplete_utf8_tail(const uint8_t *data, size_t size)
{
    if (data == nullptr || size == 0) return 0;
    size_t start = size;
    while (start > 0 && (data[start - 1] & 0xC0U) == 0x80U) {
        --start;
    }
    if (start == size) {
        const uint8_t last = data[size - 1];
        if (last < 0x80U) return size;
        // 末尾正好是多字节序列起始字节，还没有 continuation。
        return size - 1;
    }
    if (start == 0) return 0;

    const uint8_t first = data[start - 1];
    size_t expected = 1;
    if ((first & 0xE0U) == 0xC0U) expected = 2;
    else if ((first & 0xF0U) == 0xE0U) expected = 3;
    else if ((first & 0xF8U) == 0xF0U) expected = 4;
    else if (first < 0x80U) return size;
    else return start - 1;

    const size_t available = size - (start - 1);
    return available < expected ? start - 1 : size;
}

enum class Utf8DecodeResult : uint8_t
{
    Ok = 0,
    Incomplete,
    Invalid,
};

static Utf8DecodeResult decode_utf8_one(
    const uint8_t *data,
    size_t size,
    size_t offset,
    uint32_t *out_codepoint,
    size_t *out_bytes)
{
    if (data == nullptr || out_codepoint == nullptr || out_bytes == nullptr || offset >= size) {
        return Utf8DecodeResult::Invalid;
    }
    const uint8_t first = data[offset];
    if (first < 0x80U) {
        *out_codepoint = first;
        *out_bytes = 1;
        return Utf8DecodeResult::Ok;
    }

    size_t need = 0;
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    if ((first & 0xE0U) == 0xC0U) {
        need = 1; codepoint = first & 0x1FU; minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
        need = 2; codepoint = first & 0x0FU; minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
        need = 3; codepoint = first & 0x07U; minimum = 0x10000U;
    } else {
        return Utf8DecodeResult::Invalid;
    }
    if (offset + need >= size) return Utf8DecodeResult::Incomplete;
    for (size_t n = 0; n < need; ++n) {
        const uint8_t next = data[offset + n + 1U];
        if ((next & 0xC0U) != 0x80U) return Utf8DecodeResult::Invalid;
        codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if (codepoint < minimum || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        return Utf8DecodeResult::Invalid;
    }
    *out_codepoint = codepoint;
    *out_bytes = need + 1U;
    return Utf8DecodeResult::Ok;
}


struct PhysicalLine
{
    size_t start = 0;
    size_t end = 0; // 不含 CR/LF
    size_t newline_bytes = 0;
    bool complete = false;
};

struct LineMetrics
{
    size_t codepoints = 0;
    uint32_t first = 0;
    uint32_t last = 0;
    bool valid_utf8 = true;
};

static PhysicalLine physical_line_at(const uint8_t *data, size_t size, size_t start)
{
    PhysicalLine line = {};
    line.start = start;
    line.end = start;
    while (line.end < size && data[line.end] != '\r' && data[line.end] != '\n') {
        ++line.end;
    }
    if (line.end >= size) return line;
    line.complete = true;
    if (data[line.end] == '\r' && line.end + 1U < size && data[line.end + 1U] == '\n') {
        line.newline_bytes = 2;
    } else {
        line.newline_bytes = 1;
    }
    return line;
}

static bool line_starts_with(const uint8_t *data, const PhysicalLine &line, const char *literal)
{
    if (data == nullptr || literal == nullptr || line.end < line.start) return false;
    const size_t literal_len = strlen(literal);
    const size_t line_len = line.end - line.start;
    return line_len >= literal_len && memcmp(data + line.start, literal, literal_len) == 0;
}

static bool line_all_question_marks(const uint8_t *data, const PhysicalLine &line)
{
    if (line.end <= line.start) return false;
    for (size_t i = line.start; i < line.end; ++i) {
        if (data[i] != '?') return false;
    }
    return true;
}

static bool line_is_web_noise(const uint8_t *data, const PhysicalLine &line)
{
    return line_starts_with(data, line, "飞卢小说网") ||
        line_starts_with(data, line, "公告：") ||
        line_starts_with(data, line, "好消息：") ||
        line_starts_with(data, line, "多种充值渠道:") ||
        line_starts_with(data, line, "多种充值渠道：");
}

static bool is_closing_punctuation(uint32_t cp)
{
    switch (cp) {
        case 0x201DU: // ”
        case 0x2019U: // ’
        case 0x300DU: // 」
        case 0x300FU: // 』
        case 0x3009U: // 〉
        case 0x300BU: // 》
        case 0x3011U: // 】
        case 0x3015U: // 〕
        case 0xFF09U: // ）
        case ')': case ']': case '}': case '"': case '\'':
            return true;
        default:
            return false;
    }
}

static bool is_sentence_terminal(uint32_t cp)
{
    switch (cp) {
        case 0x3002U: // 。
        case 0xFF01U: // ！
        case 0xFF1FU: // ？
        case 0xFF1BU: // ；
        case 0xFF1AU: // ：
        case 0x2026U: // …
        case '.': case '!': case '?': case ';': case ':':
            return true;
        default:
            return false;
    }
}

static bool is_comma_like(uint32_t cp)
{
    return cp == 0xFF0CU || cp == 0x3001U || cp == ',';
}

static bool is_dialogue_lead(uint32_t cp)
{
    return cp == 0x201CU || cp == 0x300CU || cp == 0x300EU ||
        cp == 0x2014U || cp == '"';
}

static LineMetrics line_metrics(const uint8_t *data, const PhysicalLine &line)
{
    LineMetrics metrics = {};
    uint32_t semantic[4] = {};
    size_t semantic_count = 0;
    size_t cursor = line.start;
    while (cursor < line.end) {
        uint32_t cp = 0;
        size_t bytes = 0;
        const Utf8DecodeResult ret = decode_utf8_one(data, line.end, cursor, &cp, &bytes);
        if (ret != Utf8DecodeResult::Ok) {
            metrics.valid_utf8 = false;
            return metrics;
        }
        cursor += bytes;
        cp = TextEncoding::normalize_plain_text_codepoint(cp);
        if (cp == 0U || cp == '\t' || cp == ' ') continue;
        if (metrics.first == 0) metrics.first = cp;
        ++metrics.codepoints;
        if (semantic_count < 4) {
            semantic[semantic_count++] = cp;
        } else {
            semantic[0] = semantic[1];
            semantic[1] = semantic[2];
            semantic[2] = semantic[3];
            semantic[3] = cp;
        }
    }
    if (semantic_count == 0) return metrics;
    size_t idx = semantic_count;
    while (idx > 0 && is_closing_punctuation(semantic[idx - 1U])) --idx;
    metrics.last = idx > 0 ? semantic[idx - 1U] : semantic[semantic_count - 1U];
    return metrics;
}

static bool line_is_chapter_title(const uint8_t *data, const PhysicalLine &line, const LineMetrics &metrics)
{
    if (metrics.codepoints == 0 || metrics.codepoints > 24) return false;
    if (line_starts_with(data, line, "檞寄生 ")) return true;
    if (metrics.first >= '0' && metrics.first <= '9') {
        size_t cursor = line.start;
        uint8_t digits = 0;
        while (cursor < line.end && data[cursor] >= '0' && data[cursor] <= '9' && digits < 3U) {
            ++cursor;
            ++digits;
        }
        uint32_t cp = 0;
        size_t bytes = 0;
        if (digits > 0 && cursor < line.end &&
            decode_utf8_one(data, line.end, cursor, &cp, &bytes) == Utf8DecodeResult::Ok &&
            (cp == 0x3001U || cp == '.' || cp == 0xFF0EU)) {
            return true;
        }
    }
    return false;
}

static bool should_hide_line(
    const uint8_t *data, const PhysicalLine &line, bool previous_noise, bool previous_title)
{
    if (line_is_web_noise(data, line)) return true;
    if (previous_noise && line_starts_with(data, line, "檞寄生 ")) return true;
    return (previous_noise || previous_title) && line_all_question_marks(data, line);
}

static bool should_soft_join(
    const uint8_t *data,
    const PhysicalLine &line,
    const PhysicalLine *next_line,
    bool line_started_midway)
{
    const LineMetrics current = line_metrics(data, line);
    if (!current.valid_utf8 || current.codepoints == 0) return false;
    if (line_is_chapter_title(data, line, current)) return false;
    if (is_sentence_terminal(current.last)) return false;

    if (next_line != nullptr) {
        const LineMetrics next = line_metrics(data, *next_line);
        if (!next.valid_utf8 || next.codepoints == 0) return false;
        if (line_is_chapter_title(data, *next_line, next)) return false;
        if (line_is_web_noise(data, *next_line) || is_dialogue_lead(next.first)) return false;
    }

    // 中文旧网页 TXT 常在固定列宽处直接插入 CRLF；逗号结尾尤其高置信度。
    if (is_comma_like(current.last) && (current.codepoints >= 12U || line_started_midway)) {
        return true;
    }
    // 短对白、诗句、效果字优先原样保留；较长且句意未结束的行才重排。
    if (current.codepoints >= 20U) return true;
    return line_started_midway && current.codepoints >= 4U;
}

static bool remaining_is_noise_only(const uint8_t *data, size_t size, size_t start)
{
    bool previous_noise = false;
    size_t cursor = start;
    while (cursor < size) {
        const PhysicalLine line = physical_line_at(data, size, cursor);
        const LineMetrics metrics = line_metrics(data, line);
        if (!metrics.valid_utf8) return false;
        const bool empty = metrics.codepoints == 0;
        const bool hidden = should_hide_line(data, line, previous_noise, false);
        if (hidden) {
            previous_noise = true;
        } else if (empty) {
            // 文件末尾空行可直接略过。
        } else {
            return false;
        }
        cursor = line.end + (line.complete ? line.newline_bytes : 0U);
        if (!line.complete) break;
    }
    return cursor >= size;
}

static size_t previous_utf8_start(const char *text, size_t end)
{
    if (text == nullptr || end == 0) return 0;
    size_t pos = end - 1U;
    while (pos > 0 && (static_cast<uint8_t>(text[pos]) & 0xC0U) == 0x80U) --pos;
    return pos;
}

static void rebalance_single_glyph_tail(
    char *page_text,
    size_t write,
    size_t auto_wrap_index,
    uint8_t previous_line_glyphs,
    uint8_t current_line_glyphs)
{
    if (page_text == nullptr || auto_wrap_index == SIZE_MAX ||
        previous_line_glyphs <= 1U || current_line_glyphs != 1U || auto_wrap_index == 0) {
        return;
    }
    const size_t glyph_start = previous_utf8_start(page_text, auto_wrap_index);
    const size_t glyph_bytes = auto_wrap_index - glyph_start;
    if (glyph_bytes == 0 || glyph_bytes > 4U || auto_wrap_index + 1U > write) return;
    char glyph[4] = {};
    memcpy(glyph, page_text + glyph_start, glyph_bytes);
    page_text[glyph_start] = '\n';
    memcpy(page_text + glyph_start + 1U, glyph, glyph_bytes);
}

} // namespace

bool is_txt_name(const char *name)
{
    if (name == nullptr) return false;
    const size_t len = strlen(name);
    return len >= 4 && name[len - 4] == '.' &&
        ascii_tolower(static_cast<unsigned char>(name[len - 3])) == 't' &&
        ascii_tolower(static_cast<unsigned char>(name[len - 2])) == 'x' &&
        ascii_tolower(static_cast<unsigned char>(name[len - 1])) == 't';
}

bool path_is_inside_root(const char *path)
{
    if (path == nullptr) return false;
    const size_t root_len = strlen(kRootDirectory);
    return strncmp(path, kRootDirectory, root_len) == 0 &&
        (path[root_len] == '\0' || path[root_len] == '/');
}

esp_err_t join_child_path(const char *directory, const char *name, char *out, size_t out_size)
{
    if (directory == nullptr || name == nullptr || out == nullptr || out_size == 0 ||
        !path_is_inside_root(directory) || name_is_hidden(name) || strchr(name, '/') != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(out, out_size, "%s/%s", directory, name);
    if (written <= 0 || static_cast<size_t>(written) >= out_size) {
        if (out_size > 0) out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t parent_path(const char *path, char *out, size_t out_size)
{
    if (path == nullptr || out == nullptr || out_size == 0 || !path_is_inside_root(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(path, kRootDirectory) == 0) {
        if (strlen(kRootDirectory) + 1 > out_size) return ESP_ERR_INVALID_SIZE;
        strcpy(out, kRootDirectory);
        return ESP_OK;
    }
    const char *slash = strrchr(path, '/');
    if (slash == nullptr || slash <= path + strlen(kRootDirectory) - 1) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t len = static_cast<size_t>(slash - path);
    if (len + 1 > out_size) return ESP_ERR_INVALID_SIZE;
    memcpy(out, path, len);
    out[len] = '\0';
    return path_is_inside_root(out) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t begin_directory_scan(const char *path, DirectoryScanSession *session)
{
    if (path == nullptr || session == nullptr || !path_is_inside_root(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    cancel_directory_scan(session);

    DIR *dir = nullptr;
    {
        StorageSdLockGuard guard(kStorageLockTimeout);
        if (!guard) return ESP_ERR_TIMEOUT;
        dir = opendir(path);
    }
    if (dir == nullptr) return ESP_ERR_NOT_FOUND;

    char *path_copy = static_cast<char *>(heap_caps_calloc(
        kPathBytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    char *scratch = static_cast<char *>(heap_caps_malloc(
        kEntryNameBytes + kPathBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (path_copy == nullptr || scratch == nullptr) {
        if (path_copy != nullptr) heap_caps_free(path_copy);
        if (scratch != nullptr) heap_caps_free(scratch);
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    session->dir = dir;
    session->path = path_copy;
    session->scratch = scratch;
    snprintf(session->path, kPathBytes, "%s", path);

    if (!reserve_directory_entries(session, kInitialDirectoryEntryCapacity) ||
        !reserve_directory_pool(session, kInitialDirectoryPoolCapacity)) {
        cancel_directory_scan(session);
        return ESP_ERR_NO_MEM;
    }
    // StringPool offset 0 保留为空串，与 Music Catalog 的 offset 语义一致。
    session->string_pool[0] = '\0';
    session->pool_size = 1U;
    return ESP_OK;
}

esp_err_t scan_directory_step(
    DirectoryScanSession *session, size_t max_raw_entries, bool *out_done)
{
    if (session == nullptr || out_done == nullptr || session->dir == nullptr ||
        session->entries == nullptr || session->string_pool == nullptr ||
        session->path == nullptr || session->scratch == nullptr ||
        max_raw_entries == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_done = false;
    DIR *dir = static_cast<DIR *>(session->dir);
    char *name = session->scratch;
    char *full_path = session->scratch + kEntryNameBytes;

    for (size_t processed = 0; processed < max_raw_entries; ++processed) {
        name[0] = '\0';
        bool end = false;
        {
            StorageSdLockGuard guard(kStorageLockTimeout);
            if (!guard) return ESP_ERR_TIMEOUT;
            struct dirent *entry = readdir(dir);
            if (entry == nullptr) {
                end = true;
            } else {
                snprintf(name, kEntryNameBytes, "%s", entry->d_name);
            }
        }
        if (end) {
            *out_done = true;
            return ESP_OK;
        }
        if (name_is_hidden(name)) {
            taskYIELD();
            continue;
        }

        full_path[0] = '\0';
        if (join_child_path(session->path, name, full_path, kPathBytes) != ESP_OK) {
            taskYIELD();
            continue;
        }

        struct stat info = {};
        {
            StorageSdLockGuard guard(kStorageLockTimeout);
            if (!guard) return ESP_ERR_TIMEOUT;
            if (stat(full_path, &info) != 0) {
                taskYIELD();
                continue;
            }
        }

        const bool is_dir = S_ISDIR(info.st_mode);
        if (!is_dir && (!S_ISREG(info.st_mode) || !is_txt_name(name))) {
            taskYIELD();
            continue;
        }

        const uint64_t size_bytes = is_dir
            ? 0U
            : static_cast<uint64_t>(info.st_size < 0 ? 0 : info.st_size);
        const esp_err_t append_ret = append_directory_entry(
            session, name, is_dir, size_bytes);
        if (append_ret != ESP_OK) return append_ret;
        taskYIELD();
    }
    return ESP_OK;
}

esp_err_t finish_directory_scan(
    DirectoryScanSession *session, DirectorySnapshot *out_snapshot)
{
    if (session == nullptr || out_snapshot == nullptr || session->dir == nullptr ||
        session->entries == nullptr || session->string_pool == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (!guard) return ESP_ERR_TIMEOUT;
        closedir(static_cast<DIR *>(session->dir));
    }
    session->dir = nullptr;

    sort_directory_entries(
        session->entries, session->count, session->string_pool, session->pool_size);

    release_directory(out_snapshot);
    out_snapshot->entries = session->entries;
    out_snapshot->string_pool = session->string_pool;
    out_snapshot->count = session->count;
    out_snapshot->pool_size = session->pool_size;

    session->entries = nullptr;
    session->string_pool = nullptr;
    if (session->path != nullptr) heap_caps_free(session->path);
    if (session->scratch != nullptr) heap_caps_free(session->scratch);
    session->path = nullptr;
    session->scratch = nullptr;
    session->count = 0;
    session->entry_capacity = 0;
    session->pool_size = 0;
    session->pool_capacity = 0;
    return ESP_OK;
}

void cancel_directory_scan(DirectoryScanSession *session)
{
    if (session == nullptr) return;
    if (session->dir != nullptr) {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) {
            closedir(static_cast<DIR *>(session->dir));
            session->dir = nullptr;
        }
    }
    if (session->entries != nullptr) heap_caps_free(session->entries);
    if (session->string_pool != nullptr) heap_caps_free(session->string_pool);
    if (session->path != nullptr) heap_caps_free(session->path);
    if (session->scratch != nullptr) heap_caps_free(session->scratch);
    *session = {};
}

esp_err_t scan_directory(const char *path, DirectorySnapshot *out_snapshot)
{
    if (path == nullptr || out_snapshot == nullptr || !path_is_inside_root(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    DirectoryScanSession session = {};
    esp_err_t ret = begin_directory_scan(path, &session);
    if (ret != ESP_OK) return ret;

    bool done = false;
    while (!done) {
        ret = scan_directory_step(&session, 8U, &done);
        if (ret != ESP_OK) {
            cancel_directory_scan(&session);
            return ret;
        }
    }
    ret = finish_directory_scan(&session, out_snapshot);
    if (ret != ESP_OK) cancel_directory_scan(&session);
    return ret;
}

void release_directory(DirectorySnapshot *snapshot)
{
    if (snapshot == nullptr) return;
    if (snapshot->entries != nullptr) heap_caps_free(snapshot->entries);
    if (snapshot->string_pool != nullptr) heap_caps_free(snapshot->string_pool);
    *snapshot = {};
}

const DirectoryEntryIndex *directory_entry_at(
    const DirectorySnapshot *snapshot, size_t index)
{
    if (snapshot == nullptr || snapshot->entries == nullptr ||
        snapshot->string_pool == nullptr || index >= snapshot->count) {
        return nullptr;
    }
    const DirectoryEntryIndex *entry = &snapshot->entries[index];
    return directory_pool_string(snapshot->string_pool, snapshot->pool_size, entry->name_off) != nullptr
        ? entry
        : nullptr;
}

const char *directory_entry_name(const DirectorySnapshot *snapshot, size_t index)
{
    const DirectoryEntryIndex *entry = directory_entry_at(snapshot, index);
    if (entry == nullptr) return nullptr;
    return directory_pool_string(snapshot->string_pool, snapshot->pool_size, entry->name_off);
}

bool directory_entry_is_directory(const DirectoryEntryIndex *entry)
{
    return entry != nullptr && directory_index_is_directory(*entry);
}

static esp_err_t read_page_window(
    FILE *file,
    uint64_t file_size,
    uint64_t offset,
    TextEncoding::Encoding encoding,
    uint8_t *raw,
    size_t *out_loaded,
    bool *out_line_started_midway)
{
    if (file == nullptr || raw == nullptr || out_loaded == nullptr ||
        out_line_started_midway == nullptr || offset >= file_size ||
        offset > static_cast<uint64_t>(LONG_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((encoding == TextEncoding::Encoding::Utf16Le ||
            encoding == TextEncoding::Encoding::Utf16Be) &&
        (offset & 1ULL) != 0ULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_loaded = 0;
    *out_line_started_midway = offset > 0;

    {
        StorageSdLockGuard guard(kStorageLockTimeout);
        if (!guard) return ESP_ERR_TIMEOUT;
        clearerr(file);

        if (offset > 0) {
            if (encoding == TextEncoding::Encoding::Utf16Le ||
                encoding == TextEncoding::Encoding::Utf16Be) {
                if (offset >= 2U && offset - 2U <= static_cast<uint64_t>(LONG_MAX) &&
                    fseek(file, static_cast<long>(offset - 2U), SEEK_SET) == 0) {
                    uint8_t previous[2] = {};
                    if (fread(previous, 1U, sizeof(previous), file) == sizeof(previous)) {
                        const bool little = encoding == TextEncoding::Encoding::Utf16Le;
                        const uint16_t value = little
                            ? static_cast<uint16_t>(previous[0] |
                                (static_cast<uint16_t>(previous[1]) << 8U))
                            : static_cast<uint16_t>(
                                (static_cast<uint16_t>(previous[0]) << 8U) | previous[1]);
                        *out_line_started_midway = value != '\r' && value != '\n';
                    }
                }
            } else {
                if (fseek(file, static_cast<long>(offset - 1U), SEEK_SET) == 0) {
                    uint8_t previous = 0U;
                    if (fread(&previous, 1U, 1U, file) == 1U) {
                        // UTF-8 与 GBK 的换行都是单字节 ASCII；GBK trail 不会落到 CR/LF。
                        *out_line_started_midway = previous != '\r' && previous != '\n';
                    }
                }
            }
            clearerr(file);
        }
        if (fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
            return ESP_FAIL;
        }
    }

    const size_t wanted = static_cast<size_t>(
        (file_size - offset) > kPageReadBytes ? kPageReadBytes : (file_size - offset));
    while (*out_loaded < wanted) {
        const size_t chunk = (wanted - *out_loaded) > kReadChunkBytes
            ? kReadChunkBytes
            : (wanted - *out_loaded);
        size_t got = 0;
        {
            StorageSdLockGuard guard(kStorageLockTimeout);
            if (!guard) return ESP_ERR_TIMEOUT;
            got = fread(raw + *out_loaded, 1, chunk, file);
        }
        if (got == 0) {
            if (feof(file)) break;
            return ESP_FAIL;
        }
        *out_loaded += got;
        taskYIELD();
    }
    return *out_loaded > 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t transcode_page_window(
    const uint8_t *raw,
    size_t loaded,
    uint64_t offset,
    uint64_t file_size,
    TextEncoding::Encoding encoding,
    uint8_t *decoded,
    size_t decoded_capacity,
    uint16_t *source_map,
    size_t source_map_entries,
    size_t *out_decoded_size)
{
    if (raw == nullptr || loaded == 0U || decoded == nullptr || source_map == nullptr ||
        out_decoded_size == nullptr || decoded_capacity == 0U || source_map_entries == 0U ||
        encoding == TextEncoding::Encoding::Utf8) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t source = 0U;
    if (offset == 0U &&
        (encoding == TextEncoding::Encoding::Utf16Le ||
         encoding == TextEncoding::Encoding::Utf16Be)) {
        if (loaded < 2U) return ESP_ERR_INVALID_SIZE;
        const bool expected_bom = encoding == TextEncoding::Encoding::Utf16Le
            ? (raw[0] == 0xFFU && raw[1] == 0xFEU)
            : (raw[0] == 0xFEU && raw[1] == 0xFFU);
        if (!expected_bom) return ESP_ERR_INVALID_RESPONSE;
        source = 2U;
    }

    size_t write = 0U;
    source_map[0] = static_cast<uint16_t>(source);
    while (source < loaded) {
        uint32_t codepoint = 0U;
        size_t source_bytes = 0U;
        const TextEncoding::DecodeResult decode = TextEncoding::decode_one(
            encoding, raw, loaded, source, &codepoint, &source_bytes);
        if (decode == TextEncoding::DecodeResult::Incomplete) {
            if (offset + loaded >= file_size) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            break;
        }
        if (decode != TextEncoding::DecodeResult::Ok || source_bytes == 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        char utf8[4] = {};
        const size_t utf8_bytes = TextEncoding::encode_utf8(codepoint, utf8);
        if (utf8_bytes == 0U || write + utf8_bytes > decoded_capacity ||
            write + utf8_bytes + 1U > source_map_entries) {
            return ESP_ERR_INVALID_SIZE;
        }

        const uint16_t source_before = static_cast<uint16_t>(source);
        source += source_bytes;
        for (size_t i = 0U; i < utf8_bytes; ++i) {
            decoded[write + i] = static_cast<uint8_t>(utf8[i]);
            source_map[write + i] = source_before;
        }
        write += utf8_bytes;
        source_map[write] = static_cast<uint16_t>(source);
    }

    if (write == 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_decoded_size = write;
    return ESP_OK;
}

static esp_err_t paginate_loaded_page(
    const uint8_t *raw,
    size_t loaded,
    char *page_text,
    uint64_t offset,
    uint64_t file_size,
    bool line_started_midway,
    const uint16_t *source_map,
    const PageLayout &layout,
    TextPage *out_page)
{
    if (raw == nullptr || loaded == 0 || page_text == nullptr || out_page == nullptr ||
        layout.text_width_px == 0 || layout.max_lines == 0 || layout.glyph_width == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_page = {};

    size_t source = 0;
    size_t valid_size = loaded;
    if (source_map == nullptr) {
        if (offset == 0) {
            line_started_midway = false;
            if (loaded >= 2 && ((raw[0] == 0xFFU && raw[1] == 0xFEU) ||
                                (raw[0] == 0xFEU && raw[1] == 0xFFU))) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (loaded >= 3 && raw[0] == 0xEFU && raw[1] == 0xBBU && raw[2] == 0xBFU) {
                source = 3;
            }
        }
        valid_size = source + trim_incomplete_utf8_tail(raw + source, loaded - source);
    }
    if (valid_size <= source) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const auto source_file_offset = [offset, source_map](size_t decoded_offset) -> uint64_t {
        return offset + (source_map != nullptr
            ? static_cast<uint64_t>(source_map[decoded_offset])
            : static_cast<uint64_t>(decoded_offset));
    };
    const size_t page_source_start = source;
    size_t write = 0;
    uint8_t visual_line = 0;
    uint8_t consecutive_hard_breaks = 0; // 最多保留两个显示换行，也就是一行空行
    uint16_t line_width_px = 0;
    uint8_t current_line_glyphs = 0;
    uint8_t previous_visual_line_glyphs = 0;
    size_t last_auto_wrap_index = SIZE_MAX;
    bool previous_noise = false;
    bool previous_title = false;
    bool page_full = false;

    while (source < valid_size && visual_line < layout.max_lines && !page_full) {
        const PhysicalLine physical = physical_line_at(raw, valid_size, source);
        PhysicalLine next = {};
        const PhysicalLine *next_ptr = nullptr;
        if (physical.complete) {
            const size_t next_start = physical.end + physical.newline_bytes;
            if (next_start < valid_size) {
                next = physical_line_at(raw, valid_size, next_start);
                next_ptr = &next;
            }
        }

        const LineMetrics physical_metrics = line_metrics(raw, physical);
        if (!physical_metrics.valid_utf8) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const bool current_is_title = line_is_chapter_title(raw, physical, physical_metrics);
        const bool hide = should_hide_line(raw, physical, previous_noise, previous_title);
        if (hide) {
            previous_noise = true;
            previous_title = false;
            source = physical.end + (physical.complete ? physical.newline_bytes : 0U);
            line_started_midway = false;
            if (!physical.complete) break;
            continue;
        }

        // 章节/小节标题尽量从新页顶部开始；不消费源字节，下一页仍从标题精确开始。
        if (current_is_title && write > 0) {
            break;
        }
        previous_noise = false;
        previous_title = current_is_title;

        // 空物理行按“整段 run”处理：正常段落结尾已经贡献第一个换行，
        // 连续空行只再贡献一个换行，因此视觉上最多保留一行空行。
        // run 内其余 CR/LF 仍一次性消费，避免页尾截断后在下一页重新冒出来。
        if (physical_metrics.codepoints == 0) {
            source = physical.end;
            if (!physical.complete) break;
            source += physical.newline_bytes;
            while (source < valid_size) {
                const PhysicalLine blank = physical_line_at(raw, valid_size, source);
                const LineMetrics blank_metrics = line_metrics(raw, blank);
                if (!blank_metrics.valid_utf8) {
                    return ESP_ERR_INVALID_RESPONSE;
                }
                if (blank_metrics.codepoints != 0) break;
                source = blank.end;
                if (!blank.complete) break;
                source += blank.newline_bytes;
            }
            line_started_midway = false;
            line_width_px = 0;
            current_line_glyphs = 0;
            if (consecutive_hard_breaks < 2U) {
                ++consecutive_hard_breaks;
                if (visual_line + 1U >= layout.max_lines) break;
                page_text[write++] = '\n';
                ++visual_line;
            }
            continue;
        }

        size_t cursor = physical.start;
        while (cursor < physical.end) {
            uint32_t codepoint = 0;
            size_t codepoint_bytes = 0;
            const Utf8DecodeResult decode = decode_utf8_one(
                raw, physical.end, cursor, &codepoint, &codepoint_bytes);
            if (decode == Utf8DecodeResult::Incomplete) {
                page_full = true;
                break;
            }
            if (decode != Utf8DecodeResult::Ok) {
                return ESP_ERR_INVALID_RESPONSE;
            }

            const uint32_t source_codepoint = codepoint;
            codepoint = TextEncoding::normalize_plain_text_codepoint(codepoint);
            if (codepoint == 0U || (codepoint < 0x20U && codepoint != '\t')) {
                cursor += codepoint_bytes;
                source = cursor;
                continue;
            }

            uint32_t next_codepoint = 0;
            if (cursor + codepoint_bytes < physical.end) {
                size_t next_bytes = 0;
                if (decode_utf8_one(raw, physical.end, cursor + codepoint_bytes,
                        &next_codepoint, &next_bytes) != Utf8DecodeResult::Ok) {
                    next_codepoint = 0;
                } else {
                    next_codepoint = TextEncoding::normalize_plain_text_codepoint(next_codepoint);
                }
            }

            uint16_t glyph_width_px = 0;
            if (codepoint == '\t') {
                const uint16_t space_width = layout.glyph_width(' ', ' ');
                glyph_width_px = static_cast<uint16_t>((space_width > 0 ? space_width : 8U) * 4U);
            } else {
                glyph_width_px = layout.glyph_width(codepoint, next_codepoint);
                if (glyph_width_px == 0) glyph_width_px = codepoint < 0x80U ? 12U : 24U;
            }

            // 分页器必须与 LVGL label 的实际换行边界一致。
            // 之前允许中文闭合标点/句末标点/逗号在行尾“悬挂”越过 text_width_px，
            // 但 LV_LABEL_LONG_WRAP 不会接受这个越界：LVGL 会把该字符挪到下一视觉行，
            // 导致分页器认为本页仍只有 N 行，而屏幕实际出现第 N+1 行（常被底部裁成半行）。
            // next_offset 又已经消费了那一行的源字节，翻页后就表现为“半行丢失”。
            // 所以任何字形只要超出正文宽度，都先由分页器显式换行。
            const bool would_overflow = line_width_px > 0 &&
                static_cast<uint32_t>(line_width_px) + glyph_width_px > layout.text_width_px;
            if (would_overflow) {
                if (visual_line + 1U >= layout.max_lines) {
                    page_full = true;
                    break;
                }
                last_auto_wrap_index = write;
                previous_visual_line_glyphs = current_line_glyphs;
                page_text[write++] = '\n';
                ++visual_line;
                line_width_px = 0;
                current_line_glyphs = 0;
            }

            if (codepoint == '\t') {
                for (uint8_t n = 0; n < 4U; ++n) page_text[write++] = ' ';
            } else if (codepoint != source_codepoint) {
                // U+00A0/U+3000 统一显示为普通空格，避免字体缺字显示方框。
                page_text[write++] = ' ';
            } else {
                memcpy(page_text + write, raw + cursor, codepoint_bytes);
                write += codepoint_bytes;
            }
            cursor += codepoint_bytes;
            source = cursor;
            if (current_line_glyphs < UINT8_MAX) ++current_line_glyphs;
            const uint32_t next_width = static_cast<uint32_t>(line_width_px) + glyph_width_px;
            line_width_px = static_cast<uint16_t>(next_width > UINT16_MAX ? UINT16_MAX : next_width);
        }
        if (page_full || source < physical.end) break;

        source = physical.end;
        if (!physical.complete) break;

        const bool soft_join = should_soft_join(
            raw, physical, next_ptr, line_started_midway);
        source += physical.newline_bytes; // 无论显示方式如何，源 CR/LF 都必须被精确消费。
        line_started_midway = false;
        if (soft_join) {
            consecutive_hard_breaks = 0;
            continue;
        }

        consecutive_hard_breaks = 1U;

        rebalance_single_glyph_tail(
            page_text, write, last_auto_wrap_index,
            previous_visual_line_glyphs, current_line_glyphs);
        last_auto_wrap_index = SIZE_MAX;
        previous_visual_line_glyphs = 0;
        if (visual_line + 1U >= layout.max_lines) break;
        page_text[write++] = '\n';
        ++visual_line;
        line_width_px = 0;
        current_line_glyphs = 0;
    }

    if (source <= page_source_start) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 若本页之后直到 EOF 只剩明确网站广告/重复抓取头，则直接把源 offset 吃到文件尾，
    // 避免用户再翻到一张“只有被过滤内容”的空白末页。
    if (source < valid_size && source_file_offset(valid_size) >= file_size &&
        remaining_is_noise_only(raw, valid_size, source)) {
        source = valid_size;
    }

    page_text[write] = '\0';
    out_page->text = page_text;
    out_page->size = write;
    out_page->start_offset = offset;
    out_page->next_offset = source_file_offset(source);
    out_page->file_size = file_size;
    out_page->at_start = offset == 0;
    out_page->at_end = out_page->next_offset >= file_size;
    return ESP_OK;
}

static size_t page_text_capacity(TextEncoding::Encoding encoding)
{
    const size_t input_bytes = encoding == TextEncoding::Encoding::Utf8
        ? kPageReadBytes
        : kDecodedPageBytes;
    return input_bytes * 2U + 64U;
}

esp_err_t detect_text_encoding(const char *path, TextEncoding::Encoding *out_encoding)
{
    if (path == nullptr || out_encoding == nullptr || !path_is_inside_root(path) || !is_txt_name(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat info = {};
    FILE *file = nullptr;
    {
        StorageSdLockGuard guard(kStorageLockTimeout);
        if (!guard) return ESP_ERR_TIMEOUT;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return ESP_ERR_NOT_FOUND;
        if (info.st_size <= 0) return ESP_ERR_INVALID_SIZE;
        file = fopen(path, "rb");
    }
    if (file == nullptr) return ESP_ERR_NOT_FOUND;

    const size_t probe_size = static_cast<uint64_t>(info.st_size) > kEncodingProbeBytes
        ? kEncodingProbeBytes
        : static_cast<size_t>(info.st_size);
    uint8_t *probe = static_cast<uint8_t *>(heap_caps_malloc(
        probe_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (probe == nullptr) {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t loaded = 0U;
    esp_err_t result = ESP_OK;
    while (loaded < probe_size) {
        const size_t wanted = (probe_size - loaded) > kReadChunkBytes
            ? kReadChunkBytes
            : (probe_size - loaded);
        size_t got = 0U;
        {
            StorageSdLockGuard guard(kStorageLockTimeout);
            if (!guard) {
                result = ESP_ERR_TIMEOUT;
                break;
            }
            got = fread(probe + loaded, 1U, wanted, file);
        }
        if (got == 0U) {
            result = feof(file) ? ESP_OK : ESP_FAIL;
            break;
        }
        loaded += got;
        taskYIELD();
    }
    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) fclose(file);
    }

    if (result == ESP_OK && loaded > 0U) {
        TextEncoding::Detection detected = {};
        const bool probe_is_prefix = static_cast<uint64_t>(loaded) <
            static_cast<uint64_t>(info.st_size);
        result = TextEncoding::detect(probe, loaded, &detected, probe_is_prefix);
        if (result == ESP_OK) {
            *out_encoding = detected.encoding;
        }
    } else if (result == ESP_OK) {
        result = ESP_ERR_INVALID_SIZE;
    }
    heap_caps_free(probe);
    return result;
}

static esp_err_t load_text_page_internal(
    const char *path,
    uint64_t offset,
    TextEncoding::Encoding encoding,
    const PageLayout &layout,
    TextPage *out_page)
{
    if (path == nullptr || out_page == nullptr || !path_is_inside_root(path) || !is_txt_name(path) ||
        layout.text_width_px == 0 || layout.max_lines == 0 || layout.glyph_width == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    release_text_page(out_page);

    struct stat info = {};
    {
        StorageSdLockGuard guard(kStorageLockTimeout);
        if (!guard) return ESP_ERR_TIMEOUT;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return ESP_ERR_NOT_FOUND;
    }
    if (info.st_size <= 0) return ESP_ERR_INVALID_SIZE;

    const uint64_t file_size = static_cast<uint64_t>(info.st_size);
    if (offset >= file_size || offset > static_cast<uint64_t>(LONG_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = nullptr;
    {
        StorageSdLockGuard guard(kStorageLockTimeout);
        if (!guard) return ESP_ERR_TIMEOUT;
        file = fopen(path, "rb");
    }
    if (file == nullptr) return ESP_ERR_NOT_FOUND;

    uint8_t *raw = static_cast<uint8_t *>(heap_caps_malloc(
        kPageReadBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t *decoded = nullptr;
    uint16_t *source_map = nullptr;
    if (encoding != TextEncoding::Encoding::Utf8) {
        decoded = static_cast<uint8_t *>(heap_caps_malloc(
            kDecodedPageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        source_map = static_cast<uint16_t *>(heap_caps_malloc(
            kDecodedSourceMapEntries * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    char *page_text = static_cast<char *>(heap_caps_malloc(
        page_text_capacity(encoding), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (raw == nullptr || page_text == nullptr ||
        (encoding != TextEncoding::Encoding::Utf8 && (decoded == nullptr || source_map == nullptr))) {
        heap_caps_free(raw);
        heap_caps_free(decoded);
        heap_caps_free(source_map);
        heap_caps_free(page_text);
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t loaded = 0U;
    bool line_started_midway = false;
    esp_err_t result = read_page_window(
        file, file_size, offset, encoding, raw, &loaded, &line_started_midway);
    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) fclose(file);
    }

    if (result == ESP_OK && encoding == TextEncoding::Encoding::Utf8) {
        result = paginate_loaded_page(
            raw, loaded, page_text, offset, file_size,
            line_started_midway, nullptr, layout, out_page);
    } else if (result == ESP_OK) {
        size_t decoded_size = 0U;
        result = transcode_page_window(
            raw, loaded, offset, file_size, encoding,
            decoded, kDecodedPageBytes, source_map, kDecodedSourceMapEntries, &decoded_size);
        if (result == ESP_OK) {
            result = paginate_loaded_page(
                decoded, decoded_size, page_text, offset, file_size,
                line_started_midway, source_map, layout, out_page);
        }
    }

    heap_caps_free(raw);
    heap_caps_free(decoded);
    heap_caps_free(source_map);
    if (result != ESP_OK) {
        heap_caps_free(page_text);
        *out_page = {};
    }
    return result;
}

esp_err_t load_text_page(
    const char *path,
    uint64_t offset,
    TextEncoding::Encoding encoding,
    const PageLayout &layout,
    TextPage *out_page)
{
    return load_text_page_internal(path, offset, encoding, layout, out_page);
}

esp_err_t begin_page_scan(
    const char *path,
    TextEncoding::Encoding encoding,
    PageScanSession *session)
{
    if (path == nullptr || session == nullptr || !path_is_inside_root(path) || !is_txt_name(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    end_page_scan(session);

    struct stat info = {};
    {
        StorageSdLockGuard guard(kStorageLockTimeout);
        if (!guard) return ESP_ERR_TIMEOUT;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return ESP_ERR_NOT_FOUND;
    }
    if (info.st_size <= 0) return ESP_ERR_INVALID_SIZE;

    FILE *file = nullptr;
    {
        StorageSdLockGuard guard(kStorageLockTimeout);
        if (!guard) return ESP_ERR_TIMEOUT;
        file = fopen(path, "rb");
    }
    if (file == nullptr) return ESP_ERR_NOT_FOUND;

    uint8_t *raw = static_cast<uint8_t *>(heap_caps_malloc(
        kPageReadBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t *decoded = nullptr;
    uint16_t *source_map = nullptr;
    if (encoding != TextEncoding::Encoding::Utf8) {
        decoded = static_cast<uint8_t *>(heap_caps_malloc(
            kDecodedPageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        source_map = static_cast<uint16_t *>(heap_caps_malloc(
            kDecodedSourceMapEntries * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    char *page_text = static_cast<char *>(heap_caps_malloc(
        page_text_capacity(encoding), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (raw == nullptr || page_text == nullptr ||
        (encoding != TextEncoding::Encoding::Utf8 && (decoded == nullptr || source_map == nullptr))) {
        heap_caps_free(raw);
        heap_caps_free(decoded);
        heap_caps_free(source_map);
        heap_caps_free(page_text);
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) fclose(file);
        return ESP_ERR_NO_MEM;
    }

    session->file = file;
    session->raw = raw;
    session->decoded = decoded;
    session->source_map = source_map;
    session->page_text = page_text;
    session->file_size = static_cast<uint64_t>(info.st_size);
    session->encoding = encoding;
    return ESP_OK;
}

esp_err_t scan_page(
    PageScanSession *session, uint64_t offset, const PageLayout &layout, PageScanResult *out_result)
{
    if (session == nullptr || session->file == nullptr || session->raw == nullptr ||
        session->page_text == nullptr || session->file_size == 0 || out_result == nullptr ||
        offset >= session->file_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (session->encoding != TextEncoding::Encoding::Utf8 &&
        (session->decoded == nullptr || session->source_map == nullptr)) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_result = {};

    size_t loaded = 0U;
    bool line_started_midway = false;
    esp_err_t ret = read_page_window(
        static_cast<FILE *>(session->file), session->file_size, offset,
        session->encoding, session->raw, &loaded, &line_started_midway);
    if (ret != ESP_OK) return ret;

    const uint8_t *page_source = session->raw;
    size_t page_source_size = loaded;
    const uint16_t *source_map = nullptr;
    if (session->encoding != TextEncoding::Encoding::Utf8) {
        ret = transcode_page_window(
            session->raw, loaded, offset, session->file_size, session->encoding,
            session->decoded, kDecodedPageBytes,
            session->source_map, kDecodedSourceMapEntries, &page_source_size);
        if (ret != ESP_OK) return ret;
        page_source = session->decoded;
        source_map = session->source_map;
    }

    TextPage page = {};
    ret = paginate_loaded_page(
        page_source, page_source_size, session->page_text, offset, session->file_size,
        line_started_midway, source_map, layout, &page);
    if (ret != ESP_OK) return ret;

    out_result->start_offset = page.start_offset;
    out_result->next_offset = page.next_offset;
    out_result->at_end = page.at_end;
    return ESP_OK;
}

void end_page_scan(PageScanSession *session)
{
    if (session == nullptr) return;
    FILE *file = static_cast<FILE *>(session->file);
    if (file != nullptr) {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) fclose(file);
    }
    heap_caps_free(session->raw);
    heap_caps_free(session->decoded);
    heap_caps_free(session->source_map);
    heap_caps_free(session->page_text);
    *session = {};
}

void release_text_page(TextPage *page)
{
    if (page == nullptr) return;
    if (page->text != nullptr) heap_caps_free(page->text);
    *page = {};
}

} // namespace EbookReader
