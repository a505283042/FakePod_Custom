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

esp_err_t load_text_page(
    const char *path, uint64_t offset, const PageLayout &layout, TextPage *out_page)
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
    if (offset == 0) {
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
    uint8_t line = 0;
    uint16_t line_width_px = 0;
    while (source < valid_size && line < layout.max_lines) {
        // CRLF/CR 都归一化成一个换行；页尾只消费行结束符，不提前消费下一页的可见字符。
        if (raw[source] == '\r') {
            if (source + 1U >= valid_size && offset + valid_size < file_size) break;
            const size_t newline_bytes =
                (source + 1U < valid_size && raw[source + 1U] == '\n') ? 2U : 1U;
            source += newline_bytes;
            if (line + 1U >= layout.max_lines) break;
            page_text[write++] = '\n';
            ++line;
            line_width_px = 0;
            continue;
        }
        if (raw[source] == '\n') {
            ++source;
            if (line + 1U >= layout.max_lines) break;
            page_text[write++] = '\n';
            ++line;
            line_width_px = 0;
            continue;
        }

        uint32_t codepoint = 0;
        size_t codepoint_bytes = 0;
        const Utf8DecodeResult decode = decode_utf8_one(
            raw, valid_size, source, &codepoint, &codepoint_bytes);
        if (decode == Utf8DecodeResult::Incomplete) break;
        if (decode != Utf8DecodeResult::Ok) {
            heap_caps_free(raw);
            heap_caps_free(page_text);
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (codepoint < 0x20U && codepoint != '\t') {
            source += codepoint_bytes;
            continue;
        }

        uint32_t next_codepoint = 0;
        if (source + codepoint_bytes < valid_size &&
            raw[source + codepoint_bytes] != '\r' && raw[source + codepoint_bytes] != '\n') {
            size_t next_bytes = 0;
            if (decode_utf8_one(raw, valid_size, source + codepoint_bytes,
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

        if (line_width_px > 0 &&
            static_cast<uint32_t>(line_width_px) + glyph_width_px > layout.text_width_px) {
            if (line + 1U >= layout.max_lines) break;
            page_text[write++] = '\n';
            ++line;
            line_width_px = 0;
        }
        if (line >= layout.max_lines) break;

        if (codepoint == '\t') {
            for (uint8_t n = 0; n < 4U; ++n) page_text[write++] = ' ';
        } else {
            memcpy(page_text + write, raw + source, codepoint_bytes);
            write += codepoint_bytes;
        }
        source += codepoint_bytes;
        const uint32_t next_width = static_cast<uint32_t>(line_width_px) + glyph_width_px;
        line_width_px = static_cast<uint16_t>(next_width > UINT16_MAX ? UINT16_MAX : next_width);
    }

    if (source <= page_source_start) {
        heap_caps_free(raw);
        heap_caps_free(page_text);
        return ESP_ERR_INVALID_RESPONSE;
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

void release_text_page(TextPage *page)
{
    if (page == nullptr) return;
    if (page->text != nullptr) heap_caps_free(page->text);
    *page = {};
}

} // namespace EbookReader
