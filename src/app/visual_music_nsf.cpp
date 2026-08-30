#include "visual_music_nsf.h"

#include <stdio.h>
#include <string.h>

#include "drivers/storage/storage_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "电子音流NSF";

namespace VisualMusicNsf
{
namespace
{

static constexpr size_t kHeaderBytes = 128U;
static constexpr size_t kReadChunkBytes = 4096U;
static constexpr TickType_t kStorageLockTimeout = pdMS_TO_TICKS(30);
static constexpr uint32_t kTaskStack = 4096U;
static constexpr UBaseType_t kTaskPriority = 1U;
static constexpr BaseType_t kTaskCore = 0;

struct TaskArgs
{
    char *path = nullptr; // PSRAM
    uint32_t generation = 0U;
};

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_generation = 1U;
static bool g_initialized = false;
static bool g_result_pending = false;
static LoadResult g_pending_result = {};

static bool generation_current(uint32_t generation)
{
    bool current = false;
    portENTER_CRITICAL(&g_mux);
    current = generation == g_generation;
    portEXIT_CRITICAL(&g_mux);
    return current;
}

static uint16_t read_le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8U);
}

static void copy_header_string(char out[33], const uint8_t *src)
{
    memcpy(out, src, 32U);
    out[32] = '\0';
    // 规范字段经常以 NUL 或空格填充，统一去掉尾部填充，UI 不显示多余空白。
    size_t length = strnlen(out, 32U);
    while (length > 0U && (out[length - 1U] == ' ' || out[length - 1U] == '\0')) {
        out[--length] = '\0';
    }
}

static esp_err_t parse_image(uint8_t *data, size_t size, Image *out)
{
    if (data == nullptr || out == nullptr || size <= kHeaderBytes) return ESP_ERR_INVALID_ARG;
    static constexpr uint8_t kNsfMagic[5] = {'N', 'E', 'S', 'M', 0x1AU};
    if (memcmp(data, kNsfMagic, sizeof(kNsfMagic)) != 0) {
        // NSFE 采用独立 chunk 容器，不能按 128-byte NESM Header 猜读。
        if (size >= 4U && memcmp(data, "NSFE", 4U) == 0) return ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t track_count = data[0x06U];
    if (track_count == 0U) return ESP_ERR_INVALID_RESPONSE;

    Image image = {};
    image.file_data = data;
    image.file_size = size;
    image.prg_data = data + kHeaderBytes;
    image.prg_size = size - kHeaderBytes;
    image.version = data[0x05U];
    image.track_count = track_count;
    const uint8_t initial_track_1based = data[0x07U];
    image.initial_track = initial_track_1based > 0U ? initial_track_1based - 1U : 0U;
    if (image.initial_track >= image.track_count) image.initial_track = 0U;
    image.load_address = read_le16(data + 0x08U);
    image.init_address = read_le16(data + 0x0AU);
    image.play_address = read_le16(data + 0x0CU);
    copy_header_string(image.song_name, data + 0x0EU);
    copy_header_string(image.artist, data + 0x2EU);
    copy_header_string(image.copyright, data + 0x4EU);
    image.ntsc_speed_us = read_le16(data + 0x6EU);
    memcpy(image.banks, data + 0x70U, sizeof(image.banks));
    image.pal_speed_us = read_le16(data + 0x78U);
    image.pal_ntsc_bits = data[0x7AU];
    image.expansion_chips = data[0x7BU];
    *out = image;
    return ESP_OK;
}

static esp_err_t load_file(uint32_t generation, const char *path, uint8_t **out_data, size_t *out_size)
{
    if (path == nullptr || out_data == nullptr || out_size == nullptr) return ESP_ERR_INVALID_ARG;
    *out_data = nullptr;
    *out_size = 0U;

    FILE *file = nullptr;
    long file_size = 0L;
    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (!guard) return ESP_ERR_TIMEOUT;
        file = fopen(path, "rb");
        if (file == nullptr) return ESP_ERR_NOT_FOUND;
        if (fseek(file, 0L, SEEK_END) != 0) {
            fclose(file);
            return ESP_FAIL;
        }
        file_size = ftell(file);
        if (file_size <= static_cast<long>(kHeaderBytes) || fseek(file, 0L, SEEK_SET) != 0) {
            fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    const size_t bytes = static_cast<size_t>(file_size);
    uint8_t *data = static_cast<uint8_t *>(heap_caps_malloc(
        bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard && file != nullptr) fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0U;
    esp_err_t ret = ESP_OK;
    while (offset < bytes && generation_current(generation)) {
        const size_t wanted = (bytes - offset) < kReadChunkBytes ? bytes - offset : kReadChunkBytes;
        size_t got = 0U;
        {
            StorageSdLockGuard guard(kStorageLockTimeout);
            if (!guard) {
                vTaskDelay(1);
                continue;
            }
            got = fread(data + offset, 1U, wanted, file);
            if (got == 0U) ret = feof(file) ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
        }
        if (ret != ESP_OK) break;
        offset += got;
        taskYIELD();
    }
    if (!generation_current(generation)) ret = ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK && offset != bytes) ret = ESP_ERR_INVALID_SIZE;

    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard && file != nullptr) fclose(file);
    }

    if (ret != ESP_OK) {
        heap_caps_free(data);
        return ret;
    }
    *out_data = data;
    *out_size = bytes;
    return ESP_OK;
}

static void clear_pending_result()
{
    LoadResult pending = {};
    bool has_pending = false;
    portENTER_CRITICAL(&g_mux);
    if (g_result_pending) {
        pending = g_pending_result;
        g_pending_result = {};
        g_result_pending = false;
        has_pending = true;
    }
    portEXIT_CRITICAL(&g_mux);
    if (has_pending) release_image(&pending.image);
}

static bool publish_result(LoadResult *result)
{
    if (result == nullptr) return false;
    bool accepted = false;
    portENTER_CRITICAL(&g_mux);
    if (result->generation == g_generation && !g_result_pending) {
        g_pending_result = *result;
        g_result_pending = true;
        result->image = {};
        accepted = true;
    }
    portEXIT_CRITICAL(&g_mux);
    return accepted;
}

static void load_task(void *arg)
{
    TaskArgs *args = static_cast<TaskArgs *>(arg);
    if (args == nullptr || args->path == nullptr) {
        if (args != nullptr) heap_caps_free(args);
        vTaskDelete(nullptr);
        return;
    }

    LoadResult result = {};
    result.state = LoadState::Failed;
    result.generation = args->generation;
    result.result = ESP_FAIL;

    uint8_t *file_data = nullptr;
    size_t file_size = 0U;
    esp_err_t ret = load_file(args->generation, args->path, &file_data, &file_size);
    if (ret == ESP_OK && generation_current(args->generation)) {
        ret = parse_image(file_data, file_size, &result.image);
        if (ret != ESP_OK) {
            heap_caps_free(file_data);
            file_data = nullptr;
        } else {
            file_data = nullptr; // 所有权已转移给 result.image
        }
    }
    if (file_data != nullptr) heap_caps_free(file_data);

    result.result = ret;
    result.state = ret == ESP_OK ? LoadState::Ready : LoadState::Failed;
    const Image log_image = result.image;
    if (publish_result(&result)) {
        if (ret == ESP_OK) {
            ESP_LOGI(
                TAG,
                "NSF解析完成：version=%u tracks=%u initial=%u load=%04X init=%04X play=%04X prg=%uB expansion=0x%02X title=%s artist=%s",
                static_cast<unsigned>(log_image.version),
                static_cast<unsigned>(log_image.track_count),
                static_cast<unsigned>(log_image.initial_track + 1U),
                static_cast<unsigned>(log_image.load_address),
                static_cast<unsigned>(log_image.init_address),
                static_cast<unsigned>(log_image.play_address),
                static_cast<unsigned>(log_image.prg_size),
                static_cast<unsigned>(log_image.expansion_chips),
                log_image.song_name[0] != '\0' ? log_image.song_name : "(untitled)",
                log_image.artist[0] != '\0' ? log_image.artist : "(unknown)");
        } else {
            ESP_LOGW(TAG, "NSF解析失败：path=%s ret=%s", args->path, esp_err_to_name(ret));
        }
    } else {
        release_image(&result.image);
    }

    heap_caps_free(args->path);
    heap_caps_free(args);
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t init()
{
    if (g_initialized) return ESP_OK;
    clear_pending_result();
    g_initialized = true;
    return ESP_OK;
}

esp_err_t start(const char *path)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    if (path == nullptr || path[0] == '\0') return ESP_ERR_INVALID_ARG;

    TaskArgs *args = static_cast<TaskArgs *>(heap_caps_calloc(
        1U,
        sizeof(TaskArgs),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    const size_t path_bytes = strlen(path) + 1U;
    char *path_copy = static_cast<char *>(heap_caps_malloc(
        path_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (args == nullptr || path_copy == nullptr) {
        if (args != nullptr) heap_caps_free(args);
        if (path_copy != nullptr) heap_caps_free(path_copy);
        return ESP_ERR_NO_MEM;
    }
    memcpy(path_copy, path, path_bytes);

    portENTER_CRITICAL(&g_mux);
    ++g_generation;
    if (g_generation == 0U) ++g_generation;
    args->generation = g_generation;
    portEXIT_CRITICAL(&g_mux);
    args->path = path_copy;
    clear_pending_result();

    const BaseType_t created = xTaskCreatePinnedToCore(
        load_task,
        "NsfLoadTask",
        kTaskStack,
        args,
        kTaskPriority,
        nullptr,
        kTaskCore);
    if (created != pdPASS) {
        heap_caps_free(path_copy);
        heap_caps_free(args);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void cancel()
{
    portENTER_CRITICAL(&g_mux);
    ++g_generation;
    if (g_generation == 0U) ++g_generation;
    portEXIT_CRITICAL(&g_mux);
    clear_pending_result();
}

bool take_result(LoadResult *out_result)
{
    if (out_result == nullptr || !g_initialized) return false;
    bool available = false;
    portENTER_CRITICAL(&g_mux);
    if (g_result_pending && g_pending_result.generation == g_generation) {
        *out_result = g_pending_result;
        g_pending_result = {};
        g_result_pending = false;
        available = true;
    }
    portEXIT_CRITICAL(&g_mux);
    return available;
}

void release_image(Image *image)
{
    if (image == nullptr) return;
    if (image->file_data != nullptr) heap_caps_free(image->file_data);
    *image = {};
}

} // namespace VisualMusicNsf
