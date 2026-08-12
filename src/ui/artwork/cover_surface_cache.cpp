#include "cover_surface_cache.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "png.h"

#include "app_diag_config.h"
#include "artwork_loader.h"
#include "board_pins.h"
#include "media_catalog_v2.h"

static const char *TAG = "封面预处理";

#if APP_DIAG_ARTWORK_UI
#define COVER_TRACE(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define COVER_TRACE(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif

static constexpr uint32_t COVER_TASK_STACK_BYTES = 12288U;
static constexpr UBaseType_t COVER_TASK_PRIORITY = 1U;
static constexpr BaseType_t COVER_TASK_CORE = 1;
static constexpr size_t COVER_CACHE_SLOT_COUNT = 2U;
static constexpr size_t COVER_SOURCE_DECODE_BUDGET_BYTES = 4U * 1024U * 1024U;
static constexpr size_t COVER_PSRAM_SAFETY_RESERVE_BYTES = 512U * 1024U;
static constexpr uint32_t COVER_COOPERATIVE_ROW_INTERVAL = 32U;
static constexpr size_t COVER_SURFACE_BYTES =
    static_cast<size_t>(FAKEPOD_LCD_WIDTH) * static_cast<size_t>(FAKEPOD_LCD_HEIGHT) * 2U;

struct CoverRequest
{
    uint32_t request_id = 0;
    uint32_t catalog_generation = 0;
    uint32_t track_index = UINT32_MAX;
};

struct CoverCacheEntry
{
    uint8_t *normal = nullptr;
    uint8_t *dimmed = nullptr;
    size_t size = 0;
    uint32_t catalog_generation = 0;
    uint32_t track_index = UINT32_MAX;
    uint32_t slot_revision = 0;
    uint32_t lru_stamp = 0;
    uint16_t pin_count = 0;
    bool valid = false;
};

struct Rgb888Source
{
    uint8_t *data = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Rgb565Source
{
    uint16_t *data = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct CoverRenderStats
{
    uint32_t decode_ms = 0U;
    uint32_t resample_ms = 0U;
};

// R.20：旧 Overlay 黑层 opacity=150，等价于保留约 105/255 的原图亮度。
// 用 13/32 ~= 0.40625 做 RGB565 通道缩放，视觉接近旧黑层，同时只需乘法+移位。
static constexpr uint32_t COVER_DIM_SCALE_NUM = 13U;
static constexpr uint32_t COVER_DIM_SCALE_SHIFT = 5U;

static QueueHandle_t g_queue = nullptr;
static SemaphoreHandle_t g_submit_mutex = nullptr;
static SemaphoreHandle_t g_cache_mutex = nullptr;
static TaskHandle_t g_task = nullptr;
static volatile bool g_ready = false;

static portMUX_TYPE g_request_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_next_request_id = 1U;
static uint32_t g_latest_request_id = 0U;

static portMUX_TYPE g_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static CoverSurfaceSnapshot g_snapshot = {};

static CoverCacheEntry g_cache[COVER_CACHE_SLOT_COUNT] = {};
static uint32_t g_lru_counter = 1U;
static uint32_t g_next_slot_revision = 1U;

// R.36：取消第三张 wire-order Surface。DirectPresent 统一从 native RGB565 在线 byte-swap。

static uint32_t cover_next_request_id()
{
    portENTER_CRITICAL(&g_request_mux);
    const uint32_t id = g_next_request_id++;
    if (g_next_request_id == 0U) g_next_request_id = 1U;
    portEXIT_CRITICAL(&g_request_mux);
    return id;
}

static uint32_t cover_latest_request_id()
{
    portENTER_CRITICAL(&g_request_mux);
    const uint32_t id = g_latest_request_id;
    portEXIT_CRITICAL(&g_request_mux);
    return id;
}

static void cover_set_latest_request_id(uint32_t request_id)
{
    portENTER_CRITICAL(&g_request_mux);
    g_latest_request_id = request_id;
    portEXIT_CRITICAL(&g_request_mux);
}

static bool cover_request_is_latest(const CoverRequest &request)
{
    return request.request_id == cover_latest_request_id() &&
        request.catalog_generation == media_catalog_v2_generation();
}

static void cover_publish(
    CoverSurfaceState state,
    const CoverRequest *request,
    esp_err_t result,
    bool cache_hit,
    uint16_t source_width = 0,
    uint16_t source_height = 0,
    uint32_t prepare_ms = 0)
{
    portENTER_CRITICAL(&g_snapshot_mux);
    g_snapshot.ready = g_ready;
    g_snapshot.state = state;
    ++g_snapshot.state_revision;
    g_snapshot.request_id = request != nullptr ? request->request_id : 0U;
    g_snapshot.catalog_generation = request != nullptr ? request->catalog_generation : 0U;
    g_snapshot.track_index = request != nullptr ? request->track_index : UINT32_MAX;
    g_snapshot.width = FAKEPOD_LCD_WIDTH;
    g_snapshot.height = FAKEPOD_LCD_HEIGHT;
    g_snapshot.source_width = source_width;
    g_snapshot.source_height = source_height;
    g_snapshot.prepare_ms = prepare_ms;
    g_snapshot.result = result;
    g_snapshot.cache_hit = cache_hit;
    portEXIT_CRITICAL(&g_snapshot_mux);
}

static void cover_release_entry_locked(CoverCacheEntry *entry)
{
    if (entry == nullptr || !entry->valid || entry->pin_count != 0U) return;
    heap_caps_free(entry->normal);
    heap_caps_free(entry->dimmed);
    *entry = {};
}

static int cover_find_locked(uint32_t generation, uint32_t track_index)
{
    for (size_t i = 0; i < COVER_CACHE_SLOT_COUNT; ++i) {
        const CoverCacheEntry &entry = g_cache[i];
        if (entry.valid && entry.catalog_generation == generation && entry.track_index == track_index) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static int cover_find_lru_evictable_locked(int exclude = -1)
{
    int selected = -1;
    uint32_t oldest = UINT32_MAX;
    for (size_t i = 0; i < COVER_CACHE_SLOT_COUNT; ++i) {
        if (static_cast<int>(i) == exclude) continue;
        const CoverCacheEntry &entry = g_cache[i];
        if (!entry.valid) return static_cast<int>(i);
        if (entry.pin_count == 0U && (selected < 0 || entry.lru_stamp < oldest)) {
            selected = static_cast<int>(i);
            oldest = entry.lru_stamp;
        }
    }
    return selected;
}

static bool cover_cache_has(uint32_t generation, uint32_t track_index)
{
    if (g_cache_mutex == nullptr || xSemaphoreTake(g_cache_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const int index = cover_find_locked(generation, track_index);
    if (index >= 0) {
        g_cache[index].lru_stamp = g_lru_counter++;
        if (g_lru_counter == 0U) g_lru_counter = 1U;
    }
    xSemaphoreGive(g_cache_mutex);
    return index >= 0;
}

static void cover_cache_release_unpinned_except(uint32_t generation, uint32_t track_index)
{
    if (g_cache_mutex == nullptr || xSemaphoreTake(g_cache_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    size_t released = 0U;
    for (size_t i = 0; i < COVER_CACHE_SLOT_COUNT; ++i) {
        CoverCacheEntry &entry = g_cache[i];
        if (!entry.valid || entry.pin_count != 0U) continue;
        if (entry.catalog_generation == generation && entry.track_index == track_index) continue;
        released += COVER_SURFACE_BYTES * 2U;
        cover_release_entry_locked(&entry);
    }
    xSemaphoreGive(g_cache_mutex);
    if (released > 0U) {
        COVER_TRACE("提前释放旧Surface：%uB PSRAM_free=%u",
            static_cast<unsigned>(released),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
}

static bool cover_cache_insert(
    const CoverRequest &request,
    uint8_t *normal,
    uint8_t *dimmed)
{
    if (normal == nullptr || dimmed == nullptr || g_cache_mutex == nullptr ||
        xSemaphoreTake(g_cache_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    for (size_t i = 0; i < COVER_CACHE_SLOT_COUNT; ++i) {
        if (g_cache[i].valid && g_cache[i].catalog_generation != request.catalog_generation &&
            g_cache[i].pin_count == 0U) {
            cover_release_entry_locked(&g_cache[i]);
        }
    }

    int slot = cover_find_locked(request.catalog_generation, request.track_index);
    if (slot >= 0 && g_cache[slot].pin_count != 0U) {
        xSemaphoreGive(g_cache_mutex);
        return false;
    }
    if (slot < 0) slot = cover_find_lru_evictable_locked();
    if (slot < 0) {
        xSemaphoreGive(g_cache_mutex);
        return false;
    }
    if (g_cache[slot].valid) cover_release_entry_locked(&g_cache[slot]);

    CoverCacheEntry &entry = g_cache[slot];
    entry.normal = normal;
    entry.dimmed = dimmed;
    entry.size = COVER_SURFACE_BYTES;
    entry.catalog_generation = request.catalog_generation;
    entry.track_index = request.track_index;
    entry.slot_revision = g_next_slot_revision++;
    if (g_next_slot_revision == 0U) g_next_slot_revision = 1U;
    entry.lru_stamp = g_lru_counter++;
    if (g_lru_counter == 0U) g_lru_counter = 1U;
    entry.pin_count = 0U;
    entry.valid = true;

    xSemaphoreGive(g_cache_mutex);
    return true;
}

static bool cover_psram_budget_ok(size_t source_bytes)
{
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t need = source_bytes + COVER_SURFACE_BYTES * 2U + COVER_PSRAM_SAFETY_RESERVE_BYTES;
    return free_psram > need;
}

static uint8_t *cover_alloc_surface()
{
    return static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16U,
        COVER_SURFACE_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static bool cover_decode_jpeg(const ArtworkCacheLease &lease, Rgb565Source *out)
{
    if (out == nullptr || lease.data == nullptr || lease.size == 0U) return false;

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;

    jpeg_dec_handle_t decoder = nullptr;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || decoder == nullptr) return false;

    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t info = {};
    io.inbuf = const_cast<uint8_t *>(lease.data);
    io.inbuf_len = static_cast<int>(lease.size);

    jpeg_error_t ret = jpeg_dec_parse_header(decoder, &io, &info);
    if (ret != JPEG_ERR_OK || info.width == 0 || info.height == 0) {
        jpeg_dec_close(decoder);
        return false;
    }

    const uint64_t bytes64 = static_cast<uint64_t>(info.width) * static_cast<uint64_t>(info.height) * 2ULL;
    if (bytes64 == 0U || bytes64 > COVER_SOURCE_DECODE_BUDGET_BYTES || bytes64 > SIZE_MAX ||
        !cover_psram_budget_ok(static_cast<size_t>(bytes64))) {
        jpeg_dec_close(decoder);
        return false;
    }

    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16U,
        static_cast<size_t>(bytes64),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        jpeg_dec_close(decoder);
        return false;
    }

    io.outbuf = buffer;
    io.out_size = static_cast<int>(bytes64);
    ret = jpeg_dec_process(decoder, &io);
    jpeg_dec_close(decoder);
    if (ret != JPEG_ERR_OK) {
        heap_caps_free(buffer);
        return false;
    }

    out->data = reinterpret_cast<uint16_t *>(buffer);
    out->width = static_cast<uint32_t>(info.width);
    out->height = static_cast<uint32_t>(info.height);
    return true;
}

static bool cover_decode_png(const ArtworkCacheLease &lease, Rgb888Source *out)
{
    if (out == nullptr || lease.data == nullptr || lease.size == 0U) return false;

    png_image image = {};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, lease.data, lease.size)) return false;
    // P1.2.3: PNG 在 libpng 内部直接去 alpha，并合成到黑底 RGB888。
    // 这样采样阶段不再为 21 万个目标像素做 alpha 乘法。
    image.format = PNG_FORMAT_RGB;

    const size_t bytes = PNG_IMAGE_SIZE(image);
    if (image.width == 0U || image.height == 0U || bytes == 0U ||
        bytes > COVER_SOURCE_DECODE_BUDGET_BYTES || !cover_psram_budget_ok(bytes)) {
        png_image_free(&image);
        return false;
    }

    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16U,
        bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        png_image_free(&image);
        return false;
    }

    png_color background = {};
    background.red = 0U;
    background.green = 0U;
    background.blue = 0U;
    if (!png_image_finish_read(&image, &background, buffer, 0, nullptr)) {
        heap_caps_free(buffer);
        png_image_free(&image);
        return false;
    }

    out->data = buffer;
    out->width = image.width;
    out->height = image.height;
    png_image_free(&image);
    return true;
}

static uint16_t *cover_alloc_axis_map(size_t count)
{
    if (count == 0U || count > SIZE_MAX / sizeof(uint16_t)) return nullptr;
    const size_t bytes = count * sizeof(uint16_t);

    // 映射表只有约 1.8 KiB，优先放内部 RAM，避免每个目标像素都额外访问 PSRAM。
    uint16_t *map = static_cast<uint16_t *>(heap_caps_malloc(
        bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (map == nullptr) {
        map = static_cast<uint16_t *>(heap_caps_malloc(
            bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    return map;
}

static void cover_build_axis_map(
    uint16_t *map,
    uint32_t dest_count,
    uint32_t source_count,
    uint32_t start_fp,
    uint32_t step_fp)
{
    for (uint32_t i = 0; i < dest_count; ++i) {
        const uint64_t coord = static_cast<uint64_t>(start_fp) +
            static_cast<uint64_t>(i) * step_fp + (step_fp >> 1U);
        uint32_t source = static_cast<uint32_t>(coord >> 16U);
        if (source >= source_count) source = source_count - 1U;
        map[i] = static_cast<uint16_t>(source);
    }
}

static inline uint16_t cover_rgb888_to_rgb565(const uint8_t *p)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0] & 0xF8U) << 8U) |
        (static_cast<uint16_t>(p[1] & 0xFCU) << 3U) |
        (static_cast<uint16_t>(p[2]) >> 3U));
}

static inline uint16_t cover_dim_rgb565(uint16_t pixel)
{
    const uint32_t round = 1U << (COVER_DIM_SCALE_SHIFT - 1U);
    const uint32_t r = (static_cast<uint32_t>((pixel >> 11U) & 0x1FU) * COVER_DIM_SCALE_NUM + round) >> COVER_DIM_SCALE_SHIFT;
    const uint32_t g = (static_cast<uint32_t>((pixel >> 5U) & 0x3FU) * COVER_DIM_SCALE_NUM + round) >> COVER_DIM_SCALE_SHIFT;
    const uint32_t b = (static_cast<uint32_t>(pixel & 0x1FU) * COVER_DIM_SCALE_NUM + round) >> COVER_DIM_SCALE_SHIFT;
    return static_cast<uint16_t>((r << 11U) | (g << 5U) | b);
}

static bool cover_render_surface(
    const ArtworkCacheLease &lease,
    uint8_t **out_normal,
    uint8_t **out_dimmed,
    uint16_t *out_source_width,
    uint16_t *out_source_height,
    CoverRenderStats *out_stats)
{
    if (out_normal == nullptr || out_dimmed == nullptr ||
        out_source_width == nullptr || out_source_height == nullptr) {
        return false;
    }
    *out_normal = nullptr;
    *out_dimmed = nullptr;
    *out_source_width = 0U;
    *out_source_height = 0U;
    if (out_stats != nullptr) *out_stats = {};

    const int64_t decode_started_us = esp_timer_get_time();
    Rgb565Source jpeg = {};
    Rgb888Source png = {};
    bool is_jpeg = false;
    bool decoded = false;
    if (lease.format == MediaArtworkFormatV2::Jpeg) {
        decoded = cover_decode_jpeg(lease, &jpeg);
        is_jpeg = decoded;
    } else if (lease.format == MediaArtworkFormatV2::Png) {
        decoded = cover_decode_png(lease, &png);
    }
    if (out_stats != nullptr) {
        out_stats->decode_ms = static_cast<uint32_t>((esp_timer_get_time() - decode_started_us) / 1000LL);
    }
    if (!decoded) return false;

    const uint32_t sw = is_jpeg ? jpeg.width : png.width;
    const uint32_t sh = is_jpeg ? jpeg.height : png.height;
    if (sw == 0U || sh == 0U || sw > UINT16_MAX || sh > UINT16_MAX) {
        heap_caps_free(is_jpeg ? static_cast<void *>(jpeg.data) : static_cast<void *>(png.data));
        return false;
    }

    uint8_t *normal = cover_alloc_surface();
    uint8_t *dimmed = cover_alloc_surface();
    if (normal == nullptr || dimmed == nullptr) {
        heap_caps_free(normal);
        heap_caps_free(dimmed);
        heap_caps_free(is_jpeg ? static_cast<void *>(jpeg.data) : static_cast<void *>(png.data));
        return false;
    }

    // P1.2.2: 最终 Surface 只需要源像素索引，不再保存双线性的 p1/frac。
    // 460x460 圆屏上，快速中心裁切采样的观感足够稳定，同时把逐像素 PSRAM
    // 读取从 4 次降到 1 次，避免封面后台处理占满 CPU1 数秒。
    uint16_t *xmap = cover_alloc_axis_map(FAKEPOD_LCD_WIDTH);
    uint16_t *ymap = cover_alloc_axis_map(FAKEPOD_LCD_HEIGHT);
    if (xmap == nullptr || ymap == nullptr) {
        heap_caps_free(xmap);
        heap_caps_free(ymap);
        heap_caps_free(normal);
        heap_caps_free(dimmed);
        heap_caps_free(is_jpeg ? static_cast<void *>(jpeg.data) : static_cast<void *>(png.data));
        return false;
    }

    uint32_t start_x_fp = 0U;
    uint32_t start_y_fp = 0U;
    uint32_t step_fp = 0U;
    if (static_cast<uint64_t>(sw) * FAKEPOD_LCD_HEIGHT > static_cast<uint64_t>(sh) * FAKEPOD_LCD_WIDTH) {
        step_fp = static_cast<uint32_t>((static_cast<uint64_t>(sh) << 16U) / FAKEPOD_LCD_HEIGHT);
        const uint64_t crop_width_fp = static_cast<uint64_t>(FAKEPOD_LCD_WIDTH) * step_fp;
        start_x_fp = static_cast<uint32_t>(((static_cast<uint64_t>(sw) << 16U) - crop_width_fp) / 2U);
    } else {
        step_fp = static_cast<uint32_t>((static_cast<uint64_t>(sw) << 16U) / FAKEPOD_LCD_WIDTH);
        const uint64_t crop_height_fp = static_cast<uint64_t>(FAKEPOD_LCD_HEIGHT) * step_fp;
        start_y_fp = static_cast<uint32_t>(((static_cast<uint64_t>(sh) << 16U) - crop_height_fp) / 2U);
    }
    if (step_fp == 0U) step_fp = 1U;

    cover_build_axis_map(xmap, FAKEPOD_LCD_WIDTH, sw, start_x_fp, step_fp);
    cover_build_axis_map(ymap, FAKEPOD_LCD_HEIGHT, sh, start_y_fp, step_fp);

    const int64_t resample_started_us = esp_timer_get_time();
    uint16_t *normal16 = reinterpret_cast<uint16_t *>(normal);
    uint16_t *dimmed16 = reinterpret_cast<uint16_t *>(dimmed);

    if (is_jpeg) {
        for (uint32_t y = 0; y < FAKEPOD_LCD_HEIGHT; ++y) {
            const uint16_t *src_row = jpeg.data + static_cast<size_t>(ymap[y]) * sw;
            uint16_t *normal_row = normal16 + static_cast<size_t>(y) * FAKEPOD_LCD_WIDTH;
            uint16_t *dimmed_row = dimmed16 + static_cast<size_t>(y) * FAKEPOD_LCD_WIDTH;
            for (uint32_t x = 0; x < FAKEPOD_LCD_WIDTH; ++x) {
                const uint16_t pixel = src_row[xmap[x]];
                const uint16_t dimmed_pixel = cover_dim_rgb565(pixel);
                normal_row[x] = pixel;
                dimmed_row[x] = dimmed_pixel;
            }
            if (((y + 1U) % COVER_COOPERATIVE_ROW_INTERVAL) == 0U) {
                // taskYIELD() 不会让优先级 0 的 IDLE1 运行；真正阻塞 1 tick，
                // 才能让 Task WDT 的 Idle hook 周期性得到执行机会。
                vTaskDelay(1);
            }
        }
    } else {
        for (uint32_t y = 0; y < FAKEPOD_LCD_HEIGHT; ++y) {
            const uint8_t *src_row = png.data + static_cast<size_t>(ymap[y]) * sw * 3U;
            uint16_t *normal_row = normal16 + static_cast<size_t>(y) * FAKEPOD_LCD_WIDTH;
            uint16_t *dimmed_row = dimmed16 + static_cast<size_t>(y) * FAKEPOD_LCD_WIDTH;
            for (uint32_t x = 0; x < FAKEPOD_LCD_WIDTH; ++x) {
                const uint16_t pixel = cover_rgb888_to_rgb565(src_row + static_cast<size_t>(xmap[x]) * 3U);
                const uint16_t dimmed_pixel = cover_dim_rgb565(pixel);
                normal_row[x] = pixel;
                dimmed_row[x] = dimmed_pixel;
            }
            if (((y + 1U) % COVER_COOPERATIVE_ROW_INTERVAL) == 0U) {
                vTaskDelay(1);
            }
        }
    }

    if (out_stats != nullptr) {
        out_stats->resample_ms = static_cast<uint32_t>((esp_timer_get_time() - resample_started_us) / 1000LL);
    }

    heap_caps_free(xmap);
    heap_caps_free(ymap);
    heap_caps_free(is_jpeg ? static_cast<void *>(jpeg.data) : static_cast<void *>(png.data));
    *out_normal = normal;
    *out_dimmed = dimmed;
    *out_source_width = static_cast<uint16_t>(sw);
    *out_source_height = static_cast<uint16_t>(sh);
    return true;
}

static void cover_task_main(void *)
{
    g_ready = true;
    cover_publish(CoverSurfaceState::Idle, nullptr, ESP_OK, false);
    COVER_TRACE("当前曲Surface服务：%dx%d normal+dimmed=%uB×2 steady=%uB priority=%u core=%ld",
        FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT,
        static_cast<unsigned>(COVER_SURFACE_BYTES),
        static_cast<unsigned>(COVER_SURFACE_BYTES * 2U),
        static_cast<unsigned>(COVER_TASK_PRIORITY), static_cast<long>(COVER_TASK_CORE));

    CoverRequest request = {};
    while (true) {
        // R.36：只有当前曲 Surface 请求会唤醒任务，不再处理 wire 偏好或下一曲预热。
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (xQueueReceive(g_queue, &request, 0) != pdTRUE) continue;

        // 队列长度为 1；如果等待期间又来了更新请求，直接取最新一份。
        CoverRequest newer = {};
        while (xQueueReceive(g_queue, &newer, 0) == pdTRUE) request = newer;
        if (!cover_request_is_latest(request)) continue;

        if (cover_cache_has(request.catalog_generation, request.track_index)) {
            cover_publish(CoverSurfaceState::Ready, &request, ESP_OK, true);
            continue;
        }

        // 若旧 Surface 已经没有 UI lease，先释放再解码，避免无意义的双份峰值。
        cover_cache_release_unpinned_except(request.catalog_generation, request.track_index);
        cover_publish(CoverSurfaceState::Preparing, &request, ESP_OK, false);
        const int64_t started_us = esp_timer_get_time();

        ArtworkCacheLease compressed = {};
        if (!artwork_loader_acquire_cached(request.track_index, &compressed)) {
            cover_publish(CoverSurfaceState::Failed, &request, ESP_ERR_NOT_FOUND, false);
            continue;
        }

        uint8_t *normal = nullptr;
        uint8_t *dimmed = nullptr;
        uint16_t source_width = 0U;
        uint16_t source_height = 0U;
        CoverRenderStats render_stats = {};
#if APP_DIAG_ARTWORK_UI
        const MediaArtworkFormatV2 artwork_format = compressed.format;
#endif
        const bool rendered = cover_render_surface(
            compressed, &normal, &dimmed,
            &source_width, &source_height, &render_stats);
        artwork_loader_release_cached(&compressed);

        const uint32_t prepare_ms = static_cast<uint32_t>((esp_timer_get_time() - started_us) / 1000LL);
        if (!rendered) {
            heap_caps_free(normal);
            heap_caps_free(dimmed);
            cover_publish(CoverSurfaceState::Failed, &request, ESP_FAIL, false, source_width, source_height, prepare_ms);
            ESP_LOGW(TAG, "封面预处理失败：track=%lu，保留 LVGL 压缩图回退路径",
                static_cast<unsigned long>(request.track_index));
            continue;
        }

        if (!cover_request_is_latest(request)) {
            heap_caps_free(normal);
            heap_caps_free(dimmed);
            continue;
        }

        if (!cover_cache_insert(request, normal, dimmed)) {
            heap_caps_free(normal);
            heap_caps_free(dimmed);
            cover_publish(CoverSurfaceState::Failed, &request, ESP_ERR_NO_MEM, false, source_width, source_height, prepare_ms);
            continue;
        }

        // Surface 已经独立拥有最终像素，压缩 JPEG/PNG 不再需要长期留在 PSRAM。
        artwork_loader_discard_unpinned();
        cover_publish(CoverSurfaceState::Ready, &request, ESP_OK, false, source_width, source_height, prepare_ms);
#if APP_DIAG_ARTWORK_UI
        const UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(nullptr);
        const BaseType_t finish_core = xPortGetCoreID();
        const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        const char *format_name = artwork_format == MediaArtworkFormatV2::Png ? "PNG" : "JPEG";
        COVER_TRACE("当前曲封面完成：track=%lu format=%s source=%ux%u -> %dx%d total=%lums decode=%lums sample=%lums core=%ld stack_hwm=%u PSRAM_free=%u largest=%u",
            static_cast<unsigned long>(request.track_index), format_name,
            static_cast<unsigned>(source_width), static_cast<unsigned>(source_height),
            FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT,
            static_cast<unsigned long>(prepare_ms),
            static_cast<unsigned long>(render_stats.decode_ms),
            static_cast<unsigned long>(render_stats.resample_ms),
            static_cast<long>(finish_core),
            static_cast<unsigned>(stack_hwm),
            static_cast<unsigned>(psram_free),
            static_cast<unsigned>(psram_largest));
#endif
    }
}

esp_err_t cover_surface_cache_start()
{
    if (g_ready) return ESP_OK;
    if (g_queue == nullptr) g_queue = xQueueCreate(1, sizeof(CoverRequest));
    if (g_submit_mutex == nullptr) g_submit_mutex = xSemaphoreCreateMutex();
    if (g_cache_mutex == nullptr) g_cache_mutex = xSemaphoreCreateMutex();
    if (g_queue == nullptr || g_submit_mutex == nullptr || g_cache_mutex == nullptr) return ESP_ERR_NO_MEM;

    const BaseType_t ret = xTaskCreatePinnedToCore(
        cover_task_main,
        "CoverSurface",
        COVER_TASK_STACK_BYTES,
        nullptr,
        COVER_TASK_PRIORITY,
        &g_task,
        COVER_TASK_CORE);
    if (ret != pdPASS) {
        g_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool cover_surface_cache_is_ready()
{
    return g_ready;
}

bool cover_surface_cache_request_track(uint32_t track_index, uint32_t *out_request_id)
{
    if (!g_ready || g_queue == nullptr || g_submit_mutex == nullptr || track_index == UINT32_MAX) return false;
    if (xSemaphoreTake(g_submit_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;

    CoverRequest request = {};
    request.request_id = cover_next_request_id();
    request.catalog_generation = media_catalog_v2_generation();
    request.track_index = track_index;
    cover_set_latest_request_id(request.request_id);

    // 队列长度为 1：新请求始终覆盖尚未处理的旧请求，保持 latest-wins，
    // 不在提交线程中 reset 队列，避免与 CoverSurfaceTask 竞争队列状态。
    const BaseType_t sent = xQueueOverwrite(g_queue, &request);
    if (sent == pdTRUE && g_task != nullptr) xTaskNotifyGive(g_task);
    xSemaphoreGive(g_submit_mutex);
    if (sent != pdTRUE) return false;
    if (out_request_id != nullptr) *out_request_id = request.request_id;
    return true;
}

bool cover_surface_cache_get_snapshot(CoverSurfaceSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    portENTER_CRITICAL(&g_snapshot_mux);
    *out_snapshot = g_snapshot;
    portEXIT_CRITICAL(&g_snapshot_mux);
    return true;
}

bool cover_surface_cache_acquire(uint32_t track_index, CoverSurfaceLease *out_lease)
{
    if (out_lease == nullptr || g_cache_mutex == nullptr) return false;
    *out_lease = {};
    if (xSemaphoreTake(g_cache_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;

    const uint32_t generation = media_catalog_v2_generation();
    const int index = cover_find_locked(generation, track_index);
    if (index < 0) {
        xSemaphoreGive(g_cache_mutex);
        return false;
    }

    CoverCacheEntry &entry = g_cache[index];
    ++entry.pin_count;
    entry.lru_stamp = g_lru_counter++;
    if (g_lru_counter == 0U) g_lru_counter = 1U;

    out_lease->normal_rgb565 = entry.normal;
    out_lease->dimmed_rgb565 = entry.dimmed;
    out_lease->data_size = entry.size;
    out_lease->width = FAKEPOD_LCD_WIDTH;
    out_lease->height = FAKEPOD_LCD_HEIGHT;
    out_lease->catalog_generation = entry.catalog_generation;
    out_lease->track_index = entry.track_index;
    out_lease->slot_revision = entry.slot_revision;
    out_lease->slot_index = static_cast<uint8_t>(index);
    xSemaphoreGive(g_cache_mutex);
    return true;
}

void cover_surface_cache_release(CoverSurfaceLease *lease)
{
    if (lease == nullptr || lease->slot_index >= COVER_CACHE_SLOT_COUNT || g_cache_mutex == nullptr) {
        if (lease != nullptr) *lease = {};
        return;
    }
    if (xSemaphoreTake(g_cache_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        CoverCacheEntry &entry = g_cache[lease->slot_index];
        if (entry.valid && entry.slot_revision == lease->slot_revision && entry.pin_count > 0U) {
            --entry.pin_count;
        }
        xSemaphoreGive(g_cache_mutex);
    }
    *lease = {};
}

void cover_surface_cache_retain_track(uint32_t track_index)
{
    if (track_index == UINT32_MAX) return;
    cover_cache_release_unpinned_except(media_catalog_v2_generation(), track_index);
}
