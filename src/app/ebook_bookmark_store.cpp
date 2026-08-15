#include "ebook_bookmark_store.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "EbookBookmark";

namespace EbookBookmarkStore
{
namespace {

static constexpr const char *kNamespace = "ebook_bm";
static constexpr const char *kBlobKey = "slots";
static constexpr uint32_t kMagic = 0x314D4245U; // "EBM1"
static constexpr uint16_t kVersion = 1U;
static constexpr size_t kSlotCount = 24U;

struct BookmarkSlot
{
    uint64_t path_hash = 0;
    uint64_t offset = 0;
    uint32_t sequence = 0;
    uint32_t reserved = 0;
};

struct BookmarkBlob
{
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    uint16_t slot_count = kSlotCount;
    uint32_t next_sequence = 1U;
    BookmarkSlot slots[kSlotCount] = {};
};

static uint64_t path_hash64(const char *path)
{
    // FNV-1a 64；仅作为 NVS 索引，不参与安全决策。
    uint64_t hash = 14695981039346656037ULL;
    if (path == nullptr) return 0;
    for (const uint8_t *p = reinterpret_cast<const uint8_t *>(path); *p != 0; ++p) {
        hash ^= static_cast<uint64_t>(*p);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1ULL : hash;
}

static void reset_blob(BookmarkBlob *blob)
{
    if (blob == nullptr) return;
    *blob = {};
    blob->magic = kMagic;
    blob->version = kVersion;
    blob->slot_count = kSlotCount;
    blob->next_sequence = 1U;
}

static esp_err_t read_blob(nvs_handle_t handle, BookmarkBlob *blob)
{
    if (blob == nullptr) return ESP_ERR_INVALID_ARG;
    reset_blob(blob);

    size_t size = sizeof(*blob);
    const esp_err_t ret = nvs_get_blob(handle, kBlobKey, blob, &size);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        reset_blob(blob);
        return ESP_OK;
    }
    if (ret != ESP_OK || size != sizeof(*blob) || blob->magic != kMagic ||
        blob->version != kVersion || blob->slot_count != kSlotCount) {
        ESP_LOGW(TAG, "书签NVS结构无效，按空表恢复：ret=%s size=%u",
            esp_err_to_name(ret), static_cast<unsigned>(size));
        reset_blob(blob);
        return ret == ESP_OK ? ESP_ERR_INVALID_VERSION : ret;
    }
    if (blob->next_sequence == 0) blob->next_sequence = 1U;
    return ESP_OK;
}

static esp_err_t write_blob(nvs_handle_t handle, const BookmarkBlob &blob)
{
    esp_err_t ret = nvs_set_blob(handle, kBlobKey, &blob, sizeof(blob));
    if (ret == ESP_OK) ret = nvs_commit(handle);
    return ret;
}

static int find_slot(const BookmarkBlob &blob, uint64_t hash)
{
    for (size_t i = 0; i < kSlotCount; ++i) {
        if (blob.slots[i].path_hash == hash) return static_cast<int>(i);
    }
    return -1;
}

static size_t choose_slot(const BookmarkBlob &blob)
{
    size_t oldest_index = 0;
    uint32_t oldest_sequence = UINT32_MAX;
    for (size_t i = 0; i < kSlotCount; ++i) {
        if (blob.slots[i].path_hash == 0) return i;
        if (blob.slots[i].sequence < oldest_sequence) {
            oldest_sequence = blob.slots[i].sequence;
            oldest_index = i;
        }
    }
    return oldest_index;
}

} // namespace

esp_err_t load(const char *path, uint64_t file_size, uint64_t *out_offset, bool *out_found)
{
    if (path == nullptr || path[0] == '\0' || out_offset == nullptr || out_found == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_offset = 0;
    *out_found = false;

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;

    BookmarkBlob blob = {};
    ret = read_blob(handle, &blob);
    nvs_close(handle);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_VERSION) return ret;
    if (ret == ESP_ERR_INVALID_VERSION) return ESP_OK;

    const int index = find_slot(blob, path_hash64(path));
    if (index < 0) return ESP_OK;
    const uint64_t offset = blob.slots[index].offset;
    if (file_size == 0 || offset >= file_size) {
        ESP_LOGW(TAG, "忽略失效书签：offset=%llu size=%llu",
            static_cast<unsigned long long>(offset),
            static_cast<unsigned long long>(file_size));
        return ESP_OK;
    }
    *out_offset = offset;
    *out_found = true;
    return ESP_OK;
}

esp_err_t save(const char *path, uint64_t offset)
{
    if (path == nullptr || path[0] == '\0') return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    BookmarkBlob blob = {};
    const esp_err_t read_ret = read_blob(handle, &blob);
    if (read_ret != ESP_OK && read_ret != ESP_ERR_INVALID_VERSION) {
        nvs_close(handle);
        return read_ret;
    }
    if (read_ret == ESP_ERR_INVALID_VERSION) reset_blob(&blob);

    const uint64_t hash = path_hash64(path);
    int found = find_slot(blob, hash);
    const size_t index = found >= 0 ? static_cast<size_t>(found) : choose_slot(blob);
    blob.slots[index].path_hash = hash;
    blob.slots[index].offset = offset;
    blob.slots[index].sequence = blob.next_sequence++;
    if (blob.next_sequence == 0) blob.next_sequence = 1U;

    ret = write_blob(handle, blob);
    nvs_close(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "书签已保存：slot=%u offset=%llu",
            static_cast<unsigned>(index), static_cast<unsigned long long>(offset));
    }
    return ret;
}

esp_err_t clear(const char *path)
{
    if (path == nullptr || path[0] == '\0') return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;

    BookmarkBlob blob = {};
    const esp_err_t read_ret = read_blob(handle, &blob);
    if (read_ret != ESP_OK) {
        nvs_close(handle);
        return read_ret == ESP_ERR_INVALID_VERSION ? ESP_OK : read_ret;
    }
    const int index = find_slot(blob, path_hash64(path));
    if (index < 0) {
        nvs_close(handle);
        return ESP_OK;
    }

    blob.slots[index] = {};
    ret = write_blob(handle, blob);
    nvs_close(handle);
    if (ret == ESP_OK) ESP_LOGI(TAG, "书签已删除：slot=%d", index);
    return ret;
}

} // namespace EbookBookmarkStore
