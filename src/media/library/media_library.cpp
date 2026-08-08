#include "media_library.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdcard.h"
#include "media_index_store.h"
#include "media_catalog_v2.h"
#include "media_groups_v2.h"
#include "media_catalog_store_v2.h"
#include "media_probe.h"
#include "media_metadata.h"
#include "app_diag_config.h"

static const char *TAG = "音乐库";
static constexpr const char *MUSIC_ROOT = "/sdcard/MUSIC";
static constexpr size_t INITIAL_ENTRY_CAPACITY = 128;
static constexpr size_t INITIAL_PATH_CAPACITY = 16 * 1024;
static constexpr size_t INITIAL_DIR_CAPACITY = 16;
#if APP_DIAG_LIBRARY_ITEMS || APP_DIAG_LIBRARY_METADATA
static constexpr size_t LOG_TRACK_LIMIT = 10;
#endif

using MediaEntry = MediaIndexRecord;

struct DirectoryStack
{
    char **items;
    size_t count;
    size_t capacity;
};

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

static void media_library_reset()
{
    media_library_release_build_buffers();
    g_ready = false;
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
    MediaMetadataBuildV2 *metadata_build
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
    g_entry_count++;
    return true;
}

static bool directory_stack_push(DirectoryStack *stack, const char *path)
{
    if (stack == nullptr || path == nullptr) {
        return false;
    }
    if (stack->count == stack->capacity) {
        size_t next_capacity = stack->capacity == 0 ? INITIAL_DIR_CAPACITY : stack->capacity * 2;
        void *next = media_psram_realloc(stack->items, next_capacity * sizeof(char *));
        if (next == nullptr) {
            return false;
        }
        stack->items = static_cast<char **>(next);
        stack->capacity = next_capacity;
    }
    char *copy = media_psram_strdup(path);
    if (copy == nullptr) {
        return false;
    }
    stack->items[stack->count++] = copy;
    return true;
}

static char *directory_stack_pop(DirectoryStack *stack)
{
    if (stack == nullptr || stack->count == 0) {
        return nullptr;
    }
    return stack->items[--stack->count];
}

static void directory_stack_destroy(DirectoryStack *stack)
{
    if (stack == nullptr) {
        return;
    }
    while (stack->count > 0) {
        char *path = directory_stack_pop(stack);
        if (path != nullptr) {
            heap_caps_free(path);
        }
    }
    if (stack->items != nullptr) {
        heap_caps_free(stack->items);
    }
    stack->items = nullptr;
    stack->count = 0;
    stack->capacity = 0;
}

static char *media_library_join_path(const char *directory, const char *name)
{
    if (directory == nullptr || name == nullptr) {
        return nullptr;
    }
    const size_t dir_length = strlen(directory);
    const size_t name_length = strlen(name);
    const bool need_slash = dir_length > 0 && directory[dir_length - 1] != '/';
    const size_t total = dir_length + (need_slash ? 1 : 0) + name_length + 1;
    char *path = static_cast<char *>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (path == nullptr) {
        return nullptr;
    }
    snprintf(path, total, "%s%s%s", directory, need_slash ? "/" : "", name);
    return path;
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
    ESP_LOGI(TAG, "排序完成：按完整 UTF-8 路径排序，共 %u 首", static_cast<unsigned>(g_entry_count));
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

esp_err_t media_library_scan()
{
    if (!sdcard_is_mounted()) {
        ESP_LOGE(TAG, "TF 卡尚未挂载，无法扫描音乐库");
        return ESP_ERR_INVALID_STATE;
    }
    media_library_reset();
    ESP_LOGI(TAG, "开始递归扫描：%s", MUSIC_ROOT);
    const int64_t start_us = esp_timer_get_time();

    // Stage 10.2 优先加载正式 V2 Catalog/Manifest。第一次从 Stage 10.0 升级时，
    // 若 V2 尚不存在则只把 V1 当作迁移源复用技术信息，之后不再写回 V1。
    MediaCatalogSnapshotV2 previous_v2 = {};
    const bool have_previous_v2 = media_catalog_store_v2_load(&previous_v2) == ESP_OK;
    MediaIndexSnapshot previous_v1 = {};
    const bool have_previous_v1 = !have_previous_v2 && media_index_store_load(&previous_v1) == ESP_OK;
    if (have_previous_v1) {
        ESP_LOGI(TAG, "检测到 Stage 10.0 V1 索引，本次迁移复用技术信息并生成 V2 Catalog");
    } else if (!have_previous_v2) {
        ESP_LOGI(TAG, "未找到可复用索引，本次将首次建立 MusicCatalogV2");
    }

    DirectoryStack stack = {};
    if (!directory_stack_push(&stack, MUSIC_ROOT)) {
        ESP_LOGE(TAG, "创建目录扫描栈失败");
        directory_stack_destroy(&stack);
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        return ESP_ERR_NO_MEM;
    }

    size_t directory_count = 0;
    size_t format_count[static_cast<size_t>(MediaFormat::NSFE) + 1U] = {};
    size_t reused_count = 0;
    size_t probed_count = 0;
    size_t probe_failed_count = 0;
    size_t metadata_reused_count = 0;
    size_t metadata_scanned_count = 0;
    size_t metadata_failed_count = 0;
    size_t full_track_reused_count = 0;
    bool out_of_memory = false;
    bool root_missing = false;

    while (stack.count > 0 && !out_of_memory) {
        char *directory = directory_stack_pop(&stack);
        if (directory == nullptr) {
            break;
        }
        DIR *dir = opendir(directory);
        if (dir == nullptr) {
            if (strcmp(directory, MUSIC_ROOT) == 0) {
                root_missing = true;
            } else {
                ESP_LOGW(TAG, "无法打开目录，已跳过：%s", directory);
            }
            heap_caps_free(directory);
            continue;
        }
        directory_count++;
        struct dirent *entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char *full_path = media_library_join_path(directory, entry->d_name);
            if (full_path == nullptr) {
                out_of_memory = true;
                break;
            }
            struct stat info = {};
            if (stat(full_path, &info) != 0) {
                ESP_LOGW(TAG, "无法读取文件属性，已跳过：%s", full_path);
                heap_caps_free(full_path);
                continue;
            }
            if (S_ISDIR(info.st_mode)) {
                if (!directory_stack_push(&stack, full_path)) {
                    out_of_memory = true;
                }
                heap_caps_free(full_path);
                if (out_of_memory) {
                    break;
                }
                continue;
            }
            if (S_ISREG(info.st_mode)) {
                MediaFormat format = MediaFormat::Unknown;
                if (media_library_detect_format(entry->d_name, &format)) {
                    MediaTechnicalInfo technical = {};
                    MediaMetadataBuildV2 *metadata_build = nullptr;
                    bool reused = false;
                    bool metadata_reused = false;
                    bool v2_signature_match = false;
                    const TrackRowV2 *old_v2_track = nullptr;

                    const bool deep_probe_format =
                        format == MediaFormat::FLAC || format == MediaFormat::MP3;

                    if (have_previous_v2) {
                        const MediaManifestRecordV2 *old_manifest = nullptr;
                        if (media_catalog_store_v2_find(
                                &previous_v2, full_path, &old_v2_track, &old_manifest) &&
                            old_v2_track != nullptr && old_manifest != nullptr &&
                            old_v2_track->format == format && old_manifest->format == format &&
                            old_manifest->file_size_bytes == static_cast<uint64_t>(info.st_size) &&
                            old_manifest->modified_time == static_cast<int64_t>(info.st_mtime)) {
                            v2_signature_match = true;
                            if (!deep_probe_format || (old_v2_track->technical.flags & MEDIA_TECH_PARSED) != 0U) {
                                technical = old_v2_track->technical;
                                reused = true;
                                reused_count++;
                            }
                            if (deep_probe_format &&
                                (old_v2_track->metadata_flags & MEDIA_TRACK_META_SCANNED_V2) != 0U) {
                                metadata_build = static_cast<MediaMetadataBuildV2 *>(
                                    heap_caps_calloc(1, sizeof(MediaMetadataBuildV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                );
                                if (metadata_build == nullptr) {
                                    out_of_memory = true;
                                } else {
                                    const uint32_t old_track_index = static_cast<uint32_t>(old_v2_track - previous_v2.catalog.tracks);
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
                    if (out_of_memory) {
                        heap_caps_free(full_path);
                        break;
                    }

                    if (!reused && deep_probe_format) {
                        const esp_err_t probe_ret = media_probe_file(full_path, format, &technical);
                        if (probe_ret == ESP_OK) {
                            probed_count++;
                        } else {
                            // 文件仍进入曲库；只是不带深度技术信息，避免单个坏文件阻断全库扫描。
                            technical = {};
                            probe_failed_count++;
                        }
                    }

                    if (deep_probe_format && !metadata_reused) {
                        metadata_build = static_cast<MediaMetadataBuildV2 *>(
                            heap_caps_calloc(1, sizeof(MediaMetadataBuildV2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                        );
                        if (metadata_build == nullptr) {
                            out_of_memory = true;
                            heap_caps_free(full_path);
                            break;
                        }
                        const esp_err_t metadata_ret = media_metadata_scan_file_v2(
                            full_path, format, static_cast<uint64_t>(info.st_size), metadata_build
                        );
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
                                heap_caps_free(full_path);
                                break;
                            }
                        }
                    }

                    if (v2_signature_match && (!deep_probe_format || (reused && metadata_reused))) {
                        full_track_reused_count++;
                    }

                    if (!media_library_add_track(full_path, format, info, technical, metadata_build)) {
                        if (metadata_build != nullptr) {
                            media_metadata_build_release(metadata_build);
                            heap_caps_free(metadata_build);
                        }
                        out_of_memory = true;
                        heap_caps_free(full_path);
                        break;
                    }
                    format_count[static_cast<size_t>(format)]++;
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
                }
            }
            heap_caps_free(full_path);
        }
        closedir(dir);
        heap_caps_free(directory);
    }
    directory_stack_destroy(&stack);

    if (out_of_memory) {
        ESP_LOGE(TAG, "扫描过程中 PSRAM 不足，音乐库未完成");
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset();
        return ESP_ERR_NO_MEM;
    }
    if (root_missing) {
        ESP_LOGW(TAG, "音乐目录不存在：%s，音乐库保持为空", MUSIC_ROOT);
    }
    media_library_sort_entries();
    if (!media_library_repack_sorted_path_pool()) {
        ESP_LOGE(TAG, "排序后重建确定性路径池失败");
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset();
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
        MusicCatalogV2 reused_catalog = previous_v2.catalog;
        previous_v2.catalog = {};
        const uint32_t source_crc = previous_v2.index_crc32;
        const esp_err_t publish_ret = media_catalog_v2_publish(&reused_catalog, source_crc);
        if (publish_ret != ESP_OK) {
            media_catalog_v2_release(&reused_catalog);
            media_catalog_store_v2_release(&previous_v2);
            media_index_store_release(&previous_v1);
            media_library_reset();
            return publish_ret;
        }
        ESP_LOGI(TAG, "V2 Catalog 全量命中 Manifest，直接复用已校验运行时目录，不重建、不写盘");
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_release_build_buffers();
        g_ready = true;

        const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000);
        ESP_LOGI(TAG, "扫描完成：歌曲=%u，目录=%u，耗时=%u ms，技术复用=%u，新技术解析=%u，技术失败=%u",
            static_cast<unsigned>(final_track_count),
            static_cast<unsigned>(directory_count),
            static_cast<unsigned>(elapsed_ms),
            static_cast<unsigned>(reused_count),
            static_cast<unsigned>(probed_count),
            static_cast<unsigned>(probe_failed_count));
        ESP_LOGI(TAG, "Metadata统计：复用=%u，新解析=%u，失败=%u",
            static_cast<unsigned>(metadata_reused_count),
            static_cast<unsigned>(metadata_scanned_count),
            static_cast<unsigned>(metadata_failed_count));
        ESP_LOGI(TAG, "格式统计：MP3=%u，FLAC=%u，WAV=%u，NSF=%u，NSFE=%u",
            static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::MP3)]),
            static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::FLAC)]),
            static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::WAV)]),
            static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::NSF)]),
            static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::NSFE)]));
        const MusicCatalogV2 *published = media_catalog_v2_current();
        ESP_LOGI(TAG, "MusicCatalogV2 PSRAM：tracks=%luB artists=%luB albums=%luB artist_refs=%luB lyrics_refs=%luB strings=%luB groups=%uB generation=%lu",
            static_cast<unsigned long>(final_track_bytes),
            static_cast<unsigned long>(final_artist_bytes),
            static_cast<unsigned long>(final_album_bytes),
            static_cast<unsigned long>(published != nullptr ? published->track_artist_ref_count * sizeof(TrackArtistRefV2) : 0U),
            static_cast<unsigned long>(published != nullptr ? published->lyrics_ref_count * sizeof(LyricsRefV2) : 0U),
            static_cast<unsigned long>(final_string_bytes),
            static_cast<unsigned>(media_groups_v2_psram_bytes(published)),
            static_cast<unsigned long>(media_catalog_v2_generation()));
        return ESP_OK;
    }

    // 扫描缓冲只负责构建。正式运行时使用精确尺寸的 MusicCatalogV2，
    // 从这里开始 UI/Player 不再依赖扫描期 g_entries/path_pool。
    MusicCatalogV2 next_catalog = {};
    esp_err_t catalog_ret = media_catalog_v2_build_from_index_records(
        g_entries, g_entry_count, g_path_pool, g_path_size, &next_catalog
    );
    if (catalog_ret != ESP_OK) {
        ESP_LOGE(TAG, "构建 MusicCatalogV2 失败：%s", esp_err_to_name(catalog_ret));
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset();
        return catalog_ret;
    }

    // Catalog/Manifest 是缓存：写盘失败不阻断本次内存曲库，但发布前必须通过 semantic validation。
    uint32_t catalog_crc = 0;
    const esp_err_t index_ret = media_catalog_store_v2_commit(
        &next_catalog,
        g_entries,
        g_entry_count,
        have_previous_v2 ? &previous_v2 : nullptr,
        &catalog_crc
    );
    if (index_ret != ESP_OK) {
        ESP_LOGW(TAG, "V2 Catalog 持久化失败，本次继续使用内存目录：%s", esp_err_to_name(index_ret));
    }

    const uint32_t final_track_count = next_catalog.track_count;
    const uint32_t final_string_bytes = next_catalog.pool.size;
    const uint32_t final_track_bytes = next_catalog.track_count * sizeof(TrackRowV2);
    const uint32_t final_artist_bytes = next_catalog.artist_count * sizeof(ArtistRowV2);
    const uint32_t final_album_bytes = next_catalog.album_count * sizeof(AlbumRowV2);
    catalog_ret = media_catalog_v2_publish(&next_catalog, catalog_crc);
    if (catalog_ret != ESP_OK) {
        ESP_LOGE(TAG, "发布 MusicCatalogV2 失败：%s", esp_err_to_name(catalog_ret));
        media_catalog_v2_release(&next_catalog);
        media_catalog_store_v2_release(&previous_v2);
        media_index_store_release(&previous_v1);
        media_library_reset();
        return catalog_ret;
    }

    media_catalog_store_v2_release(&previous_v2);
    media_index_store_release(&previous_v1);
    media_library_release_build_buffers();
    g_ready = true;

    const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000);
    ESP_LOGI(TAG, "扫描完成：歌曲=%u，目录=%u，耗时=%u ms，技术复用=%u，新技术解析=%u，技术失败=%u",
        static_cast<unsigned>(final_track_count),
        static_cast<unsigned>(directory_count),
        static_cast<unsigned>(elapsed_ms),
        static_cast<unsigned>(reused_count),
        static_cast<unsigned>(probed_count),
        static_cast<unsigned>(probe_failed_count));
    ESP_LOGI(TAG, "Metadata统计：复用=%u，新解析=%u，失败=%u",
        static_cast<unsigned>(metadata_reused_count),
        static_cast<unsigned>(metadata_scanned_count),
        static_cast<unsigned>(metadata_failed_count));
    ESP_LOGI(TAG, "格式统计：MP3=%u，FLAC=%u，WAV=%u，NSF=%u，NSFE=%u",
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::MP3)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::FLAC)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::WAV)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::NSF)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::NSFE)]));
    const MusicCatalogV2 *published = media_catalog_v2_current();
    ESP_LOGI(TAG, "MusicCatalogV2 PSRAM：tracks=%luB artists=%luB albums=%luB artist_refs=%luB lyrics_refs=%luB strings=%luB groups=%uB generation=%lu",
        static_cast<unsigned long>(final_track_bytes),
        static_cast<unsigned long>(final_artist_bytes),
        static_cast<unsigned long>(final_album_bytes),
        static_cast<unsigned long>(published != nullptr ? published->track_artist_ref_count * sizeof(TrackArtistRefV2) : 0U),
        static_cast<unsigned long>(published != nullptr ? published->lyrics_ref_count * sizeof(LyricsRefV2) : 0U),
        static_cast<unsigned long>(final_string_bytes),
        static_cast<unsigned>(media_groups_v2_psram_bytes(published)),
        static_cast<unsigned long>(media_catalog_v2_generation()));
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
