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

static int entry_compare(const void *lhs_ptr, const void *rhs_ptr)
{
    const Entry &lhs = *static_cast<const Entry *>(lhs_ptr);
    const Entry &rhs = *static_cast<const Entry *>(rhs_ptr);
    if (lhs.is_directory != rhs.is_directory) {
        return lhs.is_directory ? -1 : 1;
    }
    return ascii_casecmp(lhs.name, rhs.name);
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
        if (cp == '\t' || cp == ' ' || cp == 0x3000U) continue;
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

esp_err_t scan_directory(const char *path, DirectorySnapshot *out_snapshot)
{
    if (path == nullptr || out_snapshot == nullptr || !path_is_inside_root(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    release_directory(out_snapshot);

    DIR *dir = nullptr;
    {
        StorageSdLockGuard guard(kStorageLockTimeout);
        if (!guard) return ESP_ERR_TIMEOUT;
        dir = opendir(path);
    }
    if (dir == nullptr) return ESP_ERR_NOT_FOUND;

    Entry *entries = static_cast<Entry *>(heap_caps_calloc(
        kMaxEntries, sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    char *scratch = static_cast<char *>(heap_caps_malloc(
        kEntryNameBytes + kPathBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (entries == nullptr || scratch == nullptr) {
        if (entries != nullptr) heap_caps_free(entries);
        if (scratch != nullptr) heap_caps_free(scratch);
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) closedir(dir);
        return ESP_ERR_NO_MEM;
    }
    char *name = scratch;
    char *full_path = scratch + kEntryNameBytes;

    size_t count = 0;
    bool truncated = false;
    esp_err_t result = ESP_OK;
    while (true) {
        name[0] = '\0';
        bool end = false;
        {
            StorageSdLockGuard guard(kStorageLockTimeout);
            if (!guard) {
                result = ESP_ERR_TIMEOUT;
                break;
            }
            struct dirent *entry = readdir(dir);
            if (entry == nullptr) {
                end = true;
            } else {
                snprintf(name, kEntryNameBytes, "%s", entry->d_name);
            }
        }
        if (end) break;
        if (name_is_hidden(name)) continue;

        full_path[0] = '\0';
        if (join_child_path(path, name, full_path, kPathBytes) != ESP_OK) {
            continue;
        }
        struct stat info = {};
        {
            StorageSdLockGuard guard(kStorageLockTimeout);
            if (!guard) {
                result = ESP_ERR_TIMEOUT;
                break;
            }
            if (stat(full_path, &info) != 0) {
                continue;
            }
        }

        const bool is_dir = S_ISDIR(info.st_mode);
        if (!is_dir && (!S_ISREG(info.st_mode) || !is_txt_name(name))) {
            continue;
        }
        if (count >= kMaxEntries) {
            truncated = true;
            continue;
        }

        Entry &target = entries[count++];
        target.is_directory = is_dir;
        target.size_bytes = is_dir ? 0U : static_cast<uint64_t>(info.st_size < 0 ? 0 : info.st_size);
        snprintf(target.name, sizeof(target.name), "%s", name);
        taskYIELD();
    }

    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) closedir(dir);
    }
    heap_caps_free(scratch);

    if (result != ESP_OK) {
        heap_caps_free(entries);
        return result;
    }
    if (count > 1) {
        qsort(entries, count, sizeof(Entry), entry_compare);
    }

    out_snapshot->entries = entries;
    out_snapshot->count = count;
    out_snapshot->truncated = truncated;
    return ESP_OK;
}

void release_directory(DirectorySnapshot *snapshot)
{
    if (snapshot == nullptr) return;
    if (snapshot->entries != nullptr) {
        heap_caps_free(snapshot->entries);
    }
    *snapshot = {};
}

static esp_err_t load_text_page_internal(
    const char *path,
    uint64_t offset,
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
    uint8_t previous_source_byte = 0;
    bool have_previous_source_byte = false;
    {
        StorageSdLockGuard guard(kStorageLockTimeout);
        if (!guard) return ESP_ERR_TIMEOUT;
        file = fopen(path, "rb");
        if (file != nullptr && offset > 0) {
            if (fseek(file, static_cast<long>(offset - 1U), SEEK_SET) == 0 &&
                fread(&previous_source_byte, 1, 1, file) == 1) {
                have_previous_source_byte = true;
            }
        }
        if (file != nullptr && fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
            fclose(file);
            file = nullptr;
        }
    }
    if (file == nullptr) return ESP_ERR_NOT_FOUND;

    uint8_t *raw = static_cast<uint8_t *>(heap_caps_malloc(
        kPageReadBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    char *page_text = static_cast<char *>(heap_caps_malloc(
        kPageReadBytes * 2U + 64U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (raw == nullptr || page_text == nullptr) {
        if (raw != nullptr) heap_caps_free(raw);
        if (page_text != nullptr) heap_caps_free(page_text);
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t loaded = 0;
    esp_err_t result = ESP_OK;
    const size_t wanted = static_cast<size_t>(
        (file_size - offset) > kPageReadBytes ? kPageReadBytes : (file_size - offset));
    while (loaded < wanted) {
        const size_t chunk = (wanted - loaded) > kReadChunkBytes
            ? kReadChunkBytes
            : (wanted - loaded);
        size_t got = 0;
        {
            StorageSdLockGuard guard(kStorageLockTimeout);
            if (!guard) {
                result = ESP_ERR_TIMEOUT;
                break;
            }
            got = fread(raw + loaded, 1, chunk, file);
        }
        if (got == 0) {
            if (feof(file)) break;
            result = ESP_FAIL;
            break;
        }
        loaded += got;
        taskYIELD();
    }
    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) fclose(file);
    }
    if (result != ESP_OK || loaded == 0) {
        heap_caps_free(raw);
        heap_caps_free(page_text);
        return result != ESP_OK ? result : ESP_FAIL;
    }

    size_t source = 0;
    bool line_started_midway = offset > 0 &&
        (!have_previous_source_byte ||
            (previous_source_byte != '\r' && previous_source_byte != '\n'));
    if (offset == 0) {
        line_started_midway = false;
        if (loaded >= 2 && raw[0] == 0xFFU && raw[1] == 0xFEU) {
            heap_caps_free(raw); heap_caps_free(page_text); return ESP_ERR_NOT_SUPPORTED;
        }
        if (loaded >= 2 && raw[0] == 0xFEU && raw[1] == 0xFFU) {
            heap_caps_free(raw); heap_caps_free(page_text); return ESP_ERR_NOT_SUPPORTED;
        }
        if (loaded >= 3 && raw[0] == 0xEFU && raw[1] == 0xBBU && raw[2] == 0xBFU) {
            source = 3;
        }
    }

    const size_t valid_size = source + trim_incomplete_utf8_tail(raw + source, loaded - source);
    if (valid_size <= source) {
        heap_caps_free(raw);
        heap_caps_free(page_text);
        return ESP_ERR_INVALID_RESPONSE;
    }

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
            heap_caps_free(raw);
            heap_caps_free(page_text);
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
                    heap_caps_free(raw);
                    heap_caps_free(page_text);
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
                heap_caps_free(raw);
                heap_caps_free(page_text);
                return ESP_ERR_INVALID_RESPONSE;
            }

            if (codepoint < 0x20U && codepoint != '\t') {
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

            const bool would_overflow = line_width_px > 0 &&
                static_cast<uint32_t>(line_width_px) + glyph_width_px > layout.text_width_px;
            const bool punctuation_can_hang = is_closing_punctuation(codepoint) ||
                is_sentence_terminal(codepoint) || is_comma_like(codepoint);
            if (would_overflow && !punctuation_can_hang) {
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
        heap_caps_free(raw);
        heap_caps_free(page_text);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 若本页之后直到 EOF 只剩明确网站广告/重复抓取头，则直接把源 offset 吃到文件尾，
    // 避免用户再翻到一张“只有被过滤内容”的空白末页。
    if (source < valid_size && offset + valid_size >= file_size &&
        remaining_is_noise_only(raw, valid_size, source)) {
        source = valid_size;
    }

    page_text[write] = '\0';
    out_page->text = page_text;
    out_page->size = write;
    out_page->start_offset = offset;
    out_page->next_offset = offset + source;
    out_page->file_size = file_size;
    out_page->at_start = offset == 0;
    out_page->at_end = out_page->next_offset >= file_size;
    heap_caps_free(raw);
    return ESP_OK;
}

esp_err_t load_text_page(
    const char *path, uint64_t offset, const PageLayout &layout, TextPage *out_page)
{
    return load_text_page_internal(path, offset, layout, out_page);
}

void release_text_page(TextPage *page)
{
    if (page == nullptr) return;
    if (page->text != nullptr) heap_caps_free(page->text);
    *page = {};
}

} // namespace EbookReader
