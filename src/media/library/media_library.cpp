#include "media_library.h"
#include "storage_io.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "media_index_store.h"
#include "media_catalog_v2.h"
#include "media_groups_v2.h"
#include "media_catalog_store_v2.h"
#include "media_probe.h"
#include "media_metadata.h"
#include "media_artwork.h"
#include "app_diag_config.h"
#include "system_paths.h"

static const char *TAG = "音乐库";

#if APP_DIAG_BOOT_VERBOSE
#define LIB_BOOT_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define LIB_BOOT_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif
static constexpr const char *MUSIC_ROOT = "/sdcard/MUSIC";
static constexpr size_t INITIAL_ENTRY_CAPACITY = 128;
static constexpr size_t INITIAL_PATH_CAPACITY = 16 * 1024;
static constexpr size_t INITIAL_DIR_CAPACITY = 16;
// /MUSIC 根目录为深度0，最多进入两级子目录：/MUSIC/A/B。
static constexpr uint8_t MAX_MUSIC_SUBDIR_DEPTH = 2U;
// 单个文件某一深度解析阶段超过这个时间就记录路径，方便定位坏标签/慢卡。
static constexpr int64_t SLOW_SCAN_STAGE_WARN_US = 500000LL;
// 首次建库是同步任务；每处理少量歌曲主动阻塞一个 tick，避免长时间占满启动核。
static constexpr size_t SCAN_YIELD_INTERVAL_TRACKS = 8U;
static constexpr TickType_t LIBRARY_SD_LOCK_TIMEOUT = pdMS_TO_TICKS(2000);

// 正常开机的“新增歌曲快扫”先用一个极轻量的 TF 变更戳判断介质是否可能变化。
// 命中时直接复用已经通过 CRC/semantic 校验的 V2 Catalog，不再枚举 /MUSIC 的千个长文件名。
// 正常开机和 USB MSC 归还都先走这条快判定；只有变更戳不一致才进入完整增量扫描。
static bool media_library_errno_is_missing(int error_code)
{
    return error_code == ENOENT || error_code == ENOTDIR;
}

static constexpr uint32_t LIBRARY_QUICK_STAMP_MAGIC = 0x46505331U; // "FPS1"
static constexpr uint16_t LIBRARY_QUICK_STAMP_VERSION = 1U;

struct MediaLibraryQuickStamp
{
    uint32_t magic = LIBRARY_QUICK_STAMP_MAGIC;
    uint16_t version = LIBRARY_QUICK_STAMP_VERSION;
    uint16_t struct_size = 0U;
    uint32_t index_crc32 = 0U;
    uint32_t track_count = 0U;
    uint64_t total_bytes = 0U;
    uint64_t free_bytes = 0U;
    int64_t music_root_mtime = 0;
    int64_t music_root_ctime = 0;
    uint64_t music_root_size = 0U;
    uint32_t checksum = 0U;
};
#if APP_DIAG_LIBRARY_ITEMS || APP_DIAG_LIBRARY_METADATA || APP_DIAG_LIBRARY_ARTWORK
static constexpr size_t LOG_TRACK_LIMIT = 10;
#endif

using MediaEntry = MediaIndexRecord;

struct DirectoryStackItem
{
    char *path = nullptr;
    uint8_t depth = 0U;
};

struct DirectoryStack
{
    DirectoryStackItem *items;
    size_t count;
    size_t capacity;
};

struct DirectoryLrcIndex
{
    char **names = nullptr;
    size_t count = 0U;
    size_t capacity = 0U;
};

static esp_err_t media_library_query_quick_stamp_inputs(
    uint64_t *out_total_bytes,
    uint64_t *out_free_bytes,
    int64_t *out_music_root_mtime,
    int64_t *out_music_root_ctime,
    uint64_t *out_music_root_size)
{
    if (out_total_bytes == nullptr || out_free_bytes == nullptr || out_music_root_mtime == nullptr ||
        out_music_root_ctime == nullptr || out_music_root_size == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
    if (!sd_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }

    uint64_t total_bytes = 0U;
    uint64_t free_bytes = 0U;
    const esp_err_t info_ret = esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes);
    if (info_ret != ESP_OK) {
        return info_ret;
    }

    struct stat root_info = {};
    if (stat(MUSIC_ROOT, &root_info) != 0 || !S_ISDIR(root_info.st_mode)) {
        return ESP_FAIL;
    }

    *out_total_bytes = total_bytes;
    *out_free_bytes = free_bytes;
    *out_music_root_mtime = static_cast<int64_t>(root_info.st_mtime);
    *out_music_root_ctime = static_cast<int64_t>(root_info.st_ctime);
    *out_music_root_size = static_cast<uint64_t>(root_info.st_size);
    return ESP_OK;
}

static uint32_t media_library_quick_stamp_checksum(const MediaLibraryQuickStamp *stamp)
{
    if (stamp == nullptr) return 0U;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(stamp);
    const size_t length = offsetof(MediaLibraryQuickStamp, checksum);
    uint32_t hash = 2166136261U;
    for (size_t i = 0U; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static bool media_library_quick_stamp_matches(
    const MediaCatalogSnapshotV2 *snapshot,
    const char *source_label)
{
    if (snapshot == nullptr || snapshot->source != MediaCatalogLoadSourceV2::Final ||
        snapshot->catalog.track_count == 0U) {
        return false;
    }

    MediaLibraryQuickStamp stamp = {};
    {
        StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
        if (!sd_lock.locked()) {
            return false;
        }
        FILE *file = fopen(SystemPaths::kMusicQuickStamp, "rb");
        if (file == nullptr) {
            return false;
        }
        const size_t read_bytes = fread(&stamp, 1U, sizeof(stamp), file);
        fclose(file);
        if (read_bytes != sizeof(stamp)) {
            return false;
        }
    }

    if (stamp.magic != LIBRARY_QUICK_STAMP_MAGIC ||
        stamp.version != LIBRARY_QUICK_STAMP_VERSION ||
        stamp.struct_size != sizeof(MediaLibraryQuickStamp) ||
        stamp.index_crc32 != snapshot->index_crc32 ||
        stamp.track_count != snapshot->catalog.track_count ||
        stamp.checksum != media_library_quick_stamp_checksum(&stamp)) {
        return false;
    }

    const int64_t check_started_us = esp_timer_get_time();
    uint64_t total_bytes = 0U;
    uint64_t free_bytes = 0U;
    int64_t music_root_mtime = 0;
    int64_t music_root_ctime = 0;
    uint64_t music_root_size = 0U;
    if (media_library_query_quick_stamp_inputs(
            &total_bytes, &free_bytes, &music_root_mtime,
            &music_root_ctime, &music_root_size) != ESP_OK) {
        return false;
    }

    const bool matches = stamp.total_bytes == total_bytes &&
        stamp.free_bytes == free_bytes &&
        stamp.music_root_mtime == music_root_mtime &&
        stamp.music_root_ctime == music_root_ctime &&
        stamp.music_root_size == music_root_size;
    ESP_LOGI(TAG, "曲库快速变更戳：来源=%s 结果=%s 耗时=%u ms free=%llu",
        source_label != nullptr ? source_label : "?",
        matches ? "命中" : "变化",
        static_cast<unsigned>((esp_timer_get_time() - check_started_us) / 1000LL),
        static_cast<unsigned long long>(free_bytes));
    return matches;
}

static esp_err_t media_library_write_quick_stamp(uint32_t index_crc32, uint32_t track_count)
{
    // 首次创建 stamp 文件本身可能分配一个 FAT cluster。先确保文件已经存在，
    // 再查询 free_bytes，避免把“创建 stamp 消耗的空间”误判成下一次介质变化。
    {
        StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
        if (!sd_lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        struct stat stamp_info = {};
        if (stat(SystemPaths::kMusicQuickStamp, &stamp_info) != 0) {
            FILE *seed = fopen(SystemPaths::kMusicQuickStamp, "wb");
            if (seed == nullptr) {
                return ESP_FAIL;
            }
            MediaLibraryQuickStamp empty_stamp = {};
            empty_stamp.struct_size = static_cast<uint16_t>(sizeof(MediaLibraryQuickStamp));
            const size_t written = fwrite(&empty_stamp, 1U, sizeof(empty_stamp), seed);
            fclose(seed);
            if (written != sizeof(empty_stamp)) {
                return ESP_FAIL;
            }
        }
    }

    MediaLibraryQuickStamp stamp = {};
    stamp.struct_size = static_cast<uint16_t>(sizeof(MediaLibraryQuickStamp));
    stamp.index_crc32 = index_crc32;
    stamp.track_count = track_count;
    const esp_err_t query_ret = media_library_query_quick_stamp_inputs(
        &stamp.total_bytes, &stamp.free_bytes, &stamp.music_root_mtime,
        &stamp.music_root_ctime, &stamp.music_root_size);
    if (query_ret != ESP_OK) {
        return query_ret;
    }
    stamp.checksum = media_library_quick_stamp_checksum(&stamp);

    StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
    if (!sd_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    FILE *file = fopen(SystemPaths::kMusicQuickStamp, "wb");
    if (file == nullptr) {
        return ESP_FAIL;
    }
    const size_t written = fwrite(&stamp, 1U, sizeof(stamp), file);
    fclose(file);
    return written == sizeof(stamp) ? ESP_OK : ESP_FAIL;
}

// Stage 12.0.1：扫描事务会嵌套 Catalog 写盘/回读校验。
// 这些对象生命周期长、体积明显大于普通控制变量，不能继续压在 ESP-IDF main task 栈上。
// 扫描仍然逐文件执行；这里只把事务状态/可复用 scratch 放到 PSRAM。
static void directory_stack_destroy(DirectoryStack *stack);
static void directory_lrc_index_release(DirectoryLrcIndex *index);

struct MediaLibraryScanScratch
{
    MediaCatalogSnapshotV2 previous_v2 = {};
    MediaIndexSnapshot previous_v1 = {};
    DirectoryStack directory_stack = {};
    DirectoryLrcIndex directory_lrc_index = {};
    MediaArtworkBuildV2 directory_cover = {};
    struct stat file_info = {};
    MediaTechnicalInfo technical = {};
    MusicCatalogV2 next_catalog = {};
    char *scan_path = nullptr;
    size_t scan_path_capacity = 0U;
};

static void media_library_scan_scratch_release(MediaLibraryScanScratch *scratch)
{
    if (scratch == nullptr) {
        return;
    }
    media_artwork_build_release_v2(&scratch->directory_cover);
    directory_stack_destroy(&scratch->directory_stack);
    directory_lrc_index_release(&scratch->directory_lrc_index);
    media_catalog_store_v2_release(&scratch->previous_v2);
    media_index_store_release(&scratch->previous_v1);
    media_catalog_v2_release(&scratch->next_catalog);
    heap_caps_free(scratch->scan_path);
    scratch->scan_path = nullptr;
    scratch->scan_path_capacity = 0U;
    heap_caps_free(scratch);
}

static MediaEntry *g_entries = nullptr;
static size_t g_entry_count = 0;
static size_t g_entry_capacity = 0;
static char *g_path_pool = nullptr;
static size_t g_path_size = 0;
static size_t g_path_capacity = 0;
static bool g_ready = false;

static void *media_psram_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static char *media_psram_strdup(const char *text)
{
    if (text == nullptr) {
        return nullptr;
    }
    const size_t length = strlen(text) + 1;
    char *copy = static_cast<char *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy != nullptr) {
        memcpy(copy, text, length);
    }
    return copy;
}

static void media_library_release_build_buffers()
{
    if (g_entries != nullptr) {
        for (size_t i = 0; i < g_entry_count; ++i) {
            if (g_entries[i].metadata_build != nullptr) {
                media_metadata_build_release(g_entries[i].metadata_build);
                heap_caps_free(g_entries[i].metadata_build);
                g_entries[i].metadata_build = nullptr;
            }
            if (g_entries[i].artwork_build != nullptr) {
                media_artwork_build_release_v2(g_entries[i].artwork_build);
                heap_caps_free(g_entries[i].artwork_build);
                g_entries[i].artwork_build = nullptr;
            }
        }
        heap_caps_free(g_entries);
        g_entries = nullptr;
    }
    if (g_path_pool != nullptr) {
        heap_caps_free(g_path_pool);
        g_path_pool = nullptr;
    }
    g_entry_count = 0;
    g_entry_capacity = 0;
    g_path_size = 0;
    g_path_capacity = 0;
}

static void media_library_reset_build_state(bool preserve_runtime_ready)
{
    media_library_release_build_buffers();
    if (!preserve_runtime_ready) {
        g_ready = false;
    }
}

static bool media_library_ensure_entry_capacity(size_t required)
{
    if (required <= g_entry_capacity) {
        return true;
    }
    size_t next_capacity = g_entry_capacity == 0 ? INITIAL_ENTRY_CAPACITY : g_entry_capacity;
    while (next_capacity < required) {
        next_capacity *= 2;
    }
    void *next = media_psram_realloc(g_entries, next_capacity * sizeof(MediaEntry));
    if (next == nullptr) {
        return false;
    }
    g_entries = static_cast<MediaEntry *>(next);
    g_entry_capacity = next_capacity;
    return true;
}

static bool media_library_ensure_path_capacity(size_t required)
{
    if (required <= g_path_capacity) {
        return true;
    }
    size_t next_capacity = g_path_capacity == 0 ? INITIAL_PATH_CAPACITY : g_path_capacity;
    while (next_capacity < required) {
        next_capacity *= 2;
    }
    void *next = media_psram_realloc(g_path_pool, next_capacity);
    if (next == nullptr) {
        return false;
    }
    g_path_pool = static_cast<char *>(next);
    g_path_capacity = next_capacity;
    return true;
}

static bool media_library_add_track(
    const char *path,
    MediaFormat format,
    const struct stat &file_info,
    const MediaTechnicalInfo &technical,
    MediaMetadataBuildV2 *metadata_build,
    MediaArtworkBuildV2 *artwork_build
)
{
    if (path == nullptr) {
        return false;
    }
    const size_t path_length = strlen(path) + 1;
    if (!media_library_ensure_entry_capacity(g_entry_count + 1)) {
        return false;
    }
    if (!media_library_ensure_path_capacity(g_path_size + path_length)) {
        return false;
    }
    const size_t offset = g_path_size;
    memcpy(g_path_pool + offset, path, path_length);
    g_path_size += path_length;

    MediaEntry &entry = g_entries[g_entry_count];
    entry = {};
    entry.path_offset = static_cast<uint32_t>(offset);
    entry.format = format;
    entry.file_size_bytes = static_cast<uint64_t>(file_info.st_size);
    entry.modified_time = static_cast<int64_t>(file_info.st_mtime);
    entry.technical = technical;
    entry.metadata_build = metadata_build;
    entry.artwork_build = artwork_build;
    g_entry_count++;
    return true;
}

static void media_library_drop_metadata(MediaEntry *entry)
{
    if (entry == nullptr || entry->metadata_build == nullptr) {
        return;
    }
    media_metadata_build_release(entry->metadata_build);
    heap_caps_free(entry->metadata_build);
    entry->metadata_build = nullptr;
}

static void media_library_drop_artwork(MediaEntry *entry)
{
    if (entry == nullptr || entry->artwork_build == nullptr) {
        return;
    }
    media_artwork_build_release_v2(entry->artwork_build);
    heap_caps_free(entry->artwork_build);
    entry->artwork_build = nullptr;
}

static void media_library_drop_lyrics(MediaMetadataBuildV2 *metadata)
{
    if (metadata == nullptr || metadata->lyrics == nullptr) {
        return;
    }
    for (uint16_t i = 0; i < metadata->lyrics_count; ++i) {
        heap_caps_free(metadata->lyrics[i].path);
        heap_caps_free(metadata->lyrics[i].language);
    }
    heap_caps_free(metadata->lyrics);
    metadata->lyrics = nullptr;
    metadata->lyrics_count = 0U;
    metadata->lyrics_capacity = 0U;
}

static bool media_library_is_track_catalog_error(esp_err_t error)
{
    return error == ESP_ERR_INVALID_RESPONSE || error == ESP_ERR_INVALID_SIZE;
}

static esp_err_t media_library_validate_single_track(size_t index)
{
    MusicCatalogV2 single = {};
    const esp_err_t ret = media_catalog_v2_build_from_index_records(
        &g_entries[index], 1U, g_path_pool, g_path_size, &single);
    media_catalog_v2_release(&single);
    return ret;
}

static bool directory_stack_push(DirectoryStack *stack, const char *path, uint8_t depth)
{
    if (stack == nullptr || path == nullptr) {
        return false;
    }
    if (stack->count == stack->capacity) {
        size_t next_capacity = stack->capacity == 0 ? INITIAL_DIR_CAPACITY : stack->capacity * 2;
        void *next = media_psram_realloc(stack->items, next_capacity * sizeof(DirectoryStackItem));
        if (next == nullptr) {
            return false;
        }
        stack->items = static_cast<DirectoryStackItem *>(next);
        stack->capacity = next_capacity;
    }
    char *copy = media_psram_strdup(path);
    if (copy == nullptr) {
        return false;
    }
    DirectoryStackItem &item = stack->items[stack->count++];
    item.path = copy;
    item.depth = depth;
    return true;
}

static DirectoryStackItem directory_stack_pop(DirectoryStack *stack)
{
    if (stack == nullptr || stack->count == 0) {
        return {};
    }
    DirectoryStackItem item = stack->items[--stack->count];
    stack->items[stack->count] = {};
    return item;
}

static void directory_stack_destroy(DirectoryStack *stack)
{
    if (stack == nullptr) {
        return;
    }
    while (stack->count > 0) {
        DirectoryStackItem item = directory_stack_pop(stack);
        heap_caps_free(item.path);
    }
    if (stack->items != nullptr) {
        heap_caps_free(stack->items);
    }
    stack->items = nullptr;
    stack->count = 0;
    stack->capacity = 0;
}

static void directory_lrc_index_release(DirectoryLrcIndex *index)
{
    if (index == nullptr) {
        return;
    }
    for (size_t i = 0; i < index->count; ++i) {
        heap_caps_free(index->names[i]);
    }
    heap_caps_free(index->names);
    index->names = nullptr;
    index->count = 0U;
    index->capacity = 0U;
}

static bool directory_lrc_index_add(DirectoryLrcIndex *index, const char *name)
{
    if (index == nullptr || name == nullptr) {
        return false;
    }
    if (index->count == index->capacity) {
        const size_t next_capacity = index->capacity == 0U ? 16U : index->capacity * 2U;
        void *next = media_psram_realloc(index->names, next_capacity * sizeof(char *));
        if (next == nullptr) {
            return false;
        }
        index->names = static_cast<char **>(next);
        index->capacity = next_capacity;
    }
    char *copy = media_psram_strdup(name);
    if (copy == nullptr) {
        return false;
    }
    index->names[index->count++] = copy;
    return true;
}

static bool media_library_name_has_extension(const char *name, const char *extension)
{
    if (name == nullptr || extension == nullptr) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    return dot != nullptr && strcasecmp(dot, extension) == 0;
}

static esp_err_t directory_lrc_index_build(DIR *dir, DirectoryLrcIndex *index)
{
    if (dir == nullptr || index == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    directory_lrc_index_release(index);
    rewinddir(dir);
    while (true) {
        struct dirent *entry = readdir(dir);
        if (entry == nullptr) {
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (media_library_name_has_extension(entry->d_name, ".lrc") &&
            !directory_lrc_index_add(index, entry->d_name)) {
            directory_lrc_index_release(index);
            rewinddir(dir);
            return ESP_ERR_NO_MEM;
        }
    }
    rewinddir(dir);
    return ESP_OK;
}

static const char *directory_lrc_index_find_for_audio(
    const DirectoryLrcIndex *index,
    const char *audio_name
)
{
    if (index == nullptr || audio_name == nullptr) {
        return nullptr;
    }
    const char *audio_dot = strrchr(audio_name, '.');
    if (audio_dot == nullptr) {
        return nullptr;
    }
    const size_t audio_stem_len = static_cast<size_t>(audio_dot - audio_name);
    for (size_t i = 0; i < index->count; ++i) {
        const char *lrc_name = index->names[i];
        const char *lrc_dot = lrc_name != nullptr ? strrchr(lrc_name, '.') : nullptr;
        if (lrc_dot == nullptr || strcasecmp(lrc_dot, ".lrc") != 0) {
            continue;
        }
        const size_t lrc_stem_len = static_cast<size_t>(lrc_dot - lrc_name);
        if (lrc_stem_len == audio_stem_len && strncasecmp(lrc_name, audio_name, audio_stem_len) == 0) {
            return lrc_name;
        }
    }
    return nullptr;
}

static char *media_library_alloc_join_path(const char *directory, const char *name)
{
    if (directory == nullptr || name == nullptr) {
        return nullptr;
    }
    const size_t dir_length = strlen(directory);
    const size_t name_length = strlen(name);
    const bool need_slash = dir_length > 0U && directory[dir_length - 1U] != '/';
    const size_t total = dir_length + (need_slash ? 1U : 0U) + name_length + 1U;
    char *path = static_cast<char *>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (path == nullptr) {
        return nullptr;
    }
    snprintf(path, total, "%s%s%s", directory, need_slash ? "/" : "", name);
    return path;
}

static char *media_library_build_scan_path(
    MediaLibraryScanScratch *scratch,
    const char *directory,
    const char *name
)
{
    if (scratch == nullptr || directory == nullptr || name == nullptr) {
        return nullptr;
    }
    const size_t dir_length = strlen(directory);
    const size_t name_length = strlen(name);
    const bool need_slash = dir_length > 0 && directory[dir_length - 1] != '/';
    const size_t required = dir_length + (need_slash ? 1U : 0U) + name_length + 1U;
    if (required > scratch->scan_path_capacity) {
        size_t next_capacity = scratch->scan_path_capacity == 0U ? 256U : scratch->scan_path_capacity;
        while (next_capacity < required) {
            next_capacity *= 2U;
        }
        void *next = media_psram_realloc(scratch->scan_path, next_capacity);
        if (next == nullptr) {
            return nullptr;
        }
        scratch->scan_path = static_cast<char *>(next);
        scratch->scan_path_capacity = next_capacity;
    }
    memcpy(scratch->scan_path, directory, dir_length);
    size_t cursor = dir_length;
    if (need_slash) {
        scratch->scan_path[cursor++] = '/';
    }
    memcpy(scratch->scan_path + cursor, name, name_length + 1U);
    return scratch->scan_path;
}

static bool media_library_detect_format(const char *name, MediaFormat *format)
{
    if (name == nullptr || format == nullptr) {
        return false;
    }
    const char *extension = strrchr(name, '.');
    if (extension == nullptr) {
        return false;
    }
    if (strcasecmp(extension, ".mp3") == 0) {
        *format = MediaFormat::MP3;
    } else if (strcasecmp(extension, ".flac") == 0) {
        *format = MediaFormat::FLAC;
    } else if (strcasecmp(extension, ".wav") == 0) {
        *format = MediaFormat::WAV;
    } else if (strcasecmp(extension, ".nsf") == 0) {
        *format = MediaFormat::NSF;
    } else if (strcasecmp(extension, ".nsfe") == 0) {
        *format = MediaFormat::NSFE;
    } else if (strcasecmp(extension, ".opus") == 0) {
        *format = MediaFormat::OPUS;
    } else {
        return false;
    }
    return true;
}

static int media_library_compare_entries(const void *left, const void *right)
{
    if (left == nullptr || right == nullptr || g_path_pool == nullptr) {
        return 0;
    }
    const MediaEntry *a = static_cast<const MediaEntry *>(left);
    const MediaEntry *b = static_cast<const MediaEntry *>(right);
    const char *path_a = g_path_pool + a->path_offset;
    const char *path_b = g_path_pool + b->path_offset;
    return strcasecmp(path_a, path_b);
}

static void media_library_sort_entries()
{
    if (g_entry_count < 2 || g_entries == nullptr || g_path_pool == nullptr) {
        return;
    }
    qsort(g_entries, g_entry_count, sizeof(MediaEntry), media_library_compare_entries);
    LIB_BOOT_LOGI("排序完成：按完整 UTF-8 路径排序，共 %u 首", static_cast<unsigned>(g_entry_count));
}

static bool media_library_repack_sorted_path_pool()
{
    if (g_entry_count == 0) {
        return true;
    }
    if (g_entries == nullptr || g_path_pool == nullptr || g_path_size == 0) {
        return false;
    }

    // readdir 顺序不保证跨启动稳定。排序完成后按最终 Track 顺序重新打包路径池，
    // 让 path_offset 与二进制 payload CRC 都具有确定性，未变化曲库才能真正跳过写盘。
    char *sorted_pool = static_cast<char *>(
        heap_caps_malloc(g_path_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (sorted_pool == nullptr) {
        return false;
    }

    size_t next_offset = 0;
    for (size_t i = 0; i < g_entry_count; ++i) {
        const char *path = g_path_pool + g_entries[i].path_offset;
        const size_t length = strlen(path) + 1;
        if (next_offset + length > g_path_size) {
            heap_caps_free(sorted_pool);
            return false;
        }
        memcpy(sorted_pool + next_offset, path, length);
        g_entries[i].path_offset = static_cast<uint32_t>(next_offset);
        next_offset += length;
    }

    heap_caps_free(g_path_pool);
    g_path_pool = sorted_pool;
    g_path_size = next_offset;
    g_path_capacity = next_offset;
    return true;
}

static esp_err_t media_library_scan_with_scratch(
    MediaLibraryScanScratch *scratch,
    bool replace_runtime_catalog,
    MusicCatalogV2 *out_retired,
    MediaLibraryChangeSummary *out_changes,
    MediaLibraryScanEventCallback on_scan_event,
    void *callback_context)
{
    if (scratch == nullptr || (replace_runtime_catalog && out_retired == nullptr)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_changes != nullptr) {
        *out_changes = {};
    }
    media_library_reset_build_state(replace_runtime_catalog);
    LIB_BOOT_LOGI("开始递归扫描：%s", MUSIC_ROOT);
    const int64_t start_us = esp_timer_get_time();

    // Stage 10.2 优先加载正式 V2 Catalog/Manifest。第一次从 Stage 10.0 升级时，
    // 若 V2 尚不存在则只把 V1 当作迁移源复用技术信息，之后不再写回 V1。
    MediaCatalogSnapshotV2 &previous_v2 = scratch->previous_v2;
    MediaIndexSnapshot &previous_v1 = scratch->previous_v1;
    DirectoryStack &stack = scratch->directory_stack;
    DirectoryLrcIndex &directory_lrc_index = scratch->directory_lrc_index;
    MediaArtworkBuildV2 &directory_cover = scratch->directory_cover;
    struct stat &info = scratch->file_info;
    MediaTechnicalInfo &technical = scratch->technical;
    MusicCatalogV2 &next_catalog = scratch->next_catalog;

    const bool have_previous_v2 = media_catalog_store_v2_load(&previous_v2) == ESP_OK;
    const bool have_previous_v1 = !have_previous_v2 && media_index_store_load(&previous_v1) == ESP_OK;
    // 正常开机的已有 V2 曲库以“新增/删除/改名发现”为主：目录项里已经确认旧路径仍存在时，
    // 直接复用旧 Manifest 的 size/mtime，不再对上千首旧歌逐个 stat(full_path)。
    // USB MSC 归还属于严格热刷新，仍逐首校验 size+mtime，从而保留同名覆盖/修改检测能力。
    // 首次建库没有 previous_v2，因此完全不受这条快路径影响。
    const bool fast_boot_incremental = have_previous_v2 && !replace_runtime_catalog;

    // 已有正式 V2 Catalog 时，正常开机和 USB MSC 归还都先检查轻量变更戳。
    // 命中时完全跳过 /MUSIC 千文件目录枚举：
    // - 正常开机需要把磁盘 V2 Catalog 发布为运行时 Catalog；
    // - USB 归还时旧运行时 Catalog 本来就有效，因此无需替换 generation，只返回“无变化”。
    const bool quick_stamp_eligible =
        have_previous_v2 && previous_v2.source == MediaCatalogLoadSourceV2::Final;
    if (quick_stamp_eligible && media_library_quick_stamp_matches(
            &previous_v2, replace_runtime_catalog ? "USB归还" : "启动")) {
        const uint32_t final_track_count = previous_v2.catalog.track_count;
        if (out_changes != nullptr) {
            out_changes->previous_count = final_track_count;
            out_changes->current_count = final_track_count;
            out_changes->had_previous_catalog = true;
            out_changes->changed = false;
        }

        if (!replace_runtime_catalog) {
            next_catalog = previous_v2.catalog;
            previous_v2.catalog = {};
            const uint32_t source_crc = previous_v2.index_crc32;
            const esp_err_t publish_ret = media_catalog_v2_publish(&next_catalog, source_crc);
            if (publish_ret != ESP_OK) {
                media_catalog_v2_release(&next_catalog);
                media_catalog_store_v2_release(&previous_v2);
                media_index_store_release(&previous_v1);
                media_library_reset_build_state(replace_runtime_catalog);
                return publish_ret;
            }
            g_ready = true;
        }

        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_release_build_buffers();
        const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000LL);
        ESP_LOGI(TAG, "%s快速变更戳命中：跳过/MUSIC目录枚举，直接复用V2 Catalog tracks=%u 耗时=%u ms",
            replace_runtime_catalog ? "USB归还" : "启动",
            static_cast<unsigned>(final_track_count),
            static_cast<unsigned>(elapsed_ms));
        return ESP_OK;
    }
    const size_t previous_track_count =
        have_previous_v2 ? static_cast<size_t>(previous_v2.catalog.track_count) : 0U;
    size_t existing_path_count = 0U;
    size_t added_count = 0U;
    size_t updated_count = 0U;
    size_t issue_count = 0U;
    size_t skipped_track_count = 0U;
    char first_issue_file[96] = {};
    char first_issue_reason[96] = {};
    const auto record_scan_issue = [&](const char *path, const char *reason) {
        issue_count++;
        if (first_issue_file[0] == '\0') {
            const char *name = path != nullptr ? strrchr(path, '/') : nullptr;
            snprintf(first_issue_file, sizeof(first_issue_file), "%s",
                name != nullptr ? name + 1 : (path != nullptr ? path : "未知文件"));
            snprintf(first_issue_reason, sizeof(first_issue_reason), "%s",
                reason != nullptr ? reason : "媒体文件异常，已自动降级处理");
        }
        ESP_LOGW(TAG, "曲库单曲异常：%s，%s",
            path != nullptr ? path : "<unknown>", reason != nullptr ? reason : "未知原因");
    };
    bool changes_notified = false;
    const auto notify_changes_detected = [&]() {
        if (!changes_notified && on_scan_event != nullptr) {
            changes_notified = true;
            on_scan_event(
                MediaLibraryScanEvent::ChangesDetected,
                static_cast<uint32_t>(g_entry_count),
                callback_context);
        }
    };
    if (!have_previous_v2 && on_scan_event != nullptr) {
        on_scan_event(MediaLibraryScanEvent::InitialBuild, 0U, callback_context);
    }
    if (have_previous_v1) {
        ESP_LOGI(TAG, "检测到旧 V1 索引，本次迁移复用技术信息并生成 V2 Catalog");
    } else if (!have_previous_v2) {
        ESP_LOGI(TAG, "未找到可复用索引，本次将首次建立 MusicCatalogV2");
    }

    if (!directory_stack_push(&stack, MUSIC_ROOT, 0U)) {
        ESP_LOGE(TAG, "创建目录扫描栈失败");
        directory_stack_destroy(&stack);
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset_build_state(replace_runtime_catalog);
        return ESP_ERR_NO_MEM;
    }

    size_t directory_count = 0;
    size_t format_count[static_cast<size_t>(MediaFormat::OPUS) + 1U] = {};
    size_t reused_count = 0;
    size_t probed_count = 0;
    size_t probe_failed_count = 0;
    size_t metadata_reused_count = 0;
    size_t metadata_scanned_count = 0;
    size_t metadata_failed_count = 0;
    size_t artwork_reused_count = 0;
    size_t artwork_refreshed_count = 0;
    size_t artwork_scanned_count = 0;
    size_t artwork_failed_count = 0;
    size_t artwork_embedded_count = 0;
    size_t artwork_external_count = 0;
    size_t artwork_none_count = 0;
    size_t full_track_reused_count = 0;
    size_t fast_stat_skipped_count = 0U;
    size_t strict_stat_count = 0U;
    size_t deferred_metadata_clone_count = 0U;
    size_t deferred_artwork_clone_count = 0U;
    size_t hydrated_metadata_clone_count = 0U;
    size_t hydrated_artwork_clone_count = 0U;
    bool out_of_memory = false;
    bool storage_timeout = false;
    bool root_missing = false;
    bool scan_io_error = false;
    int scan_io_errno = 0;
    char scan_io_stage[24] = {};
    char scan_io_path[128] = {};
    const auto record_scan_io_error = [&](const char *stage, const char *path, int error_code) {
        if (!scan_io_error) {
            scan_io_errno = error_code;
            snprintf(scan_io_stage, sizeof(scan_io_stage), "%s", stage != nullptr ? stage : "I/O");
            snprintf(scan_io_path, sizeof(scan_io_path), "%s", path != nullptr ? path : "");
        }
        scan_io_error = true;
    };

    // 首次建库时给启动页持续反馈“已经发现多少首音乐”。
    // 不能每发现一首就抢一次 LVGL 锁，否则大曲库会把扫描本身拖慢；
    // 这里按“至少 5 首”或“至少 250ms”节流，同时保证第 1 首立即可见。
    size_t last_initial_progress_count = 0U;
    int64_t last_initial_progress_us = start_us;
    const auto notify_initial_build_progress = [&](bool force) {
        if (have_previous_v2 || on_scan_event == nullptr || g_entry_count == 0U) {
            return;
        }

        const int64_t now_us = esp_timer_get_time();
        const bool first_track = last_initial_progress_count == 0U;
        const bool count_due =
            g_entry_count >= last_initial_progress_count + 5U;
        const bool time_due =
            now_us - last_initial_progress_us >= 250000LL;
        if (!force && !first_track && !count_due && !time_due) {
            return;
        }

        last_initial_progress_count = g_entry_count;
        last_initial_progress_us = now_us;
        on_scan_event(
            MediaLibraryScanEvent::InitialBuild,
            static_cast<uint32_t>(g_entry_count),
            callback_context);
    };

    // 已有 V2 Catalog 时，只对“真正新增歌曲”发布增量数量。
    // 和首次建库一样做节流，避免 USB 归还任务或启动扫描线程频繁进入 UI 回调。
    // 删除/文件更新仍由 ChangesDetected 提供通用提示，最终数量统一由 ChangeSummary 汇总。
    size_t last_incremental_added_progress_count = 0U;
    int64_t last_incremental_added_progress_us = start_us;
    const auto notify_incremental_added_progress = [&](bool force) {
        if (!have_previous_v2 || on_scan_event == nullptr || added_count == 0U) {
            return;
        }

        const int64_t now_us = esp_timer_get_time();
        const bool first_added = last_incremental_added_progress_count == 0U;
        const bool count_due =
            added_count >= last_incremental_added_progress_count + 5U;
        const bool time_due =
            now_us - last_incremental_added_progress_us >= 250000LL;
        if (!force && !first_added && !count_due && !time_due) {
            return;
        }

        last_incremental_added_progress_count = added_count;
        last_incremental_added_progress_us = now_us;
        on_scan_event(
            MediaLibraryScanEvent::IncrementalAddedProgress,
            static_cast<uint32_t>(added_count),
            callback_context);
    };

    const auto log_slow_scan_stage = [&](const char *stage, const char *path, int64_t started_us) {
        const int64_t elapsed_us = esp_timer_get_time() - started_us;
        if (elapsed_us >= SLOW_SCAN_STAGE_WARN_US) {
            ESP_LOGW(TAG, "建库单文件阶段耗时过长：stage=%s elapsed=%lldms path=%s",
                stage != nullptr ? stage : "?",
                static_cast<long long>(elapsed_us / 1000LL),
                path != nullptr ? path : "");
        }
    };

    while (stack.count > 0 && !out_of_memory && !storage_timeout && !scan_io_error) {
        DirectoryStackItem directory_item = directory_stack_pop(&stack);
        char *directory = directory_item.path;
        if (directory == nullptr) {
            break;
        }
        DIR *dir = nullptr;
        int directory_errno = 0;
        {
            StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
            if (!sd_lock.locked()) {
                heap_caps_free(directory);
                directory_stack_destroy(&stack);
                media_catalog_store_v2_release(&previous_v2);
                media_index_store_release(&previous_v1);
                media_library_reset_build_state(replace_runtime_catalog);
                return ESP_ERR_TIMEOUT;
            }
            errno = 0;
            dir = opendir(directory);
            if (dir == nullptr) {
                directory_errno = errno;
            }
        }
        if (dir == nullptr) {
            if (media_library_errno_is_missing(directory_errno)) {
                if (strcmp(directory, MUSIC_ROOT) == 0) {
                    root_missing = true;
                }
                heap_caps_free(directory);
                continue;
            }
            record_scan_io_error("opendir", directory, directory_errno);
            heap_caps_free(directory);
            break;
        }
        directory_count++;

        // 目录级 .lrc 名字索引只用于首次建库。
        // 已有 V2 的增量扫描通常只有少量新增/变化歌曲；如果为了它们先把千首大目录
        // 完整 readdir 一遍建立歌词索引，反而会让“全量无变化”启动多出一次目录遍历。
        // 因此 V2 增量沿用按需歌词探测，只对真正需要重扫 Metadata 的少量歌曲付出成本。
        directory_lrc_index_release(&directory_lrc_index);
        if (!have_previous_v2 && !have_previous_v1) {
            StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
            if (!sd_lock.locked()) {
                storage_timeout = true;
            } else {
                const esp_err_t lrc_index_ret = directory_lrc_index_build(dir, &directory_lrc_index);
                if (lrc_index_ret == ESP_ERR_NO_MEM) {
                    out_of_memory = true;
                }
            }
        }
        if (storage_timeout || out_of_memory) {
            StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
            if (sd_lock.locked()) {
                closedir(dir);
            }
            heap_caps_free(directory);
            break;
        }

        // 每个目录只探测一次 fallback，避免每首歌重复 stat/打开 cover.*。
        // directory_cover 使用 PSRAM scratch，避免把 locator 生命周期压在 main task 栈上。
        media_artwork_build_release_v2(&directory_cover);
        const esp_err_t directory_cover_ret = media_artwork_find_directory_fallback_v2(directory, &directory_cover);
        if (directory_cover_ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "目录封面探测等待TF锁超时，本目录按无fallback继续：%s", directory);
            media_artwork_build_release_v2(&directory_cover);
        } else if (directory_cover_ret == ESP_ERR_NO_MEM) {
            out_of_memory = true;
            {
                StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
                if (sd_lock.locked()) {
                    closedir(dir);
                }
            }
            heap_caps_free(directory);
            break;
        }
        struct dirent *entry = nullptr;
        while (true) {
            int readdir_errno = 0;
            {
                StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
                if (!sd_lock.locked()) {
                    storage_timeout = true;
                    break;
                }
                errno = 0;
                entry = readdir(dir);
                if (entry == nullptr) {
                    readdir_errno = errno;
                }
            }
            if (entry == nullptr) {
                if (readdir_errno != 0) {
                    record_scan_io_error("readdir", directory, readdir_errno);
                }
                break;
            }
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            // FatFs/VFS 已经在 dirent.d_type 标出普通文件时，先按扩展名快速过滤。
            // 这样 .lrc、jpg、文本等非媒体文件不会再逐个 stat(full_path)，
            // 对单目录上千文件的场景可以避免大量重复 FAT 名字查找；DT_UNKNOWN 时仍走后续 stat 兜底。
#if defined(DT_REG)
            if (entry->d_type == DT_REG) {
                MediaFormat quick_format = MediaFormat::Unknown;
                if (!media_library_detect_format(entry->d_name, &quick_format)) {
                    continue;
                }
            }
#endif
            char *full_path = media_library_build_scan_path(scratch, directory, entry->d_name);
            if (full_path == nullptr) {
                out_of_memory = true;
                break;
            }

#if defined(DT_DIR)
            // FatFs 已经明确告诉我们这是目录时，不需要为了确认 S_ISDIR 再做一次路径 stat。
            // 目录深度规则仍保持 /MUSIC 下最多两级。
            if (entry->d_type == DT_DIR) {
                if (directory_item.depth < MAX_MUSIC_SUBDIR_DEPTH) {
                    if (!directory_stack_push(&stack, full_path, static_cast<uint8_t>(directory_item.depth + 1U))) {
                        out_of_memory = true;
                    }
                }
                if (out_of_memory) {
                    break;
                }
                continue;
            }
#endif

            info = {};
            bool stat_ok = false;

            // 启动增量快扫：对于 readdir 已确认的普通媒体文件，先按路径查旧 V2 Manifest。
            // 旧路径仍存在且格式一致时，说明它不是“新增/删除/改名”对象；直接沿用旧 size/mtime，
            // 从而把 1000 首无变化曲库从“1000 次 FAT stat”降为“一次目录遍历 + 内存查找”。
            // 这条快路径刻意不识别“同名原地覆盖文件”；USB MSC 归还的严格热刷新仍会逐首 stat。
#if defined(DT_REG)
            if (fast_boot_incremental && entry->d_type == DT_REG) {
                MediaFormat fast_format = MediaFormat::Unknown;
                if (media_library_detect_format(entry->d_name, &fast_format)) {
                    const TrackRowV2 *fast_old_track = nullptr;
                    const MediaManifestRecordV2 *fast_old_manifest = nullptr;
                    const bool fast_old_found = media_catalog_store_v2_find(
                        &previous_v2, full_path, &fast_old_track, &fast_old_manifest);
                    if (fast_old_found && fast_old_track != nullptr && fast_old_manifest != nullptr &&
                        fast_old_track->format == fast_format && fast_old_manifest->format == fast_format) {
                        info.st_mode = S_IFREG;
                        info.st_size = static_cast<off_t>(fast_old_manifest->file_size_bytes);
                        info.st_mtime = static_cast<time_t>(fast_old_manifest->modified_time);
                        stat_ok = true;
                        fast_stat_skipped_count++;
                    }
                }
            }
#endif

            int stat_errno = 0;
            if (!stat_ok) {
                StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
                if (!sd_lock.locked()) {
                    storage_timeout = true;
                    break;
                }
                strict_stat_count++;
                errno = 0;
                stat_ok = stat(full_path, &info) == 0;
                if (!stat_ok) {
                    stat_errno = errno;
                }
            }
            if (!stat_ok) {
                if (media_library_errno_is_missing(stat_errno)) {
                    ESP_LOGW(TAG, "扫描期间文件已不存在，按删除处理：%s", full_path);
                    continue;
                }
                record_scan_io_error("stat", full_path, stat_errno);
                break;
            }
            if (S_ISDIR(info.st_mode)) {
                if (directory_item.depth < MAX_MUSIC_SUBDIR_DEPTH) {
                    if (!directory_stack_push(&stack, full_path, static_cast<uint8_t>(directory_item.depth + 1U))) {
                        out_of_memory = true;
                    }
                }
                if (out_of_memory) {
                    break;
                }
                continue;
            }
            if (S_ISREG(info.st_mode)) {
                MediaFormat format = MediaFormat::Unknown;
                if (media_library_detect_format(entry->d_name, &format)) {
                    technical = {};
                    MediaMetadataBuildV2 *metadata_build = nullptr;
                    MediaArtworkBuildV2 *artwork_build = nullptr;
                    bool reused = false;
                    bool metadata_reused = false;
                    bool artwork_reused = false;
                    bool artwork_unchanged = false;
                    bool v2_signature_match = false;
                    bool catalog_change_counted = false;
                    const TrackRowV2 *old_v2_track = nullptr;

                    const bool deep_probe_format =
                        format == MediaFormat::FLAC || format == MediaFormat::MP3 || format == MediaFormat::OPUS;

                    if (have_previous_v2) {
                        const MediaManifestRecordV2 *old_manifest = nullptr;
                        const bool old_v2_found = media_catalog_store_v2_find(
                            &previous_v2, full_path, &old_v2_track, &old_manifest);
                        if (old_v2_found && old_v2_track != nullptr) {
                            existing_path_count++;
                        }
                        if (old_v2_found && old_v2_track != nullptr && old_manifest != nullptr &&
                            old_v2_track->format == format && old_manifest->format == format &&
                            old_manifest->file_size_bytes == static_cast<uint64_t>(info.st_size) &&
                            old_manifest->modified_time == static_cast<int64_t>(info.st_mtime)) {
                            v2_signature_match = true;
                            if (!deep_probe_format || (old_v2_track->technical.flags & MEDIA_TECH_PARSED) != 0U) {
                                technical = old_v2_track->technical;
                                reused = true;
                                reused_count++;
                            }
                            const uint32_t old_track_index =
                                static_cast<uint32_t>(old_v2_track - previous_v2.catalog.tracks);

                            // 正常开机的 V2 增量快扫只需要证明“旧数据仍可复用”，
                            // 不要为每首未变化歌曲立刻 clone title/artist/album/lyrics。
                            // 无变化曲库最后会直接 move 旧 Catalog，这些临时对象创建后马上又会被释放。
                            // 真正发现新增/更新时，再在 Catalog 重建前统一补齐。
                            if (deep_probe_format &&
                                (old_v2_track->metadata_flags & MEDIA_TRACK_META_SCANNED_V2) != 0U) {
                                if (fast_boot_incremental) {
                                    metadata_reused = true;
                                    metadata_reused_count++;
                                    deferred_metadata_clone_count++;
                                } else {
                                    metadata_build = static_cast<MediaMetadataBuildV2 *>(
                                        heap_caps_calloc(1, sizeof(MediaMetadataBuildV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                    );
                                    if (metadata_build == nullptr) {
                                        out_of_memory = true;
                                    } else {
                                        const esp_err_t clone_ret = media_metadata_clone_from_catalog_v2(
                                            &previous_v2.catalog, old_track_index, metadata_build
                                        );
                                        if (clone_ret == ESP_OK) {
                                            metadata_reused = true;
                                            metadata_reused_count++;
                                        } else {
                                            media_metadata_build_release(metadata_build);
                                            heap_caps_free(metadata_build);
                                            metadata_build = nullptr;
                                            if (clone_ret == ESP_ERR_NO_MEM) {
                                                out_of_memory = true;
                                            }
                                        }
                                    }
                                }
                            }

                            if (!out_of_memory) {
                                if (fast_boot_incremental) {
                                    const esp_err_t artwork_check_ret = media_artwork_catalog_reuse_unchanged_v2(
                                        &previous_v2.catalog, old_track_index, &directory_cover, &artwork_unchanged
                                    );
                                    if (artwork_check_ret == ESP_OK && artwork_unchanged) {
                                        artwork_reused = true;
                                        artwork_reused_count++;
                                        if (old_v2_track->artwork_ref_id != MEDIA_CATALOG_INVALID_ID_V2) {
                                            deferred_artwork_clone_count++;
                                        }
                                    } else {
                                        // 目录 fallback 真正发生变化，或旧 locator 无法轻量校验时，
                                        // 仍沿用原来的 clone 路径，保证变化检测语义不变。
                                        artwork_build = static_cast<MediaArtworkBuildV2 *>(
                                            heap_caps_calloc(1, sizeof(MediaArtworkBuildV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                        );
                                        if (artwork_build == nullptr) {
                                            out_of_memory = true;
                                        } else {
                                            const esp_err_t artwork_clone_ret = media_artwork_clone_from_catalog_v2(
                                                &previous_v2.catalog, old_track_index, &directory_cover, artwork_build, &artwork_unchanged
                                            );
                                            if (artwork_clone_ret == ESP_OK) {
                                                artwork_reused = true;
                                                if (artwork_unchanged) {
                                                    artwork_reused_count++;
                                                } else {
                                                    artwork_refreshed_count++;
                                                }
                                            } else {
                                                media_artwork_build_release_v2(artwork_build);
                                                heap_caps_free(artwork_build);
                                                artwork_build = nullptr;
                                                if (artwork_clone_ret == ESP_ERR_NO_MEM) {
                                                    out_of_memory = true;
                                                }
                                            }
                                        }
                                    }
                                } else {
                                    artwork_build = static_cast<MediaArtworkBuildV2 *>(
                                        heap_caps_calloc(1, sizeof(MediaArtworkBuildV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                    );
                                    if (artwork_build == nullptr) {
                                        out_of_memory = true;
                                    } else {
                                        const esp_err_t artwork_clone_ret = media_artwork_clone_from_catalog_v2(
                                            &previous_v2.catalog, old_track_index, &directory_cover, artwork_build, &artwork_unchanged
                                        );
                                        if (artwork_clone_ret == ESP_OK) {
                                            artwork_reused = true;
                                            if (artwork_unchanged) {
                                                artwork_reused_count++;
                                            } else {
                                                artwork_refreshed_count++;
                                            }
                                        } else {
                                            media_artwork_build_release_v2(artwork_build);
                                            heap_caps_free(artwork_build);
                                            artwork_build = nullptr;
                                            if (artwork_clone_ret == ESP_ERR_NO_MEM) {
                                                out_of_memory = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    } else if (have_previous_v1) {
                        const MediaIndexRecord *old = media_index_store_find(&previous_v1, full_path);
                        if (old != nullptr && old->format == format &&
                            old->file_size_bytes == static_cast<uint64_t>(info.st_size) &&
                            old->modified_time == static_cast<int64_t>(info.st_mtime) &&
                            (!deep_probe_format || (old->technical.flags & MEDIA_TECH_PARSED) != 0U)) {
                            technical = old->technical;
                            reused = true;
                            reused_count++;
                        }
                    }

                    // 新增/文件签名变化在深度 probe 前就能确定，先通知启动页，避免用户只在扫描快结束时才看到提示。
                    if (have_previous_v2) {
                        if (old_v2_track == nullptr) {
                            added_count++;
                            catalog_change_counted = true;
                            notify_changes_detected();
                            notify_incremental_added_progress(false);
                        } else if (!v2_signature_match) {
                            updated_count++;
                            catalog_change_counted = true;
                            notify_changes_detected();
                        }
                    }

                    if (out_of_memory) {
                        if (metadata_build != nullptr) {
                            media_metadata_build_release(metadata_build);
                            heap_caps_free(metadata_build);
                            metadata_build = nullptr;
                        }
                        if (artwork_build != nullptr) {
                            media_artwork_build_release_v2(artwork_build);
                            heap_caps_free(artwork_build);
                            artwork_build = nullptr;
                        }
                        break;
                    }

                    // MP3/FLAC/Ogg Opus 只要本轮存在任意需要重新解析的阶段，就共享一次 fopen。
                    // 这样首次建库、读卡器新增歌曲后的启动增量扫描、USB MSC 归还后的热刷新
                    // 都不会再为“技术探测 / Metadata / 封面索引”分别重新按路径打开同一个文件。
                    // 对大目录而言 fopen(path) 的 FAT 长文件名查找往往比解析本身更贵，因此这里
                    // 优先保证“一首需要深度解析的音频 = 一次打开”，各解析器内部只做 fseek 复位。
                    const bool need_probe_scan = deep_probe_format && !reused;
                    const bool need_metadata_scan = deep_probe_format && !metadata_reused;
                    const bool need_artwork_scan = deep_probe_format && !artwork_reused;
                    bool shared_deep_scan_handled = false;
                    if (deep_probe_format &&
                        (need_probe_scan || need_metadata_scan || need_artwork_scan)) {
                        shared_deep_scan_handled = true;
                        const int64_t open_started_us = esp_timer_get_time();
                        StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
                        if (!sd_lock.locked()) {
                            storage_timeout = true;
                        } else {
                            FILE *shared_file = fopen(full_path, "rb");
                            log_slow_scan_stage("文件打开", full_path, open_started_us);
                            if (shared_file == nullptr) {
                                ESP_LOGW(TAG, "建库无法打开音频文件，保留可复用信息/基础条目：%s", full_path);
                                record_scan_issue(full_path, "音频文件无法打开，已保留基础条目");
                                if (need_probe_scan) {
                                    technical = {};
                                    probe_failed_count++;
                                }
                                if (need_metadata_scan) {
                                    metadata_failed_count++;
                                }
                                if (need_artwork_scan) {
                                    artwork_failed_count++;
                                }
                            } else {
                                if (need_probe_scan) {
                                    const int64_t probe_started_us = esp_timer_get_time();
                                    const esp_err_t probe_ret = media_probe_open_file(
                                        shared_file, format, static_cast<uint64_t>(info.st_size), &technical);
                                    log_slow_scan_stage("技术探测", full_path, probe_started_us);
                                    if (probe_ret == ESP_OK) {
                                        probed_count++;
                                        if (format == MediaFormat::MP3 &&
                                            technical.sample_rate_hz != 32000U &&
                                            technical.sample_rate_hz != 44100U && technical.sample_rate_hz != 48000U) {
                                            char reason[96] = {};
                                            snprintf(reason, sizeof(reason), "MP3 %lu Hz 当前不可播放，歌曲仍保留在曲库",
                                                static_cast<unsigned long>(technical.sample_rate_hz));
                                            record_scan_issue(full_path, reason);
                                        } else if (format == MediaFormat::OPUS && technical.channels > 2U) {
                                            char reason[96] = {};
                                            snprintf(reason, sizeof(reason), "Opus %u 声道当前不可播放，歌曲仍保留在曲库",
                                                static_cast<unsigned>(technical.channels));
                                            record_scan_issue(full_path, reason);
                                        }
                                    } else {
                                        technical = {};
                                        probe_failed_count++;
                                        if (probe_ret == ESP_ERR_NO_MEM) {
                                            out_of_memory = true;
                                        } else if (format == MediaFormat::OPUS) {
                                            if (probe_ret == ESP_ERR_NOT_SUPPORTED) {
                                                record_scan_issue(full_path, "暂不支持该 Ogg Opus 结构，已保留基础条目");
                                            } else if (probe_ret == ESP_ERR_INVALID_SIZE) {
                                                record_scan_issue(full_path, "Ogg Opus 页面或长度异常，已保留基础条目");
                                            } else if (probe_ret == ESP_ERR_INVALID_RESPONSE) {
                                                record_scan_issue(full_path, "Ogg/OpusHead 结构异常，已保留基础条目");
                                            } else {
                                                record_scan_issue(full_path, "Ogg Opus 技术探测失败，已保留基础条目");
                                            }
                                        } else if (probe_ret == ESP_ERR_NOT_FOUND) {
                                            record_scan_issue(full_path, "未找到连续有效 MPEG Header，已保留基础条目");
                                        } else if (probe_ret == ESP_ERR_INVALID_SIZE) {
                                            record_scan_issue(full_path, "ID3 Size 或文件长度异常，已保留基础条目");
                                        } else if (probe_ret == ESP_ERR_INVALID_RESPONSE) {
                                            record_scan_issue(full_path, "ID3/MPEG Header 结构异常，已保留基础条目");
                                        } else {
                                            record_scan_issue(full_path, "音频技术探测失败，已保留基础条目");
                                        }
                                    }
                                }

                                if (!out_of_memory && need_metadata_scan) {
                                    metadata_build = static_cast<MediaMetadataBuildV2 *>(
                                        heap_caps_calloc(1, sizeof(MediaMetadataBuildV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                                    if (metadata_build == nullptr) {
                                        out_of_memory = true;
                                    } else {
                                        esp_err_t metadata_ret = ESP_OK;
                                        const int64_t metadata_started_us = esp_timer_get_time();
                                        if (!have_previous_v2 && !have_previous_v1) {
                                            const char *lrc_name = directory_lrc_index_find_for_audio(
                                                &directory_lrc_index, entry->d_name);
                                            char *resolved_lrc_path = lrc_name != nullptr
                                                ? media_library_alloc_join_path(directory, lrc_name)
                                                : nullptr;
                                            if (lrc_name != nullptr && resolved_lrc_path == nullptr) {
                                                metadata_ret = ESP_ERR_NO_MEM;
                                            } else {
                                                metadata_ret = media_metadata_scan_open_file_indexed_v2(
                                                    shared_file, full_path, resolved_lrc_path, format,
                                                    static_cast<uint64_t>(info.st_size), metadata_build);
                                            }
                                            heap_caps_free(resolved_lrc_path);
                                        } else {
                                            // V2 增量只有新增/变化歌曲才走这里：按需探测同名歌词，
                                            // 避免为了少量变化先额外遍历整个大目录。V1 迁移也保持原语义。
                                            metadata_ret = media_metadata_scan_open_file_v2(
                                                shared_file, full_path, format,
                                                static_cast<uint64_t>(info.st_size), metadata_build);
                                        }
                                        log_slow_scan_stage("Metadata", full_path, metadata_started_us);
                                        if (metadata_ret == ESP_OK) {
                                            metadata_scanned_count++;
                                        } else {
                                            ESP_LOGW(TAG, "Metadata 解析失败，保留文件名 fallback：%s [%s] ret=%s",
                                                full_path, media_format_name(format), esp_err_to_name(metadata_ret));
                                            media_metadata_build_release(metadata_build);
                                            heap_caps_free(metadata_build);
                                            metadata_build = nullptr;
                                            metadata_failed_count++;
                                            if (metadata_ret == ESP_ERR_NO_MEM) {
                                                out_of_memory = true;
                                            } else if (format == MediaFormat::OPUS && metadata_ret == ESP_ERR_INVALID_SIZE) {
                                                record_scan_issue(full_path, "OpusTags 字段范围异常，已回退文件名");
                                            } else if (format == MediaFormat::OPUS && metadata_ret == ESP_ERR_INVALID_RESPONSE) {
                                                record_scan_issue(full_path, "OpusTags 结构异常，已回退文件名");
                                            } else if (metadata_ret == ESP_ERR_INVALID_SIZE) {
                                                record_scan_issue(full_path, "ID3 Size/Frame 范围异常，已回退文件名");
                                            } else if (metadata_ret == ESP_ERR_INVALID_RESPONSE) {
                                                record_scan_issue(full_path, "ID3v2 结构异常，已回退文件名");
                                            } else {
                                                record_scan_issue(full_path, "Metadata 解析失败，已回退文件名");
                                            }
                                        }
                                    }
                                }

                                if (metadata_build != nullptr) {
                                    const uint32_t track_pair = MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2 | MEDIA_TRACK_META_HAS_TRACK_TOTAL_V2;
                                    if ((metadata_build->metadata_flags & track_pair) == track_pair &&
                                        metadata_build->track_number > metadata_build->track_total) {
                                        metadata_build->track_total = 0U;
                                        metadata_build->metadata_flags &= ~MEDIA_TRACK_META_HAS_TRACK_TOTAL_V2;
                                        record_scan_issue(full_path, format == MediaFormat::MP3
                                            ? "ID3 TRCK 曲目号/总数异常，已忽略总数"
                                            : "曲目号/总数异常，已忽略总数");
                                    }
                                    const uint32_t disc_pair = MEDIA_TRACK_META_HAS_DISC_NUMBER_V2 | MEDIA_TRACK_META_HAS_DISC_TOTAL_V2;
                                    if ((metadata_build->metadata_flags & disc_pair) == disc_pair &&
                                        metadata_build->disc_number > metadata_build->disc_total) {
                                        metadata_build->disc_total = 0U;
                                        metadata_build->metadata_flags &= ~MEDIA_TRACK_META_HAS_DISC_TOTAL_V2;
                                        record_scan_issue(full_path, format == MediaFormat::MP3
                                            ? "ID3 TPOS 碟号/总数异常，已忽略总数"
                                            : "碟号/总数异常，已忽略总数");
                                    }
                                }

                                if (!out_of_memory && need_artwork_scan) {
                                    artwork_build = static_cast<MediaArtworkBuildV2 *>(
                                        heap_caps_calloc(1, sizeof(MediaArtworkBuildV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                                    if (artwork_build == nullptr) {
                                        out_of_memory = true;
                                    } else {
                                        const int64_t artwork_started_us = esp_timer_get_time();
                                        const esp_err_t artwork_ret = media_artwork_scan_open_file_v2(
                                            shared_file, full_path, format, static_cast<uint64_t>(info.st_size),
                                            &directory_cover, artwork_build);
                                        log_slow_scan_stage("封面索引", full_path, artwork_started_us);
                                        if (artwork_ret == ESP_OK) {
                                            artwork_scanned_count++;
                                        } else {
                                            ESP_LOGW(TAG, "封面 locator 解析失败，按无封面继续：%s [%s] ret=%s",
                                                full_path, media_format_name(format), esp_err_to_name(artwork_ret));
                                            media_artwork_build_release_v2(artwork_build);
                                            heap_caps_free(artwork_build);
                                            artwork_build = nullptr;
                                            artwork_failed_count++;
                                            if (artwork_ret == ESP_ERR_NO_MEM) {
                                                out_of_memory = true;
                                            } else {
                                                record_scan_issue(full_path, format == MediaFormat::OPUS
                                                    ? "Opus内嵌/目录封面解析失败，已按无封面处理"
                                                    : "APIC/PICTURE 封面解析失败，已按无封面处理");
                                            }
                                        }
                                    }
                                }
                                fclose(shared_file);
                            }
                        }
                    }

                    if (storage_timeout) {
                        if (metadata_build != nullptr) {
                            media_metadata_build_release(metadata_build);
                            heap_caps_free(metadata_build);
                            metadata_build = nullptr;
                        }
                        if (artwork_build != nullptr) {
                            media_artwork_build_release_v2(artwork_build);
                            heap_caps_free(artwork_build);
                            artwork_build = nullptr;
                        }
                        break;
                    }
                    if (out_of_memory) {
                        if (metadata_build != nullptr) {
                            media_metadata_build_release(metadata_build);
                            heap_caps_free(metadata_build);
                            metadata_build = nullptr;
                        }
                        if (artwork_build != nullptr) {
                            media_artwork_build_release_v2(artwork_build);
                            heap_caps_free(artwork_build);
                            artwork_build = nullptr;
                        }
                        break;
                    }

                    // 非 MP3/FLAC 不需要深度打开；这里只保留目录 fallback artwork 的原行为。
                    // MP3/FLAC 需要重新解析时已经由上面的 shared_file 一次完成，不能再二次 fopen。
                    if (!shared_deep_scan_handled && !artwork_reused) {
                        artwork_build = static_cast<MediaArtworkBuildV2 *>(
                            heap_caps_calloc(1, sizeof(MediaArtworkBuildV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                        );
                        if (artwork_build == nullptr) {
                            out_of_memory = true;
                            if (metadata_build != nullptr) {
                                media_metadata_build_release(metadata_build);
                                heap_caps_free(metadata_build);
                                metadata_build = nullptr;
                            }
                            break;
                        }
                        const int64_t artwork_started_us = esp_timer_get_time();
                        const esp_err_t artwork_ret = media_artwork_scan_file_v2(
                            full_path, format, static_cast<uint64_t>(info.st_size), &directory_cover, artwork_build
                        );
                        log_slow_scan_stage("封面索引", full_path, artwork_started_us);
                        if (artwork_ret == ESP_OK) {
                            artwork_scanned_count++;
                        } else {
                            ESP_LOGW(TAG, "封面 locator 解析失败，按无封面继续：%s [%s] ret=%s",
                                full_path, media_format_name(format), esp_err_to_name(artwork_ret));
                            media_artwork_build_release_v2(artwork_build);
                            heap_caps_free(artwork_build);
                            artwork_build = nullptr;
                            artwork_failed_count++;
                            if (artwork_ret == ESP_ERR_NO_MEM) {
                                out_of_memory = true;
                                if (metadata_build != nullptr) {
                                    media_metadata_build_release(metadata_build);
                                    heap_caps_free(metadata_build);
                                }
                                break;
                            }
                            record_scan_issue(full_path, "APIC/PICTURE 封面解析失败，已按无封面处理");
                        }
                    }

                    if (artwork_build != nullptr) {
                        if (artwork_build->source == MediaArtworkSourceV2::Mp3Apic ||
                            artwork_build->source == MediaArtworkSourceV2::FlacPicture ||
                            artwork_build->source == MediaArtworkSourceV2::OpusPicture) {
                            artwork_embedded_count++;
                        } else if (artwork_build->source == MediaArtworkSourceV2::ExternalFile) {
                            artwork_external_count++;
                        } else {
                            artwork_none_count++;
                        }
                    } else {
                        artwork_none_count++;
                    }

                    const bool full_track_reused =
                        v2_signature_match &&
                        (!deep_probe_format || (reused && metadata_reused)) &&
                        artwork_unchanged;
                    if (full_track_reused) {
                        full_track_reused_count++;
                    }

                    // 歌曲文件自身未变，但目录 fallback 封面变化也属于可见媒体更新。
                    if (have_previous_v2 && old_v2_track != nullptr &&
                        !catalog_change_counted && v2_signature_match && !artwork_unchanged) {
                        updated_count++;
                        notify_changes_detected();
                    }

                    if (!media_library_add_track(full_path, format, info, technical, metadata_build, artwork_build)) {
                        if (metadata_build != nullptr) {
                            media_metadata_build_release(metadata_build);
                            heap_caps_free(metadata_build);
                        }
                        if (artwork_build != nullptr) {
                            media_artwork_build_release_v2(artwork_build);
                            heap_caps_free(artwork_build);
                        }
                        out_of_memory = true;
                        break;
                    }
                    notify_initial_build_progress(false);
                    format_count[static_cast<size_t>(format)]++;
                    if ((g_entry_count % SCAN_YIELD_INTERVAL_TRACKS) == 0U) {
                        vTaskDelay(1);
                    }
#if APP_DIAG_LIBRARY_ITEMS
                    if (g_entry_count <= LOG_TRACK_LIMIT) {
                        const MediaMetadataBuildV2 *meta = g_entries[g_entry_count - 1].metadata_build;
                        ESP_LOGI(TAG, "发现歌曲[%u] %s [%s] index=%s metadata=%s rate=%luHz offset=%llu",
                            static_cast<unsigned>(g_entry_count),
                            full_path,
                            media_format_name(format),
                            reused ? "复用" : ((technical.flags & MEDIA_TECH_PARSED) != 0U ? "新解析" : "基础"),
                            metadata_reused ? "复用" : (meta != nullptr ? "新解析" : "基础"),
                            static_cast<unsigned long>(technical.sample_rate_hz),
                            static_cast<unsigned long long>(technical.audio_data_offset));
                    }
#endif
#if APP_DIAG_LIBRARY_METADATA
                    if (g_entry_count <= LOG_TRACK_LIMIT) {
                        const MediaMetadataBuildV2 *meta = g_entries[g_entry_count - 1].metadata_build;
                        if (meta != nullptr && (meta->metadata_flags & MEDIA_TRACK_META_SCANNED_V2) != 0U) {
                            ESP_LOGI(TAG, "META_TRACE[%u]: title=%s artist=%s album=%s track=%u/%u disc=%u/%u year=%u original=%u artists=%u lyrics=%u",
                                static_cast<unsigned>(g_entry_count),
                                meta->title != nullptr ? meta->title : "(文件名)",
                                meta->display_artist != nullptr ? meta->display_artist : "",
                                meta->album != nullptr ? meta->album : "",
                                static_cast<unsigned>(meta->track_number),
                                static_cast<unsigned>(meta->track_total),
                                static_cast<unsigned>(meta->disc_number),
                                static_cast<unsigned>(meta->disc_total),
                                static_cast<unsigned>(meta->release_year),
                                static_cast<unsigned>(meta->original_year),
                                static_cast<unsigned>(meta->artist_count),
                                static_cast<unsigned>(meta->lyrics_count));
                        }
                    }
#endif
#if APP_DIAG_LIBRARY_ARTWORK
                    if (g_entry_count <= LOG_TRACK_LIMIT) {
                        const MediaArtworkBuildV2 *art = g_entries[g_entry_count - 1].artwork_build;
                        ESP_LOGI(TAG, "ARTWORK_TRACE[%u]: source=%u format=%u type=%u offset=%llu size=%lu %ux%u external=%s",
                            static_cast<unsigned>(g_entry_count),
                            static_cast<unsigned>(art != nullptr ? art->source : MediaArtworkSourceV2::None),
                            static_cast<unsigned>(art != nullptr ? art->format : MediaArtworkFormatV2::Unknown),
                            static_cast<unsigned>(art != nullptr ? art->picture_type : 0U),
                            static_cast<unsigned long long>(art != nullptr ? art->data_offset : 0U),
                            static_cast<unsigned long>(art != nullptr ? art->data_size : 0U),
                            static_cast<unsigned>(art != nullptr ? art->width : 0U),
                            static_cast<unsigned>(art != nullptr ? art->height : 0U),
                            art != nullptr && art->external_path != nullptr ? art->external_path : "");
                    }
#endif
                }
            }
        }
        {
            StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
            if (sd_lock.locked()) {
                closedir(dir);
            } else {
                storage_timeout = true;
            }
        }
        media_artwork_build_release_v2(&directory_cover);
        directory_lrc_index_release(&directory_lrc_index);
        heap_caps_free(directory);
    }
    directory_stack_destroy(&stack);
    const int64_t scan_walk_finished_us = esp_timer_get_time();

    if (scan_io_error) {
        ESP_LOGE(TAG, "扫描遇到TF/FAT I/O异常：stage=%s errno=%d path=%s；本轮不发布残缺Catalog",
            scan_io_stage, scan_io_errno, scan_io_path);

        // 正常开机若已有通过校验的旧 V2 Catalog，I/O 异常时继续发布旧库，
        // 避免瞬态坏卡把播放器降级成空曲库。USB 热刷新则保持当前运行时 Catalog 不变。
        if (!replace_runtime_catalog && have_previous_v2) {
            const uint32_t fallback_track_count = previous_v2.catalog.track_count;
            const uint32_t fallback_crc = previous_v2.index_crc32;
            next_catalog = previous_v2.catalog;
            previous_v2.catalog = {};
            const esp_err_t publish_ret = media_catalog_v2_publish(&next_catalog, fallback_crc);
            if (publish_ret == ESP_OK) {
                media_catalog_store_v2_release(&previous_v2);
                media_index_store_release(&previous_v1);
                media_library_release_build_buffers();
                g_ready = true;
                if (out_changes != nullptr) {
                    out_changes->had_previous_catalog = true;
                    out_changes->previous_count = fallback_track_count;
                    out_changes->current_count = fallback_track_count;
                    out_changes->issue_count = 1U;
                    snprintf(out_changes->first_issue_file, sizeof(out_changes->first_issue_file), "%s", scan_io_path);
                    snprintf(out_changes->first_issue_reason, sizeof(out_changes->first_issue_reason),
                        "TF/FAT I/O异常，已保留旧曲库");
                }
                ESP_LOGW(TAG, "扫描异常后已回退旧V2 Catalog：tracks=%u",
                    static_cast<unsigned>(fallback_track_count));
                return ESP_OK;
            }
            media_catalog_v2_release(&next_catalog);
            ESP_LOGE(TAG, "扫描异常后发布旧V2 Catalog失败：%s", esp_err_to_name(publish_ret));
        }

        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset_build_state(replace_runtime_catalog);
        return ESP_FAIL;
    }

    if (storage_timeout) {
        ESP_LOGE(TAG, "扫描过程中等待TF访问锁超时，终止本次建库，避免永久卡住");
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset_build_state(replace_runtime_catalog);
        return ESP_ERR_TIMEOUT;
    }

    if (out_of_memory) {
        ESP_LOGE(TAG, "扫描过程中 PSRAM 不足，音乐库未完成");
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset_build_state(replace_runtime_catalog);
        return ESP_ERR_NO_MEM;
    }
    // 若最后一批不足节流阈值，也要把最终已发现数量送到启动页；
    // 后续还可能进行排序、Catalog 组装和落盘，因此用户不会在这段时间看到过期数字。
    notify_initial_build_progress(true);
    notify_incremental_added_progress(true);

    if (root_missing) {
        ESP_LOGW(TAG, "音乐目录不存在：%s，音乐库保持为空", MUSIC_ROOT);
    }
    media_library_sort_entries();

    size_t removed_count =
        have_previous_v2 && previous_track_count > existing_path_count
        ? previous_track_count - existing_path_count
        : 0U;
    if (removed_count > 0U) {
        notify_changes_detected();
    }

    const auto publish_change_summary = [&](size_t current_count) {
        if (out_changes == nullptr) {
            return;
        }
        out_changes->had_previous_catalog = have_previous_v2;
        out_changes->previous_count = static_cast<uint32_t>(previous_track_count);
        out_changes->current_count = static_cast<uint32_t>(current_count);
        out_changes->added_count = static_cast<uint32_t>(added_count);
        out_changes->removed_count = static_cast<uint32_t>(removed_count);
        out_changes->updated_count = static_cast<uint32_t>(updated_count);
        out_changes->issue_count = static_cast<uint32_t>(issue_count);
        out_changes->skipped_count = static_cast<uint32_t>(skipped_track_count);
        snprintf(out_changes->first_issue_file, sizeof(out_changes->first_issue_file), "%s", first_issue_file);
        snprintf(out_changes->first_issue_reason, sizeof(out_changes->first_issue_reason), "%s", first_issue_reason);
        out_changes->changed =
            have_previous_v2 &&
            (added_count > 0U || removed_count > 0U || updated_count > 0U);
    };

    if (!media_library_repack_sorted_path_pool()) {
        ESP_LOGE(TAG, "排序后重建确定性路径池失败");
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset_build_state(replace_runtime_catalog);
        return ESP_ERR_NO_MEM;
    }

    // 全库未变化且旧 V2 来自 final 时直接发布已经完整校验过的旧 Catalog。
    // 这样大曲库第二次启动不需要同时保留“旧 Catalog + 新 Catalog”两份 PSRAM。
    bool reuse_whole_catalog = have_previous_v2 &&
        previous_v2.source == MediaCatalogLoadSourceV2::Final &&
        full_track_reused_count == g_entry_count &&
        previous_v2.catalog.track_count == g_entry_count;
    if (reuse_whole_catalog) {
        for (size_t i = 0; i < g_entry_count; ++i) {
            const char *scan_path = g_path_pool + g_entries[i].path_offset;
            const char *old_path = media_catalog_v2_pool_str(
                &previous_v2.catalog, previous_v2.catalog.tracks[i].path_off
            );
            if (old_path == nullptr || strcasecmp(scan_path, old_path) != 0 ||
                g_entries[i].format != previous_v2.catalog.tracks[i].format) {
                reuse_whole_catalog = false;
                break;
            }
        }
    }

    if (reuse_whole_catalog) {
        const uint32_t final_track_count = previous_v2.catalog.track_count;
        const uint32_t final_string_bytes = previous_v2.catalog.pool.size;
        const uint32_t final_track_bytes = previous_v2.catalog.track_count * sizeof(TrackRowV2);
        const uint32_t final_artist_bytes = previous_v2.catalog.artist_count * sizeof(ArtistRowV2);
        const uint32_t final_album_bytes = previous_v2.catalog.album_count * sizeof(AlbumRowV2);
        // 复用同一份 PSRAM scratch 作为 Catalog move target，避免栈上再放一个 168B MusicCatalogV2。
        next_catalog = previous_v2.catalog;
        previous_v2.catalog = {};
        const uint32_t source_crc = previous_v2.index_crc32;
        const esp_err_t publish_ret = replace_runtime_catalog
            ? media_catalog_v2_replace_quiesced(&next_catalog, source_crc, out_retired)
            : media_catalog_v2_publish(&next_catalog, source_crc);
        if (publish_ret != ESP_OK) {
            media_catalog_v2_release(&next_catalog);
            media_catalog_store_v2_release(&previous_v2);
            media_index_store_release(&previous_v1);
            media_library_reset_build_state(replace_runtime_catalog);
            return publish_ret;
        }
        LIB_BOOT_LOGI("V2 Catalog 全量命中 Manifest，直接复用已校验运行时目录，不重建、不写盘");
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_release_build_buffers();
        g_ready = true;
        const esp_err_t quick_stamp_ret = media_library_write_quick_stamp(source_crc, final_track_count);
        if (quick_stamp_ret != ESP_OK) {
            ESP_LOGW(TAG, "更新曲库快速变更戳失败，下次启动回退完整增量扫描：%s",
                esp_err_to_name(quick_stamp_ret));
        }

        const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000);
        if (have_previous_v2) {
            const uint32_t walk_ms = static_cast<uint32_t>((scan_walk_finished_us - start_us) / 1000LL);
            ESP_LOGI(TAG, "增量统计：完整复用=%u 新增=%u 更新=%u 删除=%u",
                static_cast<unsigned>(full_track_reused_count),
                static_cast<unsigned>(added_count),
                static_cast<unsigned>(updated_count),
                static_cast<unsigned>(removed_count));
            ESP_LOGI(TAG, "增量属性校验：模式=%s 跳过stat=%u 实际stat=%u",
                fast_boot_incremental ? "启动新增快扫" : "严格校验",
                static_cast<unsigned>(fast_stat_skipped_count),
                static_cast<unsigned>(strict_stat_count));
            ESP_LOGI(TAG, "增量延迟克隆：扫描期跳过Metadata=%u Artwork=%u，本轮无变化无需补齐",
                static_cast<unsigned>(deferred_metadata_clone_count),
                static_cast<unsigned>(deferred_artwork_clone_count));
            ESP_LOGI(TAG, "增量阶段耗时：目录扫描=%u ms Catalog重建=0 ms V2落盘=0 ms 总计=%u ms",
                static_cast<unsigned>(walk_ms),
                static_cast<unsigned>(elapsed_ms));
        }
        ESP_LOGI(TAG, "扫描完成：歌曲=%u，目录=%u，耗时=%u ms，技术复用=%u，新技术解析=%u，技术失败=%u",
            static_cast<unsigned>(final_track_count),
            static_cast<unsigned>(directory_count),
            static_cast<unsigned>(elapsed_ms),
            static_cast<unsigned>(reused_count),
            static_cast<unsigned>(probed_count),
            static_cast<unsigned>(probe_failed_count));
        LIB_BOOT_LOGI("Metadata统计：复用=%u，新解析=%u，失败=%u",
            static_cast<unsigned>(metadata_reused_count),
            static_cast<unsigned>(metadata_scanned_count),
            static_cast<unsigned>(metadata_failed_count));
        LIB_BOOT_LOGI("封面统计：复用=%u，目录刷新=%u，新扫描=%u，内嵌=%u，目录fallback=%u，无封面=%u，失败=%u",
            static_cast<unsigned>(artwork_reused_count),
            static_cast<unsigned>(artwork_refreshed_count),
            static_cast<unsigned>(artwork_scanned_count),
            static_cast<unsigned>(artwork_embedded_count),
            static_cast<unsigned>(artwork_external_count),
            static_cast<unsigned>(artwork_none_count),
            static_cast<unsigned>(artwork_failed_count));
        LIB_BOOT_LOGI("格式统计：MP3=%u，FLAC=%u，WAV=%u，NSF=%u，NSFE=%u，OPUS=%u",
            static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::MP3)]),
            static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::FLAC)]),
            static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::WAV)]),
            static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::NSF)]),
            static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::NSFE)]),
            static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::OPUS)]));
        const MusicCatalogV2 *published = media_catalog_v2_current();
        LIB_BOOT_LOGI("MusicCatalogV2 PSRAM：tracks=%luB artists=%luB albums=%luB artist_refs=%luB lyrics_refs=%luB artwork_refs=%luB strings=%luB groups=%uB generation=%lu",
            static_cast<unsigned long>(final_track_bytes),
            static_cast<unsigned long>(final_artist_bytes),
            static_cast<unsigned long>(final_album_bytes),
            static_cast<unsigned long>(published != nullptr ? published->track_artist_ref_count * sizeof(TrackArtistRefV2) : 0U),
            static_cast<unsigned long>(published != nullptr ? published->lyrics_ref_count * sizeof(LyricsRefV2) : 0U),
            static_cast<unsigned long>(published != nullptr ? published->artwork_ref_count * sizeof(ArtworkRefV2) : 0U),
            static_cast<unsigned long>(final_string_bytes),
            static_cast<unsigned>(media_groups_v2_psram_bytes(published)),
            static_cast<unsigned long>(media_catalog_v2_generation()));
        publish_change_summary(final_track_count);
        return ESP_OK;
    }

    // 启动快扫如果最终发现了真实变化，才需要构建新 Catalog。
    // 此时再一次性补齐前面被延迟的旧 Metadata/Artwork；无变化启动不会进入这里，
    // 从而彻底避免“1000 首旧歌 clone 一遍，随后又全部释放”的无效 PSRAM 工作。
    const int64_t deferred_hydrate_started_us = esp_timer_get_time();
    if (fast_boot_incremental) {
        for (size_t i = 0; i < g_entry_count; ++i) {
            MediaEntry &entry = g_entries[i];
            const char *path = g_path_pool + entry.path_offset;
            const TrackRowV2 *old_track = nullptr;
            const MediaManifestRecordV2 *old_manifest = nullptr;
            if (!media_catalog_store_v2_find(&previous_v2, path, &old_track, &old_manifest) ||
                old_track == nullptr || old_manifest == nullptr ||
                old_track->format != entry.format || old_manifest->format != entry.format ||
                old_manifest->file_size_bytes != entry.file_size_bytes ||
                old_manifest->modified_time != entry.modified_time) {
                continue;
            }

            const uint32_t old_track_index =
                static_cast<uint32_t>(old_track - previous_v2.catalog.tracks);
            const bool deep_probe_format =
                entry.format == MediaFormat::FLAC || entry.format == MediaFormat::MP3 || entry.format == MediaFormat::OPUS;

            if (deep_probe_format && entry.metadata_build == nullptr &&
                (old_track->metadata_flags & MEDIA_TRACK_META_SCANNED_V2) != 0U) {
                entry.metadata_build = static_cast<MediaMetadataBuildV2 *>(
                    heap_caps_calloc(1, sizeof(MediaMetadataBuildV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                );
                if (entry.metadata_build == nullptr) {
                    out_of_memory = true;
                    break;
                }
                const esp_err_t clone_ret = media_metadata_clone_from_catalog_v2(
                    &previous_v2.catalog, old_track_index, entry.metadata_build);
                if (clone_ret != ESP_OK) {
                    media_metadata_build_release(entry.metadata_build);
                    heap_caps_free(entry.metadata_build);
                    entry.metadata_build = nullptr;
                    if (clone_ret == ESP_ERR_NO_MEM) {
                        out_of_memory = true;
                    } else {
                        ESP_LOGE(TAG, "延迟补齐旧 Metadata 失败：%s ret=%s", path, esp_err_to_name(clone_ret));
                        media_catalog_store_v2_release(&previous_v2);
                        media_index_store_release(&previous_v1);
                        media_library_reset_build_state(replace_runtime_catalog);
                        return clone_ret;
                    }
                    break;
                }
                hydrated_metadata_clone_count++;
            }

            if (entry.artwork_build == nullptr &&
                old_track->artwork_ref_id != MEDIA_CATALOG_INVALID_ID_V2) {
                entry.artwork_build = static_cast<MediaArtworkBuildV2 *>(
                    heap_caps_calloc(1, sizeof(MediaArtworkBuildV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                );
                if (entry.artwork_build == nullptr) {
                    out_of_memory = true;
                    break;
                }
                const esp_err_t clone_ret = media_artwork_clone_exact_from_catalog_v2(
                    &previous_v2.catalog, old_track_index, entry.artwork_build);
                if (clone_ret != ESP_OK) {
                    media_artwork_build_release_v2(entry.artwork_build);
                    heap_caps_free(entry.artwork_build);
                    entry.artwork_build = nullptr;
                    if (clone_ret == ESP_ERR_NO_MEM) {
                        out_of_memory = true;
                    } else {
                        ESP_LOGE(TAG, "延迟补齐旧封面 locator 失败：%s ret=%s", path, esp_err_to_name(clone_ret));
                        media_catalog_store_v2_release(&previous_v2);
                        media_index_store_release(&previous_v1);
                        media_library_reset_build_state(replace_runtime_catalog);
                        return clone_ret;
                    }
                    break;
                }
                hydrated_artwork_clone_count++;
            }
        }
    }
    if (out_of_memory) {
        ESP_LOGE(TAG, "增量变化后补齐旧 Catalog 中间对象时 PSRAM 不足");
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset_build_state(replace_runtime_catalog);
        return ESP_ERR_NO_MEM;
    }
    const uint32_t deferred_hydrate_ms =
        static_cast<uint32_t>((esp_timer_get_time() - deferred_hydrate_started_us) / 1000LL);
    if (fast_boot_incremental) {
        ESP_LOGI(TAG, "增量延迟克隆：扫描期跳过Metadata=%u Artwork=%u，重建前补齐Metadata=%u Artwork=%u，耗时=%u ms",
            static_cast<unsigned>(deferred_metadata_clone_count),
            static_cast<unsigned>(deferred_artwork_clone_count),
            static_cast<unsigned>(hydrated_metadata_clone_count),
            static_cast<unsigned>(hydrated_artwork_clone_count),
            static_cast<unsigned>(deferred_hydrate_ms));
    }

    // 扫描缓冲只负责构建。正式运行时使用精确尺寸的 MusicCatalogV2，
    // 从这里开始 UI/Player 不再依赖扫描期 g_entries/path_pool。
    media_catalog_v2_release(&next_catalog);
    const int64_t catalog_build_started_us = esp_timer_get_time();
    esp_err_t catalog_ret = media_catalog_v2_build_from_index_records(
        g_entries, g_entry_count, g_path_pool, g_path_size, &next_catalog
    );

    // 正常路径不增加额外 Catalog 构建。只有整库语义校验失败时才逐首定位，
    // 并通过临时剥离可选数据判断是封面、歌词、Metadata 还是 technical 导致失败。
    if (media_library_is_track_catalog_error(catalog_ret)) {
        bool recovered_any_track = false;
        size_t i = 0U;
        while (i < g_entry_count) {
            esp_err_t single_ret = media_library_validate_single_track(i);
            if (single_ret == ESP_OK) {
                i++;
                continue;
            }
            if (!media_library_is_track_catalog_error(single_ret)) {
                catalog_ret = single_ret;
                break;
            }

            MediaEntry &entry = g_entries[i];
            const char *path = g_path_pool + entry.path_offset;
            bool repaired = false;

            if (entry.artwork_build != nullptr) {
                MediaArtworkBuildV2 *saved = entry.artwork_build;
                entry.artwork_build = nullptr;
                single_ret = media_library_validate_single_track(i);
                entry.artwork_build = saved;
                if (single_ret == ESP_OK) {
                    media_library_drop_artwork(&entry);
                    record_scan_issue(path, "Catalog 封面数据异常，已按无封面处理");
                    repaired = true;
                } else if (!media_library_is_track_catalog_error(single_ret)) {
                    catalog_ret = single_ret;
                    break;
                }
            }

            if (!repaired && entry.metadata_build != nullptr && entry.metadata_build->lyrics_count > 0U) {
                const uint16_t saved_count = entry.metadata_build->lyrics_count;
                entry.metadata_build->lyrics_count = 0U;
                single_ret = media_library_validate_single_track(i);
                entry.metadata_build->lyrics_count = saved_count;
                if (single_ret == ESP_OK) {
                    media_library_drop_lyrics(entry.metadata_build);
                    record_scan_issue(path, "Catalog USLT/SYLT 歌词数据异常，已忽略歌词");
                    repaired = true;
                } else if (!media_library_is_track_catalog_error(single_ret)) {
                    catalog_ret = single_ret;
                    break;
                }
            }

            if (!repaired && entry.metadata_build != nullptr) {
                MediaMetadataBuildV2 *saved = entry.metadata_build;
                entry.metadata_build = nullptr;
                single_ret = media_library_validate_single_track(i);
                entry.metadata_build = saved;
                if (single_ret == ESP_OK) {
                    media_library_drop_metadata(&entry);
                    record_scan_issue(path, "Catalog Metadata 异常，已回退文件名");
                    repaired = true;
                } else if (!media_library_is_track_catalog_error(single_ret)) {
                    catalog_ret = single_ret;
                    break;
                }
            }

            if (!repaired) {
                const MediaTechnicalInfo saved = entry.technical;
                entry.technical = {};
                single_ret = media_library_validate_single_track(i);
                if (single_ret == ESP_OK) {
                    record_scan_issue(path, "Catalog technical 数据异常，已清除技术信息");
                    repaired = true;
                } else {
                    entry.technical = saved;
                    if (!media_library_is_track_catalog_error(single_ret)) {
                        catalog_ret = single_ret;
                        break;
                    }
                }
            }

            if (repaired) {
                recovered_any_track = true;
                i++;
                continue;
            }

            // 清空全部可选数据仍无效，说明这首记录本身无法进入 Catalog；只跳过该曲。
            record_scan_issue(path, "单曲 Catalog 仍无效，本次已跳过");
            skipped_track_count++;
            recovered_any_track = true;
            if (have_previous_v2) {
                const TrackRowV2 *old_track = nullptr;
                const MediaManifestRecordV2 *old_manifest = nullptr;
                if (media_catalog_store_v2_find(&previous_v2, path, &old_track, &old_manifest) && old_track != nullptr) {
                    const bool was_updated = old_manifest != nullptr &&
                        (old_track->format != entry.format || old_manifest->format != entry.format ||
                         old_manifest->file_size_bytes != entry.file_size_bytes ||
                         old_manifest->modified_time != entry.modified_time);
                    if (was_updated && updated_count > 0U) {
                        updated_count--;
                    }
                    removed_count++;
                    notify_changes_detected();
                } else if (added_count > 0U) {
                    added_count--;
                }
            }
            media_library_drop_metadata(&entry);
            media_library_drop_artwork(&entry);
            if (i + 1U < g_entry_count) {
                memmove(&g_entries[i], &g_entries[i + 1U],
                    (g_entry_count - i - 1U) * sizeof(MediaEntry));
            }
            g_entry_count--;
            g_entries[g_entry_count] = {};
        }

        if (media_library_is_track_catalog_error(catalog_ret) && recovered_any_track) {
            media_catalog_v2_release(&next_catalog);
            catalog_ret = media_catalog_v2_build_from_index_records(
                g_entries, g_entry_count, g_path_pool, g_path_size, &next_catalog);
        }
    }
    const int64_t catalog_build_finished_us = esp_timer_get_time();
    if (catalog_ret != ESP_OK) {
        ESP_LOGE(TAG, "构建 MusicCatalogV2 失败：%s", esp_err_to_name(catalog_ret));
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset_build_state(replace_runtime_catalog);
        return catalog_ret;
    }

    // Catalog/Manifest 是缓存：写盘失败不阻断本次内存曲库，但发布前必须通过 semantic validation。
    uint32_t catalog_crc = 0;
    const int64_t catalog_commit_started_us = esp_timer_get_time();
    const esp_err_t index_ret = media_catalog_store_v2_commit(
        &next_catalog,
        g_entries,
        g_entry_count,
        have_previous_v2 ? &previous_v2 : nullptr,
        &catalog_crc
    );
    const int64_t catalog_commit_finished_us = esp_timer_get_time();
    if (index_ret != ESP_OK) {
        ESP_LOGW(TAG, "V2 Catalog 持久化失败，本次继续使用内存目录：%s", esp_err_to_name(index_ret));
    }

    const uint32_t final_track_count = next_catalog.track_count;
    const uint32_t final_string_bytes = next_catalog.pool.size;
    const uint32_t final_track_bytes = next_catalog.track_count * sizeof(TrackRowV2);
    const uint32_t final_artist_bytes = next_catalog.artist_count * sizeof(ArtistRowV2);
    const uint32_t final_album_bytes = next_catalog.album_count * sizeof(AlbumRowV2);
    catalog_ret = replace_runtime_catalog
        ? media_catalog_v2_replace_quiesced(&next_catalog, catalog_crc, out_retired)
        : media_catalog_v2_publish(&next_catalog, catalog_crc);
    if (catalog_ret != ESP_OK) {
        ESP_LOGE(TAG, "发布 MusicCatalogV2 失败：%s", esp_err_to_name(catalog_ret));
        media_catalog_v2_release(&next_catalog);
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset_build_state(replace_runtime_catalog);
        return catalog_ret;
    }

    media_catalog_store_v2_release(&previous_v2);
    media_index_store_release(&previous_v1);
    media_library_release_build_buffers();
    g_ready = true;
    if (index_ret == ESP_OK) {
        const esp_err_t quick_stamp_ret = media_library_write_quick_stamp(catalog_crc, final_track_count);
        if (quick_stamp_ret != ESP_OK) {
            ESP_LOGW(TAG, "更新曲库快速变更戳失败，下次启动回退完整增量扫描：%s",
                esp_err_to_name(quick_stamp_ret));
        }
    }

    const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000);
    if (have_previous_v2) {
        const uint32_t walk_ms = static_cast<uint32_t>((scan_walk_finished_us - start_us) / 1000LL);
        const uint32_t build_ms = static_cast<uint32_t>((catalog_build_finished_us - catalog_build_started_us) / 1000LL);
        const uint32_t commit_ms = static_cast<uint32_t>((catalog_commit_finished_us - catalog_commit_started_us) / 1000LL);
        ESP_LOGI(TAG, "增量统计：完整复用=%u 新增=%u 更新=%u 删除=%u",
            static_cast<unsigned>(full_track_reused_count),
            static_cast<unsigned>(added_count),
            static_cast<unsigned>(updated_count),
            static_cast<unsigned>(removed_count));
        ESP_LOGI(TAG, "增量属性校验：模式=%s 跳过stat=%u 实际stat=%u",
            fast_boot_incremental ? "启动新增快扫" : "严格校验",
            static_cast<unsigned>(fast_stat_skipped_count),
            static_cast<unsigned>(strict_stat_count));
        ESP_LOGI(TAG, "增量阶段耗时：目录扫描=%u ms Catalog重建=%u ms V2落盘=%u ms 总计=%u ms",
            static_cast<unsigned>(walk_ms),
            static_cast<unsigned>(build_ms),
            static_cast<unsigned>(commit_ms),
            static_cast<unsigned>(elapsed_ms));
    }
    ESP_LOGI(TAG, "扫描完成：歌曲=%u，目录=%u，耗时=%u ms，技术复用=%u，新技术解析=%u，技术失败=%u",
        static_cast<unsigned>(final_track_count),
        static_cast<unsigned>(directory_count),
        static_cast<unsigned>(elapsed_ms),
        static_cast<unsigned>(reused_count),
        static_cast<unsigned>(probed_count),
        static_cast<unsigned>(probe_failed_count));
    LIB_BOOT_LOGI("Metadata统计：复用=%u，新解析=%u，失败=%u",
        static_cast<unsigned>(metadata_reused_count),
        static_cast<unsigned>(metadata_scanned_count),
        static_cast<unsigned>(metadata_failed_count));
    LIB_BOOT_LOGI("封面统计：复用=%u，目录刷新=%u，新扫描=%u，内嵌=%u，目录fallback=%u，无封面=%u，失败=%u",
        static_cast<unsigned>(artwork_reused_count),
        static_cast<unsigned>(artwork_refreshed_count),
        static_cast<unsigned>(artwork_scanned_count),
        static_cast<unsigned>(artwork_embedded_count),
        static_cast<unsigned>(artwork_external_count),
        static_cast<unsigned>(artwork_none_count),
        static_cast<unsigned>(artwork_failed_count));
    LIB_BOOT_LOGI("格式统计：MP3=%u，FLAC=%u，WAV=%u，NSF=%u，NSFE=%u，OPUS=%u",
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::MP3)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::FLAC)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::WAV)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::NSF)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::NSFE)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::OPUS)]));
    const MusicCatalogV2 *published = media_catalog_v2_current();
    LIB_BOOT_LOGI("MusicCatalogV2 PSRAM：tracks=%luB artists=%luB albums=%luB artist_refs=%luB lyrics_refs=%luB artwork_refs=%luB strings=%luB groups=%uB generation=%lu",
        static_cast<unsigned long>(final_track_bytes),
        static_cast<unsigned long>(final_artist_bytes),
        static_cast<unsigned long>(final_album_bytes),
        static_cast<unsigned long>(published != nullptr ? published->track_artist_ref_count * sizeof(TrackArtistRefV2) : 0U),
        static_cast<unsigned long>(published != nullptr ? published->lyrics_ref_count * sizeof(LyricsRefV2) : 0U),
        static_cast<unsigned long>(published != nullptr ? published->artwork_ref_count * sizeof(ArtworkRefV2) : 0U),
        static_cast<unsigned long>(final_string_bytes),
        static_cast<unsigned>(media_groups_v2_psram_bytes(published)),
        static_cast<unsigned long>(media_catalog_v2_generation()));
    publish_change_summary(final_track_count);
    return ESP_OK;
}

esp_err_t media_library_scan(
    MediaLibraryChangeSummary *out_changes,
    MediaLibraryScanEventCallback on_scan_event,
    void *callback_context)
{
    if (g_ready || media_catalog_v2_ready()) {
        // 当前 Catalog View 使用“generation 未变化期间有效”的裸指针语义；
        // 未实现运行期 lease/refcount 前禁止二次扫描替换正在使用的 Catalog。
        ESP_LOGE(TAG, "音乐库已发布，本次启动禁止再次扫描替换运行时 Catalog");
        return ESP_ERR_INVALID_STATE;
    }
    if (!sdcard_is_mounted()) {
        ESP_LOGE(TAG, "TF 卡尚未挂载，无法扫描音乐库");
        return ESP_ERR_INVALID_STATE;
    }

    MediaLibraryScanScratch *scratch = static_cast<MediaLibraryScanScratch *>(
        heap_caps_calloc(1, sizeof(MediaLibraryScanScratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (scratch == nullptr) {
        ESP_LOGE(TAG, "创建扫描期 PSRAM scratch 失败");
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t ret = media_library_scan_with_scratch(
        scratch, false, nullptr, out_changes, on_scan_event, callback_context);
    media_library_scan_scratch_release(scratch);
    return ret;
}

esp_err_t media_library_hot_reload_quiesced(
    MusicCatalogV2 *out_retired,
    MediaLibraryChangeSummary *out_changes,
    MediaLibraryScanEventCallback on_scan_event,
    void *callback_context)
{
    if (out_retired == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_retired = {};
    if (!g_ready || !media_catalog_v2_ready() || !sdcard_is_mounted()) {
        ESP_LOGE(TAG, "热刷新要求旧Catalog有效且TF已重新挂载");
        return ESP_ERR_INVALID_STATE;
    }
    if (!storage_io_usb_handoff_blocked()) {
        ESP_LOGE(TAG, "拒绝未隔离普通TF访问的Catalog热刷新");
        return ESP_ERR_INVALID_STATE;
    }

    MediaLibraryScanScratch *scratch = static_cast<MediaLibraryScanScratch *>(
        heap_caps_calloc(1, sizeof(MediaLibraryScanScratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (scratch == nullptr) {
        ESP_LOGE(TAG, "创建热刷新PSRAM scratch失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "开始USB归还后的Catalog事务热刷新：old_generation=%lu tracks=%u",
        static_cast<unsigned long>(media_catalog_v2_generation()),
        static_cast<unsigned>(media_library_get_count()));
    const esp_err_t ret = media_library_scan_with_scratch(
        scratch, true, out_retired, out_changes, on_scan_event, callback_context);
    media_library_scan_scratch_release(scratch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Catalog事务热刷新失败：%s；继续保留旧Catalog", esp_err_to_name(ret));
        media_catalog_v2_release(out_retired);
        return ret;
    }

    ESP_LOGI(TAG, "Catalog事务热刷新成功：new_generation=%lu tracks=%u",
        static_cast<unsigned long>(media_catalog_v2_generation()),
        static_cast<unsigned>(media_library_get_count()));
    return ESP_OK;
}

bool media_library_is_ready()
{
    return g_ready && media_catalog_v2_ready();
}

size_t media_library_get_count()
{
    const MusicCatalogV2 *catalog = media_catalog_v2_current();
    return media_library_is_ready() && catalog != nullptr ? catalog->track_count : 0;
}

const char *media_library_get_path(size_t index)
{
    MediaTrackViewV2 view = {};
    return media_catalog_v2_get_track_view(index, &view) ? view.path : nullptr;
}

MediaFormat media_library_get_format(size_t index)
{
    MediaTrackViewV2 view = {};
    return media_catalog_v2_get_track_view(index, &view) && view.row != nullptr
        ? view.row->format
        : MediaFormat::Unknown;
}

const char *media_library_get_filename(size_t index)
{
    const char *path = media_library_get_path(index);
    if (path == nullptr) {
        return nullptr;
    }
    const char *slash = strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

bool media_library_copy_display_name(size_t index, char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0) {
        return false;
    }
    buffer[0] = '\0';
    MediaTrackViewV2 view = {};
    if (!media_catalog_v2_get_track_view(index, &view) || view.title == nullptr) {
        return false;
    }
    const size_t title_length = strlen(view.title);
    const size_t copy_length = title_length < buffer_size - 1 ? title_length : buffer_size - 1;
    memcpy(buffer, view.title, copy_length);
    buffer[copy_length] = '\0';
    return true;
}

bool media_library_get_technical_info(size_t index, MediaTechnicalInfo *out_info)
{
    return media_catalog_v2_copy_technical(index, out_info);
}


bool media_library_get_artwork_view(size_t index, MediaArtworkViewV2 *out_view)
{
    return media_catalog_v2_get_artwork_view(index, out_view);
}
