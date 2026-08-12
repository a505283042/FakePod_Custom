#include "now_playing_artwork.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app_diag_config.h"
#include "artwork_loader.h"
#include "board_pins.h"
#include "cover_surface_cache.h"
#include "display.h"
#include "display_bounded_spi.h"
#include "font/font_manager.h"
#include "media_catalog_v2.h"
#include "media_library.h"
#include "player_state.h"
#include "ui_common.h"

static const char *TAG = "封面界面";

#if APP_DIAG_ARTWORK_UI
#define ARTWORK_UI_TRACE(...) ESP_LOGI(TAG, "ARTWORK_UI_TRACE: " __VA_ARGS__)
#else
#define ARTWORK_UI_TRACE(...) APP_DIAG_DISCARDED_LOGI(TAG, "ARTWORK_UI_TRACE: " __VA_ARGS__)
#endif

// 压缩图直接交给 LVGL 的路径只保留为兼容回退（例如不受 esp_new_jpeg 支持的 JPEG）。
// R.20 正常路径由 CoverSurfaceTask 同时预处理 normal + dimmed 两张 460x460 RGB565。
// R.22 起跨 Track 替换时保留旧封面直到新 Surface 真正可用，“准备封面/读取封面”不再可见。
// R.36 取消第三张 wire-order Surface；R.36.4 起封面整屏提交改走 BoundedSPI，
// 直接消费 native normal/dimmed，并在双 DMA staging 拷贝时在线 byte-swap。
// 主页 Surface 高速提交统一走 BoundedSPI；LVGL 仅保留兼容回退。
static constexpr size_t kArtworkDecodedBudgetBytes = 3U * 1024U * 1024U;
static constexpr size_t kArtworkPsramSafetyReserveBytes = 768U * 1024U;
static constexpr uint32_t kLvImageScaleNone = 256U;

static lv_obj_t *g_container = nullptr;
static lv_obj_t *g_image = nullptr;
static lv_obj_t *g_placeholder_icon = nullptr;
static lv_obj_t *g_status = nullptr;
static int32_t g_image_max_size = 0;

// R.20 快速路径：一个 lease 同时持有 normal + dimmed 两张最终 RGB565 Surface。
static CoverSurfaceLease g_surface_lease = {};
static lv_image_dsc_t g_surface_normal_dsc = {};
static lv_image_dsc_t g_surface_dimmed_dsc = {};
static bool g_has_surface_source = false;
static bool g_dimmed_requested = false;
static bool g_dimmed_applied = false;
static uint32_t g_last_surface_state_revision = UINT32_MAX;
static bool g_bounded_present_allowed = true;
static bool g_bounded_present_event_pending = false;

// 兼容回退：Stage 12.2 压缩图直接交给 LVGL decoder。
static ArtworkCacheLease g_compressed_lease = {};
static lv_image_dsc_t g_compressed_dsc = {};
static bool g_has_compressed_source = false;

static uint32_t g_context_generation = 0U;
static uint32_t g_context_track = UINT32_MAX;
static uint32_t g_last_loader_state_revision = UINT32_MAX;
static bool g_active = true;

static lv_obj_t *artwork_ui_create_label(
    lv_obj_t *parent,
    const char *text,
    lv_color_t color,
    const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    ui_common_lock_object(label);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font != nullptr ? font : font_manager_get_ui_font(), 0);
    return label;
}

static uint32_t artwork_ui_displayed_track()
{
    if (g_has_surface_source) return g_surface_lease.track_index;
    if (g_has_compressed_source) return g_compressed_lease.track_index;
    return UINT32_MAX;
}

static bool artwork_ui_source_matches_context()
{
    if (g_has_surface_source) {
        return g_surface_lease.catalog_generation == g_context_generation &&
            g_surface_lease.track_index == g_context_track;
    }
    if (g_has_compressed_source) {
        return g_compressed_lease.catalog_generation == g_context_generation &&
            g_compressed_lease.track_index == g_context_track;
    }
    return false;
}

// R.22：等待新封面时不显示“准备封面/读取封面”。
// 已经有上一首封面就继续保持；首次启动还没有任何 Source 时保持纯黑底。
static void artwork_ui_show_waiting_without_placeholder()
{
    if (g_placeholder_icon != nullptr) lv_obj_add_flag(g_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
    if (g_status != nullptr) lv_obj_add_flag(g_status, LV_OBJ_FLAG_HIDDEN);
    if (!g_has_surface_source && !g_has_compressed_source && g_image != nullptr) {
        lv_obj_add_flag(g_image, LV_OBJ_FLAG_HIDDEN);
    }
}

static void artwork_ui_show_placeholder(const char *status)
{
    if (g_image != nullptr) lv_obj_add_flag(g_image, LV_OBJ_FLAG_HIDDEN);
    if (g_placeholder_icon != nullptr) lv_obj_remove_flag(g_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
    if (g_status != nullptr) {
        if (status != nullptr && status[0] != '\0') {
            lv_label_set_text(g_status, status);
            lv_obj_remove_flag(g_status, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_status, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void artwork_ui_release_compressed_source(bool hide_image = true)
{
    if (g_has_compressed_source) {
        if (hide_image && g_image != nullptr) lv_obj_add_flag(g_image, LV_OBJ_FLAG_HIDDEN);
        lv_image_cache_drop(&g_compressed_dsc);
        lv_image_header_cache_drop(&g_compressed_dsc);
        g_has_compressed_source = false;
    }
    artwork_loader_release_cached(&g_compressed_lease);
    g_compressed_dsc = {};
}

static void artwork_ui_release_surface_source(bool hide_image = true)
{
    if (g_has_surface_source && hide_image && g_image != nullptr) {
        lv_obj_add_flag(g_image, LV_OBJ_FLAG_HIDDEN);
    }
    cover_surface_cache_release(&g_surface_lease);
    g_surface_normal_dsc = {};
    g_surface_dimmed_dsc = {};
    g_has_surface_source = false;
    g_dimmed_applied = false;
}

static void artwork_ui_release_all_sources(bool hide_image = true)
{
    artwork_ui_release_compressed_source(hide_image);
    artwork_ui_release_surface_source(hide_image);
}

static void artwork_ui_init_rgb565_dsc(lv_image_dsc_t *dsc, const uint8_t *data, uint16_t width, uint16_t height, size_t size)
{
    if (dsc == nullptr) return;
    *dsc = {};
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.flags = 0U;
    dsc->header.w = width;
    dsc->header.h = height;
    dsc->header.stride = static_cast<uint32_t>(width) * 2U;
    dsc->data_size = static_cast<uint32_t>(size);
    dsc->data = data;
}

static bool artwork_ui_apply_surface(uint32_t track_index)
{
    CoverSurfaceLease lease = {};
    if (!cover_surface_cache_acquire(track_index, &lease)) return false;
    if (lease.normal_rgb565 == nullptr || lease.dimmed_rgb565 == nullptr ||
        lease.width == 0U || lease.height == 0U || lease.data_size == 0U) {
        cover_surface_cache_release(&lease);
        return false;
    }

    const uint32_t previous_track = artwork_ui_displayed_track();
    const bool replacing_track = previous_track != UINT32_MAX && previous_track != track_index;
    const uint8_t *present_surface = g_dimmed_requested
        ? lease.dimmed_rgb565
        : lease.normal_rgb565;
    const uint8_t *bounded_surface = present_surface;

    bool bounded_presented = false;
    DisplayBoundedSpiStats bounded_stats = {};
    if (replacing_track && g_bounded_present_allowed &&
        lease.width == FAKEPOD_LCD_WIDTH && lease.height == FAKEPOD_LCD_HEIGHT) {
        const esp_err_t bounded_ret = display_cover_bounded_spi_present(
            bounded_surface,
            lease.width,
            lease.height,
            false,
            &bounded_stats);
        bounded_presented = bounded_ret == ESP_OK;
        if (!bounded_presented && bounded_ret == ESP_ERR_NO_MEM) {
            // R.27：双 staging 临时拿不到时不要黑屏、不要切换 LVGL source。
            // 释放刚 acquire 的新 lease，继续保持旧封面；下一次 Artwork update 会自动重试。
            ESP_LOGW(TAG,
                "R.36.4 封面BoundedSPI暂缓：%lu -> %lu 双staging内存不足，保持旧封面并重试",
                static_cast<unsigned long>(previous_track),
                static_cast<unsigned long>(track_index));
            cover_surface_cache_release(&lease);
            return false;
        }
        if (!bounded_presented) {
            ESP_LOGW(TAG,
                "R.36.4 封面BoundedSPI失败：%lu -> %lu ret=%s，退回R.22 LVGL PresentHold",
                static_cast<unsigned long>(previous_track),
                static_cast<unsigned long>(track_index),
                esp_err_to_name(bounded_ret));
        }
    }

    if (replacing_track && !bounded_presented) {
        // 非内存类错误才保留 R.22 兼容回退；NO_MEM 已在上方保持旧封面等待重试。
        display_present_request_hold();
    }

    // 新 lease 已经到手后才释放旧 lease；等待阶段旧图一直可见。
    artwork_ui_release_all_sources(false);
    g_surface_lease = lease;
    // R.36：新 Surface 已 pin、旧 lease 已释放。此时清理交换槽中的旧曲，
    // 并释放所有未被 fallback 固定的压缩原图，稳态只留下当前 normal+dimmed。
    cover_surface_cache_retain_track(track_index);
    artwork_loader_discard_unpinned();
    artwork_ui_init_rgb565_dsc(
        &g_surface_normal_dsc,
        g_surface_lease.normal_rgb565,
        g_surface_lease.width,
        g_surface_lease.height,
        g_surface_lease.data_size);
    artwork_ui_init_rgb565_dsc(
        &g_surface_dimmed_dsc,
        g_surface_lease.dimmed_rgb565,
        g_surface_lease.width,
        g_surface_lease.height,
        g_surface_lease.data_size);
    g_dimmed_applied = g_dimmed_requested;

    // BoundedSPI 已经把完整新封面写进 CO5300 GRAM。此时必须同步更新 LVGL 的 image source，
    // 但不能再次把 460x460 image 标成 invalid，否则会重新走 70ms 左右的整屏 render/flush。
    lv_display_t *display = lv_display_get_default();
    const bool invalidation_was_enabled =
        display != nullptr && lv_display_is_invalidation_enabled(display);
    if (bounded_presented && invalidation_was_enabled) {
        lv_display_enable_invalidation(display, false);
    }

    lv_image_set_src(
        g_image,
        g_dimmed_applied ? &g_surface_dimmed_dsc : &g_surface_normal_dsc);
    lv_image_set_scale(g_image, kLvImageScaleNone);
    lv_image_set_antialias(g_image, false);
    lv_obj_center(g_image);
    lv_obj_remove_flag(g_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_status, LV_OBJ_FLAG_HIDDEN);

    if (bounded_presented && invalidation_was_enabled) {
        lv_display_enable_invalidation(display, true);
    }

    g_has_surface_source = true;
    if (bounded_presented) {
        g_bounded_present_event_pending = true;
#if APP_DIAG_DISPLAY_TRANSPORT
        ESP_LOGI(TAG,
            "封面BoundedSPI：%lu -> %lu gen=%u total=%uus te=%uus stream=%uus swap=%uus wait=%uus chunks=%u staging=%u行×%u dim=%u",
            static_cast<unsigned long>(previous_track),
            static_cast<unsigned long>(track_index),
            static_cast<unsigned>(bounded_stats.generation),
            static_cast<unsigned>(bounded_stats.total_us),
            static_cast<unsigned>(bounded_stats.te_wait_us),
            static_cast<unsigned>(bounded_stats.stream_us),
            static_cast<unsigned>(bounded_stats.byte_swap_us),
            static_cast<unsigned>(bounded_stats.queue_wait_us),
            static_cast<unsigned>(bounded_stats.chunks),
            static_cast<unsigned>(bounded_stats.staging_rows),
            static_cast<unsigned>(bounded_stats.staging_buffers),
            static_cast<unsigned>(g_dimmed_applied));
#endif
    } else if (replacing_track) {
#if APP_DIAG_DISPLAY_TRANSPORT
        ESP_LOGI(TAG,
            "封面BoundedSPI回退请求：%lu -> %lu，旧图保持到新Surface就绪",
            static_cast<unsigned long>(previous_track),
            static_cast<unsigned long>(track_index));
#endif
    }

    ARTWORK_UI_TRACE("SURFACE_READY generation=%lu track=%lu %ux%u dim=%u bounded=%u",
        static_cast<unsigned long>(g_surface_lease.catalog_generation),
        static_cast<unsigned long>(g_surface_lease.track_index),
        static_cast<unsigned>(g_surface_lease.width),
        static_cast<unsigned>(g_surface_lease.height),
        static_cast<unsigned>(g_dimmed_applied),
        static_cast<unsigned>(bounded_presented));
    return true;
}

static bool artwork_ui_read_png_dimensions(
    const uint8_t *data,
    size_t size,
    uint32_t *out_width,
    uint32_t *out_height)
{
    if (data == nullptr || size < 24U || out_width == nullptr || out_height == nullptr ||
        memcmp(data, "\x89PNG\x0D\x0A\x1A\x0A", 8U) != 0) return false;
    const uint32_t width =
        (static_cast<uint32_t>(data[16]) << 24U) |
        (static_cast<uint32_t>(data[17]) << 16U) |
        (static_cast<uint32_t>(data[18]) << 8U) |
        static_cast<uint32_t>(data[19]);
    const uint32_t height =
        (static_cast<uint32_t>(data[20]) << 24U) |
        (static_cast<uint32_t>(data[21]) << 16U) |
        (static_cast<uint32_t>(data[22]) << 8U) |
        static_cast<uint32_t>(data[23]);
    if (width == 0U || height == 0U) return false;
    *out_width = width;
    *out_height = height;
    return true;
}

static bool artwork_ui_is_jpeg_sof_marker(uint8_t marker)
{
    switch (marker) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB:
        case 0xCD: case 0xCE: case 0xCF:
            return true;
        default:
            return false;
    }
}

static bool artwork_ui_read_jpeg_dimensions(
    const uint8_t *data,
    size_t size,
    uint32_t *out_width,
    uint32_t *out_height)
{
    if (data == nullptr || size < 4U || out_width == nullptr || out_height == nullptr ||
        data[0] != 0xFFU || data[1] != 0xD8U) return false;

    size_t pos = 2U;
    while (pos + 3U < size) {
        while (pos < size && data[pos] != 0xFFU) ++pos;
        while (pos < size && data[pos] == 0xFFU) ++pos;
        if (pos >= size) break;
        const uint8_t marker = data[pos++];
        if (marker == 0xD8U || marker == 0xD9U || marker == 0x01U ||
            (marker >= 0xD0U && marker <= 0xD7U)) continue;
        if (pos + 2U > size) break;
        const uint16_t segment_length =
            (static_cast<uint16_t>(data[pos]) << 8U) |
            static_cast<uint16_t>(data[pos + 1U]);
        if (segment_length < 2U || pos + static_cast<size_t>(segment_length) > size) break;
        if (artwork_ui_is_jpeg_sof_marker(marker) && segment_length >= 7U) {
            const uint32_t height =
                (static_cast<uint32_t>(data[pos + 3U]) << 8U) |
                static_cast<uint32_t>(data[pos + 4U]);
            const uint32_t width =
                (static_cast<uint32_t>(data[pos + 5U]) << 8U) |
                static_cast<uint32_t>(data[pos + 6U]);
            if (width > 0U && height > 0U) {
                *out_width = width;
                *out_height = height;
                return true;
            }
        }
        if (marker == 0xDAU) break;
        pos += static_cast<size_t>(segment_length);
    }
    return false;
}

static bool artwork_ui_resolve_dimensions(
    const ArtworkCacheLease &lease,
    uint32_t *out_width,
    uint32_t *out_height)
{
    if (out_width == nullptr || out_height == nullptr) return false;
    if (lease.width > 0U && lease.height > 0U) {
        *out_width = lease.width;
        *out_height = lease.height;
        return true;
    }
    if (lease.format == MediaArtworkFormatV2::Png) {
        return artwork_ui_read_png_dimensions(lease.data, lease.size, out_width, out_height);
    }
    if (lease.format == MediaArtworkFormatV2::Jpeg) {
        return artwork_ui_read_jpeg_dimensions(lease.data, lease.size, out_width, out_height);
    }
    return false;
}

static bool artwork_ui_decode_budget_ok(
    MediaArtworkFormatV2 format,
    uint32_t width,
    uint32_t height,
    size_t *out_estimated_bytes)
{
    const size_t bytes_per_pixel = format == MediaArtworkFormatV2::Png ? 4U : 2U;
    if (width == 0U || height == 0U ||
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > SIZE_MAX / bytes_per_pixel) return false;
    const size_t estimated = static_cast<size_t>(width) * static_cast<size_t>(height) * bytes_per_pixel;
    if (out_estimated_bytes != nullptr) *out_estimated_bytes = estimated;
    if (estimated > kArtworkDecodedBudgetBytes) return false;
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return free_psram > estimated + kArtworkPsramSafetyReserveBytes;
}

static bool artwork_ui_apply_compressed_fallback(uint32_t track_index)
{
    ArtworkCacheLease lease = {};
    if (!artwork_loader_acquire_cached(track_index, &lease)) return false;

    uint32_t width = 0U;
    uint32_t height = 0U;
    if (!artwork_ui_resolve_dimensions(lease, &width, &height)) {
        artwork_loader_release_cached(&lease);
        return false;
    }

    size_t estimated_bytes = 0U;
    if (!artwork_ui_decode_budget_ok(lease.format, width, height, &estimated_bytes)) {
        ESP_LOGW(TAG, "回退封面展开预算不足：track=%lu %lux%lu",
            static_cast<unsigned long>(track_index),
            static_cast<unsigned long>(width), static_cast<unsigned long>(height));
        artwork_loader_release_cached(&lease);
        return false;
    }

    lv_image_dsc_t new_dsc = {};
    new_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    new_dsc.header.cf = lease.format == MediaArtworkFormatV2::Png
        ? LV_COLOR_FORMAT_RAW_ALPHA
        : LV_COLOR_FORMAT_RAW;
    new_dsc.header.w = static_cast<uint16_t>(width > UINT16_MAX ? UINT16_MAX : width);
    new_dsc.header.h = static_cast<uint16_t>(height > UINT16_MAX ? UINT16_MAX : height);
    new_dsc.data_size = static_cast<uint32_t>(lease.size);
    new_dsc.data = lease.data;

    lv_image_header_t decoded_header = {};
    if (lv_image_decoder_get_info(&new_dsc, &decoded_header) != LV_RESULT_OK ||
        decoded_header.w == 0U || decoded_header.h == 0U) {
        artwork_loader_release_cached(&lease);
        return false;
    }
    new_dsc.header.w = decoded_header.w;
    new_dsc.header.h = decoded_header.h;

    const uint32_t previous_track = artwork_ui_displayed_track();
    const bool replacing_track = previous_track != UINT32_MAX && previous_track != track_index;
    if (replacing_track) display_present_request_hold();

    artwork_ui_release_all_sources(false);
    g_compressed_lease = lease;
    g_compressed_dsc = new_dsc;

    lv_image_set_src(g_image, &g_compressed_dsc);
    const uint32_t min_dim = decoded_header.w < decoded_header.h ? decoded_header.w : decoded_header.h;
    uint32_t scale = kLvImageScaleNone;
    if (min_dim > 0U) {
        scale = static_cast<uint32_t>(
            (static_cast<uint64_t>(g_image_max_size) * kLvImageScaleNone + min_dim - 1U) / min_dim);
        if (scale == 0U) scale = 1U;
    }
    lv_image_set_scale(g_image, scale);
    lv_image_set_antialias(g_image, true);
    lv_obj_center(g_image);
    lv_obj_remove_flag(g_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_status, LV_OBJ_FLAG_HIDDEN);
    g_has_compressed_source = true;

    ESP_LOGW(TAG, "使用 LVGL 压缩图回退路径：track=%lu；Overlay 将临时使用 alpha 遮罩",
        static_cast<unsigned long>(track_index));
    return true;
}

static void artwork_ui_sync_context(bool force)
{
    if (g_container == nullptr || !player_state_is_ready() || media_library_get_count() == 0U) return;
    const uint32_t generation = media_catalog_v2_generation();
    const uint32_t track_index = static_cast<uint32_t>(player_state_get_index());
    if (!force && generation == g_context_generation && track_index == g_context_track) return;

    g_context_generation = generation;
    g_context_track = track_index;
    g_last_loader_state_revision = UINT32_MAX;
    g_last_surface_state_revision = UINT32_MAX;

    // R.22：先尝试新曲最终 Surface。命中时 artwork_ui_apply_surface() 会“先 acquire 新、后 release 旧”。
    // 未命中时绝不先释放旧封面，也不显示“准备封面”。
    if (cover_surface_cache_is_ready() && artwork_ui_apply_surface(track_index)) return;

    MediaArtworkViewV2 artwork = {};
    if (!media_library_get_artwork_view(track_index, &artwork)) {
        artwork_ui_release_all_sources();
        artwork_ui_show_placeholder("暂无封面");
    } else if (!artwork_loader_is_ready()) {
        artwork_ui_release_all_sources();
        artwork_ui_show_placeholder("封面服务不可用");
    } else {
        artwork_ui_show_waiting_without_placeholder();
    }
}

esp_err_t now_playing_artwork_create(lv_obj_t *parent, int32_t size_px, lv_obj_t **out_container)
{
    if (parent == nullptr || size_px < 80 || out_container == nullptr) return ESP_ERR_INVALID_ARG;
    *out_container = nullptr;
    if (g_container != nullptr) {
        *out_container = g_container;
        return ESP_OK;
    }

    g_image_max_size = size_px;
    g_container = lv_obj_create(parent);
    if (g_container == nullptr) return ESP_ERR_NO_MEM;
    ui_common_lock_object(g_container);
    lv_obj_set_size(g_container, size_px, size_px);
    lv_obj_set_style_radius(g_container, 0, 0);
    lv_obj_set_style_bg_color(g_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_container, 0, 0);
    lv_obj_set_style_shadow_width(g_container, 0, 0);
    lv_obj_set_style_pad_all(g_container, 0, 0);
    lv_obj_remove_flag(g_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_container, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    g_image = lv_image_create(g_container);
    if (g_image == nullptr) {
        lv_obj_delete(g_container);
        g_container = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ui_common_lock_object(g_image);
    lv_obj_remove_flag(g_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_image, LV_OBJ_FLAG_HIDDEN);

    g_placeholder_icon = artwork_ui_create_label(
        g_container, LV_SYMBOL_AUDIO, lv_color_hex(0x6F7A89), lv_font_default());
    lv_obj_align(g_placeholder_icon, LV_ALIGN_CENTER, 0, -10);

    g_status = artwork_ui_create_label(
        g_container, "", lv_color_hex(0x6F7A89), font_manager_get_ui_font());
    lv_obj_set_style_text_align(g_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_status, LV_ALIGN_CENTER, 0, 28);
    lv_obj_add_flag(g_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_status, LV_OBJ_FLAG_HIDDEN);

    artwork_ui_sync_context(true);
    *out_container = g_container;
    return ESP_OK;
}

void now_playing_artwork_refresh_context()
{
    if (!g_active) return;
    artwork_ui_sync_context(false);
}

void now_playing_artwork_set_active(bool active, bool suppress_invalidation)
{
    if (g_active == active) return;

    // Launcher BoundedSPI 接管/归还 GRAM 时，lease 生命周期仍必须完整执行，但不能让
    // lv_obj_add_flag()/lv_image_set_src() 产生一笔稍后才刷出的 460x460 invalidation。
    // 否则 raw full-present 之后，迟到的 LVGL flush 会用主页黑底覆盖外圈，只剩中心 strip。
    lv_display_t *display = lv_display_get_default();
    const bool invalidation_was_enabled =
        suppress_invalidation && display != nullptr && lv_display_is_invalidation_enabled(display);
    if (invalidation_was_enabled) {
        lv_display_enable_invalidation(display, false);
    }

    g_active = active;

    if (!g_active) {
        g_bounded_present_event_pending = false;
        // 页面被完整覆盖后立即释放 UI lease。quiet 模式只抑制物理重绘请求，
        // hidden/source 状态仍同步更新，因此不会留下悬空 Surface 指针。
        artwork_ui_release_all_sources();
        if (g_placeholder_icon != nullptr) lv_obj_add_flag(g_placeholder_icon, LV_OBJ_FLAG_HIDDEN);
        if (g_status != nullptr) lv_obj_add_flag(g_status, LV_OBJ_FLAG_HIDDEN);
        ARTWORK_UI_TRACE("SUSPEND release leases context_track=%lu quiet=%u",
            static_cast<unsigned long>(g_context_track),
            static_cast<unsigned>(suppress_invalidation));
    } else {
        // 恢复时强制重新读取 Player context。若外部 BoundedSPI 已把当前 Surface 恢复到 GRAM，
        // quiet resume 只同步 LVGL source/lease，不再重复整屏 render/flush。
        artwork_ui_sync_context(true);
        ARTWORK_UI_TRACE("RESUME rebind context_track=%lu quiet=%u",
            static_cast<unsigned long>(g_context_track),
            static_cast<unsigned>(suppress_invalidation));
    }

    if (invalidation_was_enabled) {
        lv_display_enable_invalidation(display, true);
    }
}

void now_playing_artwork_update()
{
    if (g_container == nullptr || !g_active) return;

    artwork_ui_sync_context(false);
    if (g_context_track == UINT32_MAX) return;

    // 先直接查当前曲最终 RGB565 Surface。显示正确性以 cache 命中为准，
    // 不依赖 UI 必须消费某一次 Ready Snapshot。
    if (cover_surface_cache_is_ready() && !artwork_ui_source_matches_context()) {
        (void)artwork_ui_apply_surface(g_context_track);
    }

    // Snapshot 仍用于 Preparing/Failed 状态提示与兼容回退。
    if (cover_surface_cache_is_ready()) {
        CoverSurfaceSnapshot surface = {};
        if (cover_surface_cache_get_snapshot(&surface) &&
            surface.state_revision != g_last_surface_state_revision) {
            g_last_surface_state_revision = surface.state_revision;
            if (surface.catalog_generation == g_context_generation && surface.track_index == g_context_track) {
                if (surface.state == CoverSurfaceState::Ready) {
                    if (!artwork_ui_source_matches_context() && !artwork_ui_apply_surface(g_context_track)) {
                        artwork_ui_show_waiting_without_placeholder();
                    }
                } else if (surface.state == CoverSurfaceState::Preparing) {
                    if (!artwork_ui_source_matches_context()) artwork_ui_show_waiting_without_placeholder();
                } else if (surface.state == CoverSurfaceState::Failed) {
                    // progressive JPEG 等无法走 esp_new_jpeg 时保留旧 decoder 兼容能力。
                    if (!artwork_ui_source_matches_context() &&
                        !artwork_ui_apply_compressed_fallback(g_context_track)) {
                        artwork_ui_release_all_sources();
                        artwork_ui_show_placeholder("封面不可显示");
                    }
                }
            }
        }
    }

    if (!artwork_loader_is_ready()) return;
    ArtworkLoaderSnapshot snapshot = {};
    if (!artwork_loader_get_snapshot(&snapshot)) return;
    if (snapshot.state_revision == g_last_loader_state_revision) return;
    g_last_loader_state_revision = snapshot.state_revision;

    if (snapshot.catalog_generation != g_context_generation || snapshot.track_index != g_context_track) return;

    switch (snapshot.state) {
        case ArtworkLoadState::Loading:
            if (!artwork_ui_source_matches_context()) artwork_ui_show_waiting_without_placeholder();
            break;

        case ArtworkLoadState::Ready:
            // R.36：Surface 请求统一由 system_loop 发出，UI 只消费当前曲缓存。
            // ArtworkLoader Ready 只表示压缩原图已到 PSRAM，随后由 CoverTask 后台解码。
            if (cover_surface_cache_is_ready()) {
                if (!artwork_ui_source_matches_context()) {
                    artwork_ui_show_waiting_without_placeholder();
                }
            } else if (!artwork_ui_apply_compressed_fallback(g_context_track)) {
                artwork_ui_show_placeholder("封面不可显示");
            }
            break;

        case ArtworkLoadState::NoArtwork:
            artwork_ui_release_all_sources();
            artwork_ui_show_placeholder("暂无封面");
            break;

        case ArtworkLoadState::Failed:
            artwork_ui_release_all_sources();
            artwork_ui_show_placeholder("封面加载失败");
            break;

        case ArtworkLoadState::Stopped:
            artwork_ui_release_all_sources();
            artwork_ui_show_placeholder("封面服务不可用");
            break;

        case ArtworkLoadState::Idle:
        default:
            break;
    }
}

bool now_playing_artwork_set_dimmed(bool dimmed)
{
    g_dimmed_requested = dimmed;
    // R.36：Overlay 只切 normal/dimmed native Surface；不再分配/刷新第三张 wire。
    // R.20：最终 Surface 命中时直接在 normal / dimmed 两张 RGB565 之间切换。
    // 没有 alpha blend、JPEG/PNG decode 或 resize；调用方可把全屏黑色 backdrop 设为透明。
    if (!g_has_surface_source || g_image == nullptr ||
        g_surface_lease.normal_rgb565 == nullptr || g_surface_lease.dimmed_rgb565 == nullptr) {
        g_dimmed_applied = false;
        return false;
    }

    if (g_dimmed_applied != dimmed) {
        lv_image_set_src(
            g_image,
            dimmed ? &g_surface_dimmed_dsc : &g_surface_normal_dsc);
        g_dimmed_applied = dimmed;
        // R.34.1：lv_image_set_src() 已经负责 image 的绘制失效。
        // 禁止再次 invalidate 460x460 image，避免同一整屏 dirty 重复登记。
        ARTWORK_UI_TRACE("SURFACE_DIM_SWITCH track=%lu dim=%u",
            static_cast<unsigned long>(g_surface_lease.track_index),
            static_cast<unsigned>(g_dimmed_applied));
    }
    return true;
}

bool now_playing_artwork_has_fast_surface()
{
    return g_has_surface_source;
}

void now_playing_artwork_set_bounded_present_allowed(bool allowed)
{
    g_bounded_present_allowed = allowed;
}

bool now_playing_artwork_take_bounded_present_event()
{
    const bool pending = g_bounded_present_event_pending;
    g_bounded_present_event_pending = false;
    return pending;
}
