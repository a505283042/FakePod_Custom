#include "fallback_cover_images.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "storage_io.h"
#include "system_paths.h"

namespace {

static const char *TAG = "替补封面";

static constexpr uint16_t kFallbackWidth = 460U;
static constexpr uint16_t kFallbackHeight = 460U;
static constexpr size_t kFallbackRgb565Bytes =
    static_cast<size_t>(kFallbackWidth) * static_cast<size_t>(kFallbackHeight) * 2U;
static constexpr size_t kMaxCompressedBytes = 1024U * 1024U;
static constexpr size_t kPsramSafetyReserveBytes = 512U * 1024U;
static constexpr uint8_t kSlotCount = 2U;

struct FallbackCoverSlot
{
    uint8_t *rgb565 = nullptr;
    size_t data_size = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint32_t revision = 0U;
    uint16_t pin_count = 0U;
};

static FallbackCoverSlot g_slots[kSlotCount] = {};
static uint32_t g_next_revision = 1U;
static portMUX_TYPE g_slots_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t kind_to_slot(FallbackCoverImageKind kind)
{
    return kind == FallbackCoverImageKind::Cassette ? 1U : 0U;
}

static const char *kind_path(FallbackCoverImageKind kind)
{
    return kind == FallbackCoverImageKind::Cassette
        ? SystemPaths::kNoCoverCassette
        : SystemPaths::kNoCoverArtwork;
}

static const char *kind_name(FallbackCoverImageKind kind)
{
    return kind == FallbackCoverImageKind::Cassette ? "磁带标签" : "封面视图";
}

static bool read_file_to_psram(const char *path, uint8_t **out_data, size_t *out_size)
{
    if (path == nullptr || out_data == nullptr || out_size == nullptr) return false;
    *out_data = nullptr;
    *out_size = 0U;

    struct stat info = {};
    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (!guard || stat(path, &info) != 0 || info.st_size <= 0) {
            return false;
        }
    }

    const uint64_t size64 = static_cast<uint64_t>(info.st_size);
    if (size64 == 0U || size64 > kMaxCompressedBytes || size64 > SIZE_MAX) {
        ESP_LOGE(TAG, "替补JPG大小非法：%s bytes=%llu",
            path, static_cast<unsigned long long>(size64));
        return false;
    }

    const size_t size = static_cast<size_t>(size64);
    uint8_t *data = static_cast<uint8_t *>(
        heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) return false;

    FILE *file = nullptr;
    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (!guard) {
            heap_caps_free(data);
            return false;
        }
        file = fopen(path, "rb");
    }
    if (file == nullptr) {
        heap_caps_free(data);
        return false;
    }

    size_t loaded = 0U;
    static constexpr size_t kReadChunk = 16U * 1024U;
    while (loaded < size) {
        const size_t remaining = size - loaded;
        const size_t chunk = remaining < kReadChunk ? remaining : kReadChunk;
        size_t got = 0U;
        {
            StorageSdLockGuard guard(portMAX_DELAY);
            if (!guard) break;
            got = fread(data + loaded, 1U, chunk, file);
        }
        if (got != chunk) break;
        loaded += got;
    }

    {
        StorageSdLockGuard guard(portMAX_DELAY);
        if (guard) fclose(file);
    }

    if (loaded != size) {
        heap_caps_free(data);
        return false;
    }

    *out_data = data;
    *out_size = size;
    return true;
}

static bool decode_jpeg_460(const uint8_t *data, size_t size, uint8_t **out_rgb565)
{
    if (data == nullptr || size == 0U || out_rgb565 == nullptr) return false;
    *out_rgb565 = nullptr;

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;

    jpeg_dec_handle_t decoder = nullptr;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || decoder == nullptr) return false;

    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t info = {};
    io.inbuf = const_cast<uint8_t *>(data);
    io.inbuf_len = static_cast<int>(size);

    jpeg_error_t ret = jpeg_dec_parse_header(decoder, &io, &info);
    if (ret != JPEG_ERR_OK || info.width != kFallbackWidth || info.height != kFallbackHeight) {
        ESP_LOGE(TAG, "替补JPG尺寸必须为460x460：实际=%dx%d jpeg_ret=%d",
            info.width, info.height, static_cast<int>(ret));
        jpeg_dec_close(decoder);
        return false;
    }

    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram <= kFallbackRgb565Bytes + kPsramSafetyReserveBytes) {
        ESP_LOGE(TAG, "替补JPG解码PSRAM不足：free=%uKB need=%uKB reserve=%uKB",
            static_cast<unsigned>(free_psram / 1024U),
            static_cast<unsigned>(kFallbackRgb565Bytes / 1024U),
            static_cast<unsigned>(kPsramSafetyReserveBytes / 1024U));
        jpeg_dec_close(decoder);
        return false;
    }

    uint8_t *rgb565 = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16U, kFallbackRgb565Bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (rgb565 == nullptr) {
        jpeg_dec_close(decoder);
        return false;
    }

    io.outbuf = rgb565;
    io.out_size = static_cast<int>(kFallbackRgb565Bytes);
    ret = jpeg_dec_process(decoder, &io);
    jpeg_dec_close(decoder);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "替补JPG解码失败：jpeg_ret=%d", static_cast<int>(ret));
        heap_caps_free(rgb565);
        return false;
    }

    *out_rgb565 = rgb565;
    return true;
}

static bool load_kind_rgb565(FallbackCoverImageKind kind, uint8_t **out_rgb565)
{
    if (out_rgb565 == nullptr) return false;
    *out_rgb565 = nullptr;

    const char *path = kind_path(kind);
    uint8_t *compressed = nullptr;
    size_t compressed_size = 0U;
    if (!read_file_to_psram(path, &compressed, &compressed_size)) {
        ESP_LOGW(TAG, "%s替补JPG读取失败：%s", kind_name(kind), path);
        return false;
    }

    uint8_t *rgb565 = nullptr;
    const bool decoded = decode_jpeg_460(compressed, compressed_size, &rgb565);
    heap_caps_free(compressed);
    if (!decoded || rgb565 == nullptr) {
        ESP_LOGW(TAG, "%s替补JPG不可用：%s；请使用460x460 Baseline JPG",
            kind_name(kind), path);
        return false;
    }

    *out_rgb565 = rgb565;
    ESP_LOGI(TAG, "%s替补JPG已准备：%s compressed=%uB RGB565=%uB PSRAM",
        kind_name(kind), path,
        static_cast<unsigned>(compressed_size),
        static_cast<unsigned>(kFallbackRgb565Bytes));
    return true;
}

}  // namespace

bool fallback_cover_image_acquire(FallbackCoverImageKind kind, FallbackCoverImageLease *out_lease)
{
    if (out_lease == nullptr) return false;
    *out_lease = {};

    const uint8_t slot_index = kind_to_slot(kind);
    if (slot_index >= kSlotCount) return false;

    // 快路径只在短临界区内增加 pin；JPEG 读盘/解码绝不持锁。
    portENTER_CRITICAL(&g_slots_mux);
    FallbackCoverSlot &slot = g_slots[slot_index];
    if (slot.rgb565 != nullptr) {
        if (slot.pin_count == UINT16_MAX) {
            portEXIT_CRITICAL(&g_slots_mux);
            return false;
        }
        ++slot.pin_count;
        out_lease->rgb565 = slot.rgb565;
        out_lease->data_size = slot.data_size;
        out_lease->width = slot.width;
        out_lease->height = slot.height;
        out_lease->revision = slot.revision;
        out_lease->slot_index = slot_index;
        portEXIT_CRITICAL(&g_slots_mux);
        return true;
    }
    portEXIT_CRITICAL(&g_slots_mux);

    uint8_t *loaded_rgb565 = nullptr;
    if (!load_kind_rgb565(kind, &loaded_rgb565) || loaded_rgb565 == nullptr) return false;

    // 两个 Core 可能同时发现空槽并各自解码。只允许第一个结果安装；
    // 后到者复用已安装槽并释放自己的临时副本。
    uint8_t *redundant_rgb565 = nullptr;
    bool acquired = false;
    portENTER_CRITICAL(&g_slots_mux);
    FallbackCoverSlot &install_slot = g_slots[slot_index];
    if (install_slot.rgb565 == nullptr) {
        install_slot.rgb565 = loaded_rgb565;
        install_slot.data_size = kFallbackRgb565Bytes;
        install_slot.width = kFallbackWidth;
        install_slot.height = kFallbackHeight;
        install_slot.revision = g_next_revision++;
        if (g_next_revision == 0U) g_next_revision = 1U;
        loaded_rgb565 = nullptr;
    }
    if (install_slot.pin_count != UINT16_MAX) {
        ++install_slot.pin_count;
        out_lease->rgb565 = install_slot.rgb565;
        out_lease->data_size = install_slot.data_size;
        out_lease->width = install_slot.width;
        out_lease->height = install_slot.height;
        out_lease->revision = install_slot.revision;
        out_lease->slot_index = slot_index;
        acquired = true;
    }
    redundant_rgb565 = loaded_rgb565;
    portEXIT_CRITICAL(&g_slots_mux);

    if (redundant_rgb565 != nullptr) heap_caps_free(redundant_rgb565);
    return acquired;
}

void fallback_cover_image_release(FallbackCoverImageLease *lease)
{
    if (lease == nullptr) return;
    portENTER_CRITICAL(&g_slots_mux);
    if (lease->slot_index < kSlotCount) {
        FallbackCoverSlot &slot = g_slots[lease->slot_index];
        if (slot.revision == lease->revision && slot.pin_count > 0U) {
            --slot.pin_count;
        }
    }
    portEXIT_CRITICAL(&g_slots_mux);
    *lease = {};
}

void fallback_cover_image_discard_unpinned()
{
    uint8_t *detached[kSlotCount] = {};
    size_t detached_count = 0U;

    // 只在临界区内摘掉指针；heap free 放到锁外，避免长时间关中断。
    portENTER_CRITICAL(&g_slots_mux);
    for (uint8_t i = 0U; i < kSlotCount; ++i) {
        if (g_slots[i].pin_count == 0U && g_slots[i].rgb565 != nullptr) {
            detached[detached_count++] = g_slots[i].rgb565;
            g_slots[i] = {};
        }
    }
    portEXIT_CRITICAL(&g_slots_mux);

    for (size_t i = 0U; i < detached_count; ++i) {
        heap_caps_free(detached[i]);
    }
}

void fallback_cover_image_get_debug_snapshot(FallbackCoverImageDebugSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return;
    *out_snapshot = {};

    portENTER_CRITICAL(&g_slots_mux);
    const FallbackCoverSlot &artwork = g_slots[kind_to_slot(FallbackCoverImageKind::Artwork)];
    const FallbackCoverSlot &cassette = g_slots[kind_to_slot(FallbackCoverImageKind::Cassette)];
    out_snapshot->artwork_bytes = artwork.rgb565 != nullptr ? artwork.data_size : 0U;
    out_snapshot->cassette_bytes = cassette.rgb565 != nullptr ? cassette.data_size : 0U;
    out_snapshot->artwork_pins = artwork.pin_count;
    out_snapshot->cassette_pins = cassette.pin_count;
    portEXIT_CRITICAL(&g_slots_mux);
}
