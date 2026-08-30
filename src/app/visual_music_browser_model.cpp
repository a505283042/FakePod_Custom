#include "visual_music_browser_model.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage_io.h"

namespace VisualMusicBrowser
{
namespace
{

static constexpr TickType_t kStorageLockTimeout = pdMS_TO_TICKS(30);
static constexpr size_t kInitialEntryCapacity = 32U;
static constexpr size_t kInitialPoolCapacity = 2048U;

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

static bool ascii_extension_is(const char *name, const char *extension)
{
    if (name == nullptr || extension == nullptr || extension[0] != '.') return false;
    const size_t name_len = strlen(name);
    const size_t ext_len = strlen(extension);
    if (name_len < ext_len) return false;
    const char *actual = name + name_len - ext_len;
    for (size_t i = 0U; i < ext_len; ++i) {
        if (ascii_tolower(static_cast<unsigned char>(actual[i])) !=
            ascii_tolower(static_cast<unsigned char>(extension[i]))) {
            return false;
        }
    }
    return true;
}

static uint32_t supported_file_flag(const char *name)
{
    if (ascii_extension_is(name, ".mid") || ascii_extension_is(name, ".midi")) {
        return kEntryMidi;
    }
    if (ascii_extension_is(name, ".nsf") || ascii_extension_is(name, ".nsfe")) {
        return kEntryNsf;
    }
    return kEntryNone;
}

static bool index_is_directory(const EntryIndex &entry)
{
    return (entry.flags & kEntryDirectory) != 0U;
}

static const char *pool_string(const char *pool, size_t pool_size, uint32_t offset)
{
    if (pool == nullptr || static_cast<size_t>(offset) >= pool_size) return nullptr;
    const char *text = pool + offset;
    return memchr(text, '\0', pool_size - static_cast<size_t>(offset)) != nullptr
        ? text
        : nullptr;
}

static int entry_compare(
    const EntryIndex &lhs,
    const EntryIndex &rhs,
    const char *pool,
    size_t pool_size)
{
    const bool lhs_dir = index_is_directory(lhs);
    const bool rhs_dir = index_is_directory(rhs);
    if (lhs_dir != rhs_dir) return lhs_dir ? -1 : 1;

    const char *lhs_name = pool_string(pool, pool_size, lhs.name_off);
    const char *rhs_name = pool_string(pool, pool_size, rhs.name_off);
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
    if (*block != nullptr && old_bytes > 0U) memcpy(next, *block, old_bytes);
    if (*block != nullptr) heap_caps_free(*block);
    *block = next;
    return true;
}

static bool reserve_entries(DirectoryScanSession *session, size_t needed)
{
    if (session == nullptr) return false;
    if (needed <= session->entry_capacity) return true;
    if (needed > SIZE_MAX / sizeof(EntryIndex)) return false;

    size_t next_capacity = session->entry_capacity > 0U
        ? session->entry_capacity
        : kInitialEntryCapacity;
    while (next_capacity < needed) {
        if (next_capacity > SIZE_MAX / 2U) {
            next_capacity = needed;
            break;
        }
        next_capacity *= 2U;
    }
    if (next_capacity > SIZE_MAX / sizeof(EntryIndex)) return false;

    void *block = session->entries;
    if (!grow_psram_block(
            &block,
            session->entry_capacity * sizeof(EntryIndex),
            next_capacity * sizeof(EntryIndex))) {
        return false;
    }
    session->entries = static_cast<EntryIndex *>(block);
    session->entry_capacity = next_capacity;
    return true;
}

static bool reserve_pool(DirectoryScanSession *session, size_t needed)
{
    if (session == nullptr) return false;
    if (needed <= session->pool_capacity) return true;

    size_t next_capacity = session->pool_capacity > 0U
        ? session->pool_capacity
        : kInitialPoolCapacity;
    while (next_capacity < needed) {
        if (next_capacity > SIZE_MAX / 2U) {
            next_capacity = needed;
            break;
        }
        next_capacity *= 2U;
    }

    void *block = session->string_pool;
    if (!grow_psram_block(&block, session->pool_capacity, next_capacity)) return false;
    session->string_pool = static_cast<char *>(block);
    session->pool_capacity = next_capacity;
    return true;
}

static esp_err_t append_entry(
    DirectoryScanSession *session,
    const char *name,
    uint32_t flags,
    uint64_t size_bytes)
{
    if (session == nullptr || name == nullptr || name[0] == '\0') return ESP_ERR_INVALID_ARG;
    const size_t name_bytes = strlen(name) + 1U;
    if (session->pool_size > UINT32_MAX ||
        name_bytes > static_cast<size_t>(UINT32_MAX) - session->pool_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!reserve_entries(session, session->count + 1U) ||
        !reserve_pool(session, session->pool_size + name_bytes)) {
        return ESP_ERR_NO_MEM;
    }

    const uint32_t name_off = static_cast<uint32_t>(session->pool_size);
    memcpy(session->string_pool + session->pool_size, name, name_bytes);
    session->pool_size += name_bytes;

    EntryIndex &entry = session->entries[session->count++];
    entry.size_bytes = (flags & kEntryDirectory) != 0U ? 0U : size_bytes;
    entry.name_off = name_off;
    entry.flags = flags;
    return ESP_OK;
}

static void heap_sift_down(
    EntryIndex *entries,
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
            entry_compare(entries[largest], entries[right], pool, pool_size) < 0) {
            largest = right;
        }
        if (entry_compare(entries[root], entries[largest], pool, pool_size) >= 0) return;
        const EntryIndex temp = entries[root];
        entries[root] = entries[largest];
        entries[largest] = temp;
        root = largest;
    }
}

static void sort_entries(EntryIndex *entries, size_t count, const char *pool, size_t pool_size)
{
    if (entries == nullptr || count < 2U || pool == nullptr) return;
    for (size_t start = count / 2U; start > 0U; --start) {
        heap_sift_down(entries, count, start - 1U, pool, pool_size);
    }
    for (size_t end = count; end > 1U; --end) {
        const EntryIndex temp = entries[0];
        entries[0] = entries[end - 1U];
        entries[end - 1U] = temp;
        heap_sift_down(entries, end - 1U, 0U, pool, pool_size);
    }
}

} // namespace

bool path_is_inside_root(const char *path)
{
    if (path == nullptr) return false;
    const size_t root_len = strlen(kRootDirectory);
    return strncmp(path, kRootDirectory, root_len) == 0 &&
        (path[root_len] == '\0' || path[root_len] == '/');
}

esp_err_t join_child_path(const char *parent, const char *name, char *out, size_t out_size)
{
    if (parent == nullptr || name == nullptr || out == nullptr || out_size == 0U ||
        !path_is_inside_root(parent) || name_is_hidden(name) || strchr(name, '/') != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(out, out_size, "%s/%s", parent, name);
    if (written < 0 || static_cast<size_t>(written) >= out_size) return ESP_ERR_INVALID_SIZE;
    return path_is_inside_root(out) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t parent_path(const char *path, char *out, size_t out_size)
{
    if (path == nullptr || out == nullptr || out_size == 0U || !path_is_inside_root(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(path, kRootDirectory) == 0) {
        if (strlen(kRootDirectory) + 1U > out_size) return ESP_ERR_INVALID_SIZE;
        strcpy(out, kRootDirectory);
        return ESP_OK;
    }
    const char *slash = strrchr(path, '/');
    if (slash == nullptr || slash <= path + strlen(kRootDirectory) - 1) return ESP_ERR_INVALID_STATE;
    const size_t len = static_cast<size_t>(slash - path);
    if (len + 1U > out_size) return ESP_ERR_INVALID_SIZE;
    memcpy(out, path, len);
    out[len] = '\0';
    return path_is_inside_root(out) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t begin_directory_scan(const char *path, DirectoryScanSession *session)
{
    if (path == nullptr || session == nullptr || !path_is_inside_root(path)) return ESP_ERR_INVALID_ARG;
    cancel_directory_scan(session);

    DIR *dir = nullptr;
    {
        StorageSdLockGuard guard(kStorageLockTimeout);
        if (!guard) return ESP_ERR_TIMEOUT;
        dir = opendir(path);
    }
    if (dir == nullptr) return ESP_ERR_NOT_FOUND;

    char *path_copy = static_cast<char *>(heap_caps_calloc(
        kPathBytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
    if (!reserve_entries(session, kInitialEntryCapacity) ||
        !reserve_pool(session, kInitialPoolCapacity)) {
        cancel_directory_scan(session);
        return ESP_ERR_NO_MEM;
    }
    session->string_pool[0] = '\0';
    session->pool_size = 1U;
    return ESP_OK;
}

esp_err_t scan_directory_step(
    DirectoryScanSession *session,
    size_t max_raw_entries,
    bool *out_done)
{
    if (session == nullptr || out_done == nullptr || session->dir == nullptr ||
        session->entries == nullptr || session->string_pool == nullptr ||
        session->path == nullptr || session->scratch == nullptr || max_raw_entries == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_done = false;
    DIR *dir = static_cast<DIR *>(session->dir);
    char *name = session->scratch;
    char *full_path = session->scratch + kEntryNameBytes;

    for (size_t processed = 0U; processed < max_raw_entries; ++processed) {
        name[0] = '\0';
        bool end = false;
        {
            StorageSdLockGuard guard(kStorageLockTimeout);
            if (!guard) return ESP_ERR_TIMEOUT;
            struct dirent *entry = readdir(dir);
            if (entry == nullptr) end = true;
            else snprintf(name, kEntryNameBytes, "%s", entry->d_name);
        }
        if (end) {
            *out_done = true;
            return ESP_OK;
        }
        if (name_is_hidden(name)) {
            taskYIELD();
            continue;
        }

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
        uint32_t flags = is_dir ? kEntryDirectory : supported_file_flag(name);
        if (!is_dir && (!S_ISREG(info.st_mode) || flags == kEntryNone)) {
            taskYIELD();
            continue;
        }
        const uint64_t size_bytes = is_dir
            ? 0U
            : static_cast<uint64_t>(info.st_size < 0 ? 0 : info.st_size);
        const esp_err_t append_ret = append_entry(session, name, flags, size_bytes);
        if (append_ret != ESP_OK) return append_ret;
        taskYIELD();
    }
    return ESP_OK;
}

esp_err_t finish_directory_scan(
    DirectoryScanSession *session,
    DirectorySnapshot *out_snapshot)
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
    sort_entries(session->entries, session->count, session->string_pool, session->pool_size);

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
    session->count = 0U;
    session->entry_capacity = 0U;
    session->pool_size = 0U;
    session->pool_capacity = 0U;
    return ESP_OK;
}

void cancel_directory_scan(DirectoryScanSession *session)
{
    if (session == nullptr) return;
    if (session->dir != nullptr) {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) closedir(static_cast<DIR *>(session->dir));
    }
    if (session->entries != nullptr) heap_caps_free(session->entries);
    if (session->string_pool != nullptr) heap_caps_free(session->string_pool);
    if (session->path != nullptr) heap_caps_free(session->path);
    if (session->scratch != nullptr) heap_caps_free(session->scratch);
    *session = {};
}

void release_directory(DirectorySnapshot *snapshot)
{
    if (snapshot == nullptr) return;
    if (snapshot->entries != nullptr) heap_caps_free(snapshot->entries);
    if (snapshot->string_pool != nullptr) heap_caps_free(snapshot->string_pool);
    *snapshot = {};
}

const EntryIndex *entry_at(const DirectorySnapshot *snapshot, size_t index)
{
    if (snapshot == nullptr || snapshot->entries == nullptr || index >= snapshot->count) return nullptr;
    return &snapshot->entries[index];
}

const char *entry_name(const DirectorySnapshot *snapshot, size_t index)
{
    const EntryIndex *entry = entry_at(snapshot, index);
    if (entry == nullptr) return nullptr;
    return pool_string(snapshot->string_pool, snapshot->pool_size, entry->name_off);
}

bool entry_is_directory(const EntryIndex *entry)
{
    return entry != nullptr && (entry->flags & kEntryDirectory) != 0U;
}

bool entry_is_midi(const EntryIndex *entry)
{
    return entry != nullptr && (entry->flags & kEntryMidi) != 0U;
}

bool entry_is_nsf(const EntryIndex *entry)
{
    return entry != nullptr && (entry->flags & kEntryNsf) != 0U;
}

const char *entry_kind_name(const EntryIndex *entry)
{
    if (entry_is_directory(entry)) return ">";
    if (entry_is_midi(entry)) return "MIDI";
    if (entry_is_nsf(entry)) return "NSF";
    return "?";
}

} // namespace VisualMusicBrowser
