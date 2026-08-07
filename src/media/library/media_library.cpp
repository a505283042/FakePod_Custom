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

static const char *TAG = "音乐库";
static constexpr const char *MUSIC_ROOT = "/sdcard/MUSIC";
static constexpr size_t INITIAL_ENTRY_CAPACITY = 128;
static constexpr size_t INITIAL_PATH_CAPACITY = 16 * 1024;
static constexpr size_t INITIAL_DIR_CAPACITY = 16;
static constexpr size_t LOG_TRACK_LIMIT = 10;

struct MediaEntry
{
    uint32_t path_offset;
    MediaFormat format;
};

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

static void media_library_reset()
{
    if (g_entries != nullptr) {
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

static bool media_library_add_track(const char *path, MediaFormat format)
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
    g_entries[g_entry_count].path_offset = static_cast<uint32_t>(offset);
    g_entries[g_entry_count].format = format;
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

const char *media_library_format_name(MediaFormat format)
{
    switch (format) {
        case MediaFormat::Unknown: return "未知";
        case MediaFormat::MP3: return "MP3";
        case MediaFormat::FLAC: return "FLAC";
        case MediaFormat::WAV: return "WAV";
        case MediaFormat::NSF: return "NSF";
        case MediaFormat::NSFE: return "NSFE";
    }
    return "未知";
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
    DirectoryStack stack = {};
    if (!directory_stack_push(&stack, MUSIC_ROOT)) {
        ESP_LOGE(TAG, "创建目录扫描栈失败");
        directory_stack_destroy(&stack);
        return ESP_ERR_NO_MEM;
    }

    size_t directory_count = 0;
    size_t format_count[6] = {};
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
                    if (!media_library_add_track(full_path, format)) {
                        out_of_memory = true;
                        heap_caps_free(full_path);
                        break;
                    }
                    format_count[static_cast<size_t>(format)]++;
                    if (g_entry_count <= LOG_TRACK_LIMIT) {
                        ESP_LOGI(TAG, "发现歌曲[%u] %s [%s]",
                            static_cast<unsigned>(g_entry_count),
                            full_path,
                            media_library_format_name(format));
                    }
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
        media_library_reset();
        return ESP_ERR_NO_MEM;
    }
    if (root_missing) {
        ESP_LOGW(TAG, "音乐目录不存在：%s，音乐库保持为空", MUSIC_ROOT);
    }
    media_library_sort_entries();
    g_ready = true;
    const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - start_us) / 1000);
    ESP_LOGI(TAG, "扫描完成：歌曲=%u，目录=%u，耗时=%u ms",
        static_cast<unsigned>(g_entry_count),
        static_cast<unsigned>(directory_count),
        static_cast<unsigned>(elapsed_ms));
    ESP_LOGI(TAG, "格式统计：MP3=%u，FLAC=%u，WAV=%u，NSF=%u，NSFE=%u",
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::MP3)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::FLAC)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::WAV)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::NSF)]),
        static_cast<unsigned>(format_count[static_cast<size_t>(MediaFormat::NSFE)]));
    ESP_LOGI(TAG, "索引占用 PSRAM：条目=%u 字节，路径=%u/%u 字节",
        static_cast<unsigned>(g_entry_capacity * sizeof(MediaEntry)),
        static_cast<unsigned>(g_path_size),
        static_cast<unsigned>(g_path_capacity));
    return ESP_OK;
}

bool media_library_is_ready()
{
    return g_ready;
}

size_t media_library_get_count()
{
    return g_entry_count;
}

const char *media_library_get_path(size_t index)
{
    if (index >= g_entry_count || g_path_pool == nullptr) {
        return nullptr;
    }
    return g_path_pool + g_entries[index].path_offset;
}

MediaFormat media_library_get_format(size_t index)
{
    if (index >= g_entry_count) {
        return MediaFormat::Unknown;
    }
    return g_entries[index].format;
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
    const char *filename = media_library_get_filename(index);
    if (filename == nullptr) {
        return false;
    }
    const char *extension = strrchr(filename, '.');
    size_t length = extension != nullptr && extension != filename
        ? static_cast<size_t>(extension - filename)
        : strlen(filename);
    if (length >= buffer_size) {
        length = buffer_size - 1;
    }
    memcpy(buffer, filename, length);
    buffer[length] = '\0';
    return true;
}
