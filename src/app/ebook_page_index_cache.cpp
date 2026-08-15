#include "ebook_page_index_cache.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage_io.h"

static const char *TAG = "EbookIdxCache";

namespace EbookPageIndexCache
{
namespace
{

static constexpr const char *kSystemDirectory = "/sdcard/System";
static constexpr const char *kCacheDirectory = "/sdcard/System/ebook_index";
static constexpr uint32_t kMagic = 0x58444945U; // "EIDX"
static constexpr uint16_t kVersion = 1U;
static constexpr uint16_t kMaxBlockPages = 32U;
static constexpr size_t kCachePathBytes = 160U;

#pragma pack(push, 1)
struct CacheHeader
{
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    uint8_t view_slot = 0;
    uint8_t reserved0 = 0;
    uint32_t layout_signature = 0;
    uint64_t path_hash = 0;
    uint64_t file_size = 0;
    int64_t modified_time = 0;
    uint32_t reserved1 = 0;
};

struct CacheBlockHeader
{
    uint32_t first_ordinal = 0;
    uint16_t count = 0;
    uint16_t reserved = 0;
    uint32_t payload_crc32 = 0;
};
#pragma pack(pop)

static_assert(sizeof(CacheHeader) == 40, "Ebook cache header layout changed");
static_assert(sizeof(CacheBlockHeader) == 12, "Ebook cache block layout changed");

static uint64_t path_hash64(const char *path)
{
    uint64_t hash = 14695981039346656037ULL;
    if (path == nullptr) return 0;
    for (const uint8_t *p = reinterpret_cast<const uint8_t *>(path); *p != 0; ++p) {
        hash ^= static_cast<uint64_t>(*p);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1ULL : hash;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

static uint32_t crc32_buffer(const void *data, size_t size)
{
    return crc32_update(0xFFFFFFFFU, data, size) ^ 0xFFFFFFFFU;
}

static void *psram_alloc(size_t bytes)
{
    if (bytes == 0) return nullptr;
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool build_cache_path(const char *book_path, uint8_t view_slot, char *out, size_t out_size)
{
    if (book_path == nullptr || out == nullptr || out_size == 0 || view_slot > 1U) return false;
    const uint64_t hash = path_hash64(book_path);
    const int written = snprintf(out, out_size, "%s/%016llx_%c.idx",
        kCacheDirectory,
        static_cast<unsigned long long>(hash),
        view_slot == 0 ? 'f' : 'l');
    return written > 0 && static_cast<size_t>(written) < out_size;
}

static bool ensure_cache_directory()
{
    StorageSdLockGuard guard;
    if (!guard) return false;

    struct stat info = {};
    if (stat(kSystemDirectory, &info) != 0) {
        if (mkdir(kSystemDirectory, 0775) != 0 && errno != EEXIST) return false;
    } else if (!S_ISDIR(info.st_mode)) {
        return false;
    }

    memset(&info, 0, sizeof(info));
    if (stat(kCacheDirectory, &info) != 0) {
        if (mkdir(kCacheDirectory, 0775) != 0 && errno != EEXIST) return false;
    } else if (!S_ISDIR(info.st_mode)) {
        return false;
    }
    return true;
}

static bool read_book_identity(const char *book_path, uint64_t expected_file_size, CacheHeader *header,
    uint8_t view_slot, uint32_t layout_signature)
{
    if (book_path == nullptr || header == nullptr) return false;
    StorageSdLockGuard guard;
    if (!guard) return false;
    struct stat info = {};
    if (stat(book_path, &info) != 0 || !S_ISREG(info.st_mode)) return false;
    const uint64_t file_size = static_cast<uint64_t>(info.st_size);
    if (expected_file_size != 0 && file_size != expected_file_size) return false;

    *header = {};
    header->magic = kMagic;
    header->version = kVersion;
    header->view_slot = view_slot;
    header->layout_signature = layout_signature;
    header->path_hash = path_hash64(book_path);
    header->file_size = file_size;
    header->modified_time = static_cast<int64_t>(info.st_mtime);
    return true;
}

static bool header_matches(const CacheHeader &actual, const CacheHeader &expected)
{
    return actual.magic == kMagic && actual.version == kVersion &&
        actual.view_slot == expected.view_slot &&
        actual.layout_signature == expected.layout_signature &&
        actual.path_hash == expected.path_hash &&
        actual.file_size == expected.file_size &&
        actual.modified_time == expected.modified_time;
}

static bool locked_read_exact(FILE *file, void *buffer, size_t bytes)
{
    if (file == nullptr || buffer == nullptr || bytes == 0) return false;
    StorageSdLockGuard guard;
    return guard && fread(buffer, 1, bytes, file) == bytes;
}

static bool locked_write_exact(FILE *file, const void *buffer, size_t bytes)
{
    if (file == nullptr || buffer == nullptr || bytes == 0) return false;
    StorageSdLockGuard guard;
    return guard && fwrite(buffer, 1, bytes, file) == bytes;
}

static FILE *locked_fopen(const char *path, const char *mode)
{
    StorageSdLockGuard guard;
    if (!guard) return nullptr;
    return fopen(path, mode);
}

static void locked_fclose(FILE *file)
{
    if (file == nullptr) return;
    StorageSdLockGuard guard;
    if (guard) fclose(file);
}

static bool locked_flush(FILE *file)
{
    if (file == nullptr) return false;
    StorageSdLockGuard guard;
    if (!guard || fflush(file) != 0) return false;
    const int descriptor = fileno(file);
    return descriptor < 0 || fsync(descriptor) == 0;
}

static void locked_remove(const char *path)
{
    if (path == nullptr) return;
    StorageSdLockGuard guard;
    if (guard) remove(path);
}

static bool locked_publish_temp(const char *temp_path, const char *final_path)
{
    StorageSdLockGuard guard;
    if (!guard) return false;
    remove(final_path);
    return rename(temp_path, final_path) == 0;
}

static bool append_result_offset(LoadResult *result, uint64_t offset, uint64_t file_size)
{
    if (result == nullptr || offset >= file_size) return false;
    if (result->count > 0 && offset <= result->starts[result->count - 1U]) return false;

    if (result->count >= result->capacity) {
        const size_t next_capacity = result->capacity == 0 ? 64U : result->capacity * 2U;
        if (next_capacity < result->capacity || next_capacity > (SIZE_MAX / sizeof(uint64_t))) return false;
        uint64_t *next = static_cast<uint64_t *>(psram_alloc(next_capacity * sizeof(uint64_t)));
        if (next == nullptr) return false;
        if (result->starts != nullptr && result->count > 0) {
            memcpy(next, result->starts, result->count * sizeof(uint64_t));
            heap_caps_free(result->starts);
        }
        result->starts = next;
        result->capacity = next_capacity;
    }
    result->starts[result->count++] = offset;
    return true;
}

static esp_err_t write_blocks(FILE *file, const uint64_t *starts, size_t first, size_t count)
{
    if (file == nullptr || starts == nullptr || first > count) return ESP_ERR_INVALID_ARG;
    size_t ordinal = first;
    while (ordinal < count) {
        const size_t remain = count - ordinal;
        const uint16_t block_count = static_cast<uint16_t>(
            remain > kMaxBlockPages ? kMaxBlockPages : remain);
        CacheBlockHeader block = {};
        block.first_ordinal = static_cast<uint32_t>(ordinal);
        block.count = block_count;
        block.payload_crc32 = crc32_buffer(starts + ordinal, block_count * sizeof(uint64_t));
        if (!locked_write_exact(file, &block, sizeof(block)) ||
            !locked_write_exact(file, starts + ordinal, block_count * sizeof(uint64_t))) {
            return ESP_FAIL;
        }
        ordinal += block_count;
    }
    return locked_flush(file) ? ESP_OK : ESP_FAIL;
}

static esp_err_t recreate_cache(
    const char *cache_path, const CacheHeader &header, const uint64_t *starts, size_t count)
{
    if (!ensure_cache_directory()) return ESP_FAIL;
    char temp_path[kCachePathBytes] = {};
    const int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", cache_path);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(temp_path)) return ESP_ERR_INVALID_SIZE;
    locked_remove(temp_path);

    FILE *file = locked_fopen(temp_path, "wb");
    if (file == nullptr) return ESP_FAIL;
    esp_err_t ret = ESP_OK;
    if (!locked_write_exact(file, &header, sizeof(header))) {
        ret = ESP_FAIL;
    } else if (count > 0) {
        ret = write_blocks(file, starts, 0, count);
    } else if (!locked_flush(file)) {
        ret = ESP_FAIL;
    }
    locked_fclose(file);

    if (ret != ESP_OK || !locked_publish_temp(temp_path, cache_path)) {
        locked_remove(temp_path);
        return ret != ESP_OK ? ret : ESP_FAIL;
    }
    return ESP_OK;
}

} // namespace

esp_err_t load(
    const char *book_path,
    uint8_t view_slot,
    uint64_t expected_file_size,
    uint32_t layout_signature,
    LoadResult *out_result)
{
    if (book_path == nullptr || out_result == nullptr || view_slot > 1U) return ESP_ERR_INVALID_ARG;
    release_load_result(out_result);

    CacheHeader expected = {};
    if (!read_book_identity(book_path, expected_file_size, &expected, view_slot, layout_signature)) {
        return ESP_ERR_NOT_FOUND;
    }

    char cache_path[kCachePathBytes] = {};
    if (!build_cache_path(book_path, view_slot, cache_path, sizeof(cache_path))) return ESP_ERR_INVALID_SIZE;

    FILE *file = locked_fopen(cache_path, "rb");
    if (file == nullptr) return ESP_ERR_NOT_FOUND;

    CacheHeader actual = {};
    if (!locked_read_exact(file, &actual, sizeof(actual)) || !header_matches(actual, expected)) {
        locked_fclose(file);
        locked_remove(cache_path);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = ESP_OK;
    bool saw_block = false;
    for (;;) {
        CacheBlockHeader block = {};
        size_t got = 0;
        {
            StorageSdLockGuard guard;
            if (!guard) {
                ret = ESP_ERR_TIMEOUT;
                break;
            }
            got = fread(&block, 1, sizeof(block), file);
        }
        if (got == 0) break; // clean EOF
        if (got != sizeof(block) || block.count == 0 || block.count > kMaxBlockPages ||
            block.first_ordinal != out_result->count) {
            ret = ESP_ERR_INVALID_CRC;
            break;
        }

        uint64_t payload[kMaxBlockPages] = {};
        const size_t payload_bytes = static_cast<size_t>(block.count) * sizeof(uint64_t);
        if (!locked_read_exact(file, payload, payload_bytes) ||
            crc32_buffer(payload, payload_bytes) != block.payload_crc32) {
            ret = ESP_ERR_INVALID_CRC;
            break;
        }
        for (uint16_t i = 0; i < block.count; ++i) {
            if (!append_result_offset(out_result, payload[i], expected.file_size)) {
                ret = ESP_ERR_INVALID_RESPONSE;
                break;
            }
        }
        if (ret != ESP_OK) break;
        saw_block = true;
    }
    locked_fclose(file);

    const bool valid_prefix = saw_block && out_result->count > 0 && out_result->starts[0] == 0;
    if (ret != ESP_OK && valid_prefix) {
        // 典型场景是断电发生在最后一个 append block 中间。前面的 block 都有独立 CRC，
        // 因此保留已验证前缀并重写一个干净 cache，而不是把全部历史页索引丢掉。
        ESP_LOGW(TAG, "分页缓存尾块损坏，保留有效前缀：slot=%u pages=%u ret=%s",
            static_cast<unsigned>(view_slot), static_cast<unsigned>(out_result->count),
            esp_err_to_name(ret));
        const esp_err_t repair_ret = recreate_cache(cache_path, expected, out_result->starts, out_result->count);
        if (repair_ret != ESP_OK) {
            ESP_LOGW(TAG, "分页缓存前缀修复失败：slot=%u ret=%s",
                static_cast<unsigned>(view_slot), esp_err_to_name(repair_ret));
        }
        ret = ESP_OK;
    }

    if (ret != ESP_OK || !valid_prefix) {
        ESP_LOGW(TAG, "分页缓存无效，丢弃：slot=%u ret=%s pages=%u",
            static_cast<unsigned>(view_slot), esp_err_to_name(ret),
            static_cast<unsigned>(out_result->count));
        release_load_result(out_result);
        locked_remove(cache_path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "分页缓存命中：slot=%u pages=%u last=%llu",
        static_cast<unsigned>(view_slot), static_cast<unsigned>(out_result->count),
        static_cast<unsigned long long>(out_result->starts[out_result->count - 1U]));
    return ESP_OK;
}

esp_err_t commit_prefix(
    const char *book_path,
    uint8_t view_slot,
    uint64_t expected_file_size,
    uint32_t layout_signature,
    const uint64_t *starts,
    size_t count,
    size_t persisted_count,
    size_t *out_persisted_count)
{
    if (out_persisted_count != nullptr) *out_persisted_count = persisted_count;
    if (book_path == nullptr || view_slot > 1U || starts == nullptr || count == 0 ||
        persisted_count > count || starts[0] != 0 || count > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    CacheHeader expected = {};
    if (!read_book_identity(book_path, expected_file_size, &expected, view_slot, layout_signature)) {
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < count; ++i) {
        if (starts[i] >= expected.file_size || (i > 0 && starts[i] <= starts[i - 1U])) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    char cache_path[kCachePathBytes] = {};
    if (!build_cache_path(book_path, view_slot, cache_path, sizeof(cache_path))) return ESP_ERR_INVALID_SIZE;
    if (!ensure_cache_directory()) return ESP_FAIL;

    if (persisted_count == 0) {
        const esp_err_t ret = recreate_cache(cache_path, expected, starts, count);
        if (ret == ESP_OK && out_persisted_count != nullptr) *out_persisted_count = count;
        return ret;
    }
    if (persisted_count == count) {
        if (out_persisted_count != nullptr) *out_persisted_count = count;
        return ESP_OK;
    }

    // Append 前只验证固定身份头。调用方的 persisted_count 来自本次 load/成功 commit；
    // 若文件缺失或身份变化，则直接用当前完整前缀重建，避免在错误尾部继续追加。
    FILE *file = locked_fopen(cache_path, "rb");
    CacheHeader actual = {};
    const bool valid_existing = file != nullptr && locked_read_exact(file, &actual, sizeof(actual)) &&
        header_matches(actual, expected);
    locked_fclose(file);
    if (!valid_existing) {
        const esp_err_t ret = recreate_cache(cache_path, expected, starts, count);
        if (ret == ESP_OK && out_persisted_count != nullptr) *out_persisted_count = count;
        return ret;
    }

    file = locked_fopen(cache_path, "ab");
    if (file == nullptr) return ESP_FAIL;
    const esp_err_t ret = write_blocks(file, starts, persisted_count, count);
    locked_fclose(file);
    if (ret == ESP_OK) {
        if (out_persisted_count != nullptr) *out_persisted_count = count;
    } else {
        // 同一运行期 append 失败后不在可能存在的半块尾部继续追加；下次提交直接完整重建。
        locked_remove(cache_path);
    }
    return ret;
}

void release_load_result(LoadResult *result)
{
    if (result == nullptr) return;
    if (result->starts != nullptr) heap_caps_free(result->starts);
    *result = {};
}

} // namespace EbookPageIndexCache
