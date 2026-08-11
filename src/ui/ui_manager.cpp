#include "ui_manager.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_lv_decoder.h"
#include "lvgl.h"
#include "board_pins.h"
#include "cst820.h"
#include "display.h"
#include "font/font_manager.h"
#include "gesture/gesture_router.h"
#include "input/touch_input.h"
#include "lyrics/lyrics_service.h"
#include "lyrics/lyrics_view.h"
#include "spectrum/spectrum_view.h"
#include "screens/player_home.h"
#include "screens/library_view.h"

static const char *TAG = "界面";
static lv_display_t *g_display = nullptr;
static lv_indev_t *g_touch = nullptr;
static bool g_ready = false;
static esp_lv_decoder_handle_t g_image_decoder = nullptr;
static int16_t g_touch_last_x = 0;
static int16_t g_touch_last_y = 0;
static uint32_t g_touch_last_dispatch_sequence = 0U;
static bool g_touch_dispatch_sequence_valid = false;

// P1.5.3.2R.21：大面积刷新才等待 TE。
// 小型进度条/按钮局部更新继续立即刷新，避免所有 UI 交互都额外等待一帧。
static bool g_te_sync_pending = false;
static bool g_te_sync_runtime_enabled = false;
static uint8_t g_te_sync_consecutive_timeouts = 0U;
static uint32_t g_te_sync_success_count = 0U;
static uint32_t g_te_sync_timeout_count = 0U;
static uint32_t g_te_sync_timeout_ms = 25U;

// R.22：只有 Artwork 明确请求“另一首封面整帧提交”时才临时关闭面板输出。
// 这不是全局大刷新策略，歌词/频谱/Launcher 不会因此黑屏。
static bool g_present_hold_active = false;
static uint32_t g_present_hold_count = 0U;
static int64_t g_present_hold_started_us = 0;

// P1.5.3.2R.23：把 R.22 观测到的约70ms整屏周期拆成 render / flush / flush-wait。
// 只对 >=25% 屏的大刷新采样，避免进度条等小刷新刷日志。
struct UiLargeRefreshProfile
{
    bool active = false;
    int64_t refr_started_us = 0;
    int64_t render_started_us = 0;
    int64_t flush_started_us = 0;
    int64_t wait_started_us = 0;
    uint32_t render_us = 0U;
    uint32_t flush_us = 0U;
    uint32_t wait_us = 0U;
    uint16_t render_count = 0U;
    uint16_t flush_count = 0U;
};
static UiLargeRefreshProfile g_large_refresh_profile = {};
static uint32_t g_large_refresh_profile_count = 0U;

// P1.5.3.2R.30：全页面 LVGL 性能审计。
// 不改变任何页面绘制策略，只在 display event 上做轻量计数/计时，并每2秒汇总一次。
// inv_sum 是所有 invalidation 面积之和（可能重叠）；bbox 是这些 invalidation 的包围盒，
// 两者一起看可以区分“很多小对象反复失效”和“单个大对象整块失效”。
enum class UiPerfContext : uint8_t
{
    Home = 0,
    HomeOverlay,
    Launcher,
    LauncherAnim,
    Lyrics,
    LyricsMotion,
    LyricsOverlay,
    LyricsMotionOverlay,
    SpectrumSegmented,
    SpectrumHorizontal,
    SpectrumNeon,
    Library,
    LibraryInertia,
    LibrarySearch,
    LibrarySearchInertia,
};

struct UiPerfInvalidationAccum
{
    uint32_t count = 0U;
    uint64_t sum_pixels = 0U;
    uint32_t max_pixels = 0U;
    bool bbox_valid = false;
    int16_t x1 = 0;
    int16_t y1 = 0;
    int16_t x2 = 0;
    int16_t y2 = 0;
};

struct UiPerfRefreshFrame
{
    bool active = false;
    UiPerfContext context = UiPerfContext::Home;
    int64_t refr_started_us = 0;
    int64_t render_started_us = 0;
    int64_t flush_started_us = 0;
    int64_t wait_started_us = 0;
    uint32_t render_us = 0U;
    uint32_t flush_us = 0U;
    uint32_t wait_us = 0U;
    uint32_t te_wait_us = 0U;
    uint16_t render_count = 0U;
    uint16_t flush_count = 0U;
    uint32_t invalid_count = 0U;
    uint64_t invalid_sum_pixels = 0U;
    uint32_t invalid_max_pixels = 0U;
    uint32_t invalid_bbox_pixels = 0U;
};

struct UiPerfWindow
{
    bool active = false;
    UiPerfContext context = UiPerfContext::Home;
    int64_t started_us = 0;
    int64_t last_refresh_started_us = 0;
    uint32_t refresh_count = 0U;
    uint64_t total_us = 0U;
    uint64_t render_us = 0U;
    uint64_t flush_us = 0U;
    uint64_t wait_us = 0U;
    uint64_t te_wait_us = 0U;
    uint64_t invalid_sum_pixels = 0U;
    uint64_t invalid_bbox_pixels = 0U;
    uint64_t invalid_count = 0U;
    uint64_t flush_count = 0U;
    uint64_t gap_us = 0U;
    uint32_t gap_count = 0U;
    uint32_t late_gap_count = 0U;
    uint32_t max_total_us = 0U;
    uint32_t max_render_us = 0U;
    uint32_t max_flush_us = 0U;
    uint32_t max_wait_us = 0U;
    uint32_t max_te_wait_us = 0U;
    uint32_t max_invalid_sum_pixels = 0U;
    uint32_t max_invalid_bbox_pixels = 0U;
    uint32_t max_invalid_count = 0U;
    uint16_t max_flush_count = 0U;
    uint32_t max_gap_us = 0U;
};

static UiPerfInvalidationAccum g_perf_invalid = {};
static UiPerfRefreshFrame g_perf_frame = {};
static UiPerfWindow g_perf_window = {};
static lv_timer_t *g_perf_audit_timer = nullptr;
static constexpr uint32_t kPerfAuditWindowMs = 2000U;
static constexpr uint32_t kPerfScreenPixels = FAKEPOD_LCD_WIDTH * FAKEPOD_LCD_HEIGHT;

static constexpr uint32_t kTeSyncDisableAfterTimeouts = 3U;
static constexpr uint32_t kTeSyncMinPixels =
    (FAKEPOD_LCD_WIDTH * FAKEPOD_LCD_HEIGHT) / 4U;
// R.31：连续动画不能每帧都支付 0~16.7ms 的 TE 等待；但真正接近整屏的
// 页面切换仍保留 TE，因此用 90% 屏作为“强制同步”门槛。
static constexpr uint32_t kTeSyncForceFullFramePixels =
    (FAKEPOD_LCD_WIDTH * FAKEPOD_LCD_HEIGHT * 9U) / 10U;
static uint32_t g_te_animation_bypass_count = 0U;

// P1.5.3.2R.18：LVGL RGB565 DMA 条带由 20 行提升到 40 行。
// 双缓冲总像素 RAM = 460 * 40 * 2B * 2 = 73,600B。
static constexpr uint32_t kLvglDmaBufferLines = 40U;

static const char *ui_perf_context_name(UiPerfContext context)
{
    switch (context) {
        case UiPerfContext::HomeOverlay: return "主页/Overlay";
        case UiPerfContext::Launcher: return "Launcher/静态";
        case UiPerfContext::LauncherAnim: return "Launcher/动画";
        case UiPerfContext::Lyrics: return "歌词/静态";
        case UiPerfContext::LyricsMotion: return "歌词/缓动";
        case UiPerfContext::LyricsOverlay: return "歌词/Overlay";
        case UiPerfContext::LyricsMotionOverlay: return "歌词/缓动+Overlay";
        case UiPerfContext::SpectrumSegmented: return "频谱/SegmentedColumns";
        case UiPerfContext::SpectrumHorizontal: return "频谱/HorizontalMirror";
        case UiPerfContext::SpectrumNeon: return "频谱/NeonRidge";
        case UiPerfContext::Library: return "曲库/列表";
        case UiPerfContext::LibraryInertia: return "曲库/惯性";
        case UiPerfContext::LibrarySearch: return "曲库/搜索";
        case UiPerfContext::LibrarySearchInertia: return "曲库/搜索惯性";
        case UiPerfContext::Home:
        default:
            return "主页/封面";
    }
}

static uint32_t ui_perf_context_target_period_ms(UiPerfContext context)
{
    switch (context) {
        case UiPerfContext::LauncherAnim:
        case UiPerfContext::LibraryInertia:
        case UiPerfContext::LibrarySearchInertia:
            return 16U;
        case UiPerfContext::LyricsMotion:
        case UiPerfContext::LyricsMotionOverlay:
            return 33U;
        case UiPerfContext::SpectrumSegmented:
        case UiPerfContext::SpectrumHorizontal:
        case UiPerfContext::SpectrumNeon:
            return 50U;
        default:
            return 0U;
    }
}

static UiPerfContext ui_perf_resolve_context()
{
    if (library_view_is_visible()) {
        const bool search = library_view_search_is_active();
        const bool inertia = library_view_inertia_is_active();
        if (search && inertia) return UiPerfContext::LibrarySearchInertia;
        if (search) return UiPerfContext::LibrarySearch;
        if (inertia) return UiPerfContext::LibraryInertia;
        return UiPerfContext::Library;
    }

    if (lyrics_view_is_visible()) {
        const bool motion = lyrics_view_motion_is_active();
        const bool overlay = lyrics_view_overlay_is_visible();
        if (motion && overlay) return UiPerfContext::LyricsMotionOverlay;
        if (motion) return UiPerfContext::LyricsMotion;
        if (overlay) return UiPerfContext::LyricsOverlay;
        return UiPerfContext::Lyrics;
    }

    if (spectrum_view_is_visible()) {
        const char *style = spectrum_view_current_style_name();
        if (style != nullptr && strcmp(style, "HorizontalMirror") == 0) {
            return UiPerfContext::SpectrumHorizontal;
        }
        if (style != nullptr && strcmp(style, "NeonRidge") == 0) {
            return UiPerfContext::SpectrumNeon;
        }
        return UiPerfContext::SpectrumSegmented;
    }

    if (player_home_launcher_is_visible()) {
        return player_home_launcher_is_animating()
            ? UiPerfContext::LauncherAnim
            : UiPerfContext::Launcher;
    }
    if (player_home_overlay_is_visible()) {
        return UiPerfContext::HomeOverlay;
    }
    return UiPerfContext::Home;
}

static bool ui_te_context_is_continuous_animation(UiPerfContext context)
{
    switch (context) {
        case UiPerfContext::LauncherAnim:
        case UiPerfContext::LyricsMotion:
        case UiPerfContext::LyricsMotionOverlay:
        case UiPerfContext::SpectrumSegmented:
        case UiPerfContext::SpectrumHorizontal:
        case UiPerfContext::SpectrumNeon:
        case UiPerfContext::LibraryInertia:
        case UiPerfContext::LibrarySearchInertia:
            return true;
        default:
            return false;
    }
}

static void ui_perf_reset_window(UiPerfContext context, int64_t now_us)
{
    g_perf_window = {};
    g_perf_window.active = true;
    g_perf_window.context = context;
    g_perf_window.started_us = now_us;
}

static void ui_perf_dump_window(int64_t now_us, bool context_switch)
{
    if (!g_perf_window.active || g_perf_window.refresh_count == 0U) {
        return;
    }

    uint32_t elapsed_ms = now_us > g_perf_window.started_us
        ? static_cast<uint32_t>((now_us - g_perf_window.started_us) / 1000LL)
        : 0U;
    if (elapsed_ms == 0U) elapsed_ms = 1U;

    const uint32_t n = g_perf_window.refresh_count;
    const uint32_t hz_x10 = static_cast<uint32_t>(
        (static_cast<uint64_t>(n) * 10000ULL) / elapsed_ms);
    const uint32_t avg_total = static_cast<uint32_t>(g_perf_window.total_us / n);
    const uint32_t avg_render = static_cast<uint32_t>(g_perf_window.render_us / n);
    const uint32_t avg_flush = static_cast<uint32_t>(g_perf_window.flush_us / n);
    const uint32_t avg_wait = static_cast<uint32_t>(g_perf_window.wait_us / n);
    const uint32_t avg_te = static_cast<uint32_t>(g_perf_window.te_wait_us / n);
    const uint32_t avg_inv_sum = static_cast<uint32_t>(g_perf_window.invalid_sum_pixels / n);
    const uint32_t avg_bbox = static_cast<uint32_t>(g_perf_window.invalid_bbox_pixels / n);
    const uint32_t avg_inv_count_x10 = static_cast<uint32_t>(
        (g_perf_window.invalid_count * 10ULL) / n);
    const uint32_t avg_flush_count_x10 = static_cast<uint32_t>(
        (g_perf_window.flush_count * 10ULL) / n);
    const uint32_t avg_gap = g_perf_window.gap_count > 0U
        ? static_cast<uint32_t>(g_perf_window.gap_us / g_perf_window.gap_count)
        : 0U;
    const uint32_t avg_bbox_pct = static_cast<uint32_t>(
        (static_cast<uint64_t>(avg_bbox) * 100ULL) / kPerfScreenPixels);
    const uint32_t max_bbox_pct = static_cast<uint32_t>(
        (static_cast<uint64_t>(g_perf_window.max_invalid_bbox_pixels) * 100ULL) / kPerfScreenPixels);

    ESP_LOGI(TAG,
        "R.30 LVGL审计[%s] window=%ums refresh=%u hz=%u.%u total(avg/max)=%u/%uus render=%u/%uus flush=%u/%uus wait=%u/%uus TE=%u/%uus gap=%u/%uus late=%u%s",
        ui_perf_context_name(g_perf_window.context),
        static_cast<unsigned>(elapsed_ms),
        static_cast<unsigned>(n),
        static_cast<unsigned>(hz_x10 / 10U),
        static_cast<unsigned>(hz_x10 % 10U),
        static_cast<unsigned>(avg_total),
        static_cast<unsigned>(g_perf_window.max_total_us),
        static_cast<unsigned>(avg_render),
        static_cast<unsigned>(g_perf_window.max_render_us),
        static_cast<unsigned>(avg_flush),
        static_cast<unsigned>(g_perf_window.max_flush_us),
        static_cast<unsigned>(avg_wait),
        static_cast<unsigned>(g_perf_window.max_wait_us),
        static_cast<unsigned>(avg_te),
        static_cast<unsigned>(g_perf_window.max_te_wait_us),
        static_cast<unsigned>(avg_gap),
        static_cast<unsigned>(g_perf_window.max_gap_us),
        static_cast<unsigned>(g_perf_window.late_gap_count),
        context_switch ? " switch" : "");

    ESP_LOGI(TAG,
        "R.30 LVGL失效[%s] inv(avg/max)=%u.%u/%u次 sum(avg/max)=%u/%upx bbox(avg/max)=%u%%/%u%% flushN(avg/max)=%u.%u/%u DMAfree=%u largest=%u PSRAM=%u",
        ui_perf_context_name(g_perf_window.context),
        static_cast<unsigned>(avg_inv_count_x10 / 10U),
        static_cast<unsigned>(avg_inv_count_x10 % 10U),
        static_cast<unsigned>(g_perf_window.max_invalid_count),
        static_cast<unsigned>(avg_inv_sum),
        static_cast<unsigned>(g_perf_window.max_invalid_sum_pixels),
        static_cast<unsigned>(avg_bbox_pct),
        static_cast<unsigned>(max_bbox_pct),
        static_cast<unsigned>(avg_flush_count_x10 / 10U),
        static_cast<unsigned>(avg_flush_count_x10 % 10U),
        static_cast<unsigned>(g_perf_window.max_flush_count),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

static void ui_perf_ensure_window(UiPerfContext context, int64_t now_us)
{
    if (!g_perf_window.active) {
        ui_perf_reset_window(context, now_us);
        return;
    }
    if (g_perf_window.context != context) {
        ui_perf_dump_window(now_us, true);
        ui_perf_reset_window(context, now_us);
    }
}

static void ui_perf_note_invalidation(const lv_area_t &area)
{
    const int32_t width = area.x2 - area.x1 + 1;
    const int32_t height = area.y2 - area.y1 + 1;
    if (width <= 0 || height <= 0) {
        return;
    }
    const uint32_t pixels = static_cast<uint32_t>(width) * static_cast<uint32_t>(height);
    ++g_perf_invalid.count;
    g_perf_invalid.sum_pixels += pixels;
    if (pixels > g_perf_invalid.max_pixels) g_perf_invalid.max_pixels = pixels;
    if (!g_perf_invalid.bbox_valid) {
        g_perf_invalid.bbox_valid = true;
        g_perf_invalid.x1 = area.x1; g_perf_invalid.y1 = area.y1;
        g_perf_invalid.x2 = area.x2; g_perf_invalid.y2 = area.y2;
    } else {
        if (area.x1 < g_perf_invalid.x1) g_perf_invalid.x1 = area.x1;
        if (area.y1 < g_perf_invalid.y1) g_perf_invalid.y1 = area.y1;
        if (area.x2 > g_perf_invalid.x2) g_perf_invalid.x2 = area.x2;
        if (area.y2 > g_perf_invalid.y2) g_perf_invalid.y2 = area.y2;
    }
}

static void ui_perf_begin_refresh(int64_t now_us)
{
    const UiPerfContext context = ui_perf_resolve_context();
    ui_perf_ensure_window(context, now_us);

    g_perf_frame = {};
    g_perf_frame.active = true;
    g_perf_frame.context = context;
    g_perf_frame.refr_started_us = now_us;
    g_perf_frame.invalid_count = g_perf_invalid.count;
    g_perf_frame.invalid_sum_pixels = g_perf_invalid.sum_pixels;
    g_perf_frame.invalid_max_pixels = g_perf_invalid.max_pixels;
    if (g_perf_invalid.bbox_valid) {
        const int32_t width = g_perf_invalid.x2 - g_perf_invalid.x1 + 1;
        const int32_t height = g_perf_invalid.y2 - g_perf_invalid.y1 + 1;
        if (width > 0 && height > 0) {
            g_perf_frame.invalid_bbox_pixels =
                static_cast<uint32_t>(width) * static_cast<uint32_t>(height);
        }
    }
    g_perf_invalid = {};
}

static void ui_perf_finalize_refresh(int64_t now_us)
{
    if (!g_perf_frame.active) {
        return;
    }
    ui_perf_ensure_window(g_perf_frame.context, now_us);
    const uint32_t total_us = now_us > g_perf_frame.refr_started_us
        ? static_cast<uint32_t>(now_us - g_perf_frame.refr_started_us)
        : 0U;
    UiPerfWindow &w = g_perf_window;

    if (w.last_refresh_started_us != 0 && g_perf_frame.refr_started_us > w.last_refresh_started_us) {
        const uint32_t gap_us = static_cast<uint32_t>(
            g_perf_frame.refr_started_us - w.last_refresh_started_us);
        w.gap_us += gap_us;
        ++w.gap_count;
        if (gap_us > w.max_gap_us) w.max_gap_us = gap_us;
        const uint32_t target_ms = ui_perf_context_target_period_ms(w.context);
        if (target_ms > 0U && gap_us > target_ms * 1500U) {
            ++w.late_gap_count;
        }
    }
    w.last_refresh_started_us = g_perf_frame.refr_started_us;

    ++w.refresh_count;
    w.total_us += total_us;
    w.render_us += g_perf_frame.render_us;
    w.flush_us += g_perf_frame.flush_us;
    w.wait_us += g_perf_frame.wait_us;
    w.te_wait_us += g_perf_frame.te_wait_us;
    w.invalid_sum_pixels += g_perf_frame.invalid_sum_pixels;
    w.invalid_bbox_pixels += g_perf_frame.invalid_bbox_pixels;
    w.invalid_count += g_perf_frame.invalid_count;
    w.flush_count += g_perf_frame.flush_count;

    if (total_us > w.max_total_us) w.max_total_us = total_us;
    if (g_perf_frame.render_us > w.max_render_us) w.max_render_us = g_perf_frame.render_us;
    if (g_perf_frame.flush_us > w.max_flush_us) w.max_flush_us = g_perf_frame.flush_us;
    if (g_perf_frame.wait_us > w.max_wait_us) w.max_wait_us = g_perf_frame.wait_us;
    if (g_perf_frame.te_wait_us > w.max_te_wait_us) w.max_te_wait_us = g_perf_frame.te_wait_us;
    const uint32_t inv_sum_clamped = g_perf_frame.invalid_sum_pixels > UINT32_MAX
        ? UINT32_MAX : static_cast<uint32_t>(g_perf_frame.invalid_sum_pixels);
    if (inv_sum_clamped > w.max_invalid_sum_pixels) w.max_invalid_sum_pixels = inv_sum_clamped;
    if (g_perf_frame.invalid_bbox_pixels > w.max_invalid_bbox_pixels) {
        w.max_invalid_bbox_pixels = g_perf_frame.invalid_bbox_pixels;
    }
    if (g_perf_frame.invalid_count > w.max_invalid_count) w.max_invalid_count = g_perf_frame.invalid_count;
    if (g_perf_frame.flush_count > w.max_flush_count) w.max_flush_count = g_perf_frame.flush_count;

    g_perf_frame = {};
}

static void ui_perf_audit_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const int64_t now_us = esp_timer_get_time();
    const UiPerfContext context = ui_perf_resolve_context();
    ui_perf_ensure_window(context, now_us);
    if (g_perf_window.active &&
        now_us - g_perf_window.started_us >= static_cast<int64_t>(kPerfAuditWindowMs) * 1000LL) {
        ui_perf_dump_window(now_us, false);
        ui_perf_reset_window(context, now_us);
    }
}

// CO5300 对局部刷新窗口有偶数对齐要求：
// 起始 X/Y 必须为偶数，刷新宽度和高度也必须为偶数。
static void ui_display_align_area_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_INVALIDATE_AREA) {
        return;
    }

    lv_area_t *area = static_cast<lv_area_t *>(lv_event_get_param(event));
    if (area == nullptr) {
        return;
    }

    if (area->x1 < 0) area->x1 = 0;
    if (area->y1 < 0) area->y1 = 0;
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;

    const int32_t max_x = FAKEPOD_LCD_WIDTH - 1;
    const int32_t max_y = FAKEPOD_LCD_HEIGHT - 1;
    if (area->x2 > max_x) area->x2 = max_x;
    if (area->y2 > max_y) area->y2 = max_y;

    ui_perf_note_invalidation(*area);

    if (g_te_sync_runtime_enabled) {
        const int32_t width = area->x2 - area->x1 + 1;
        const int32_t height = area->y2 - area->y1 + 1;
        if (width > 0 && height > 0) {
            const uint32_t pixels =
                static_cast<uint32_t>(width) * static_cast<uint32_t>(height);
            if (pixels >= kTeSyncMinPixels) {
                const UiPerfContext context = ui_perf_resolve_context();
                const bool continuous_animation =
                    ui_te_context_is_continuous_animation(context);
                // R.31：频谱 / Launcher / 歌词缓动 / 惯性滚动的中等面积动画帧
                // 直接刷新；>=90% 屏的真正页面切换仍同步 TE。
                if (!continuous_animation || pixels >= kTeSyncForceFullFramePixels) {
                    g_te_sync_pending = true;
                } else {
                    ++g_te_animation_bypass_count;
                }
            }
        }
    }
}

static void ui_display_profile_cb(lv_event_t *event)
{
    if (event == nullptr) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    const lv_event_code_t code = lv_event_get_code(event);

    // R.30：所有刷新都记录，供页面级2秒窗口汇总。
    if (g_perf_frame.active) {
        switch (code) {
            case LV_EVENT_RENDER_START:
                if (g_perf_frame.render_started_us == 0) g_perf_frame.render_started_us = now_us;
                ++g_perf_frame.render_count;
                break;
            case LV_EVENT_RENDER_READY:
                if (g_perf_frame.render_started_us != 0) {
                    g_perf_frame.render_us += static_cast<uint32_t>(now_us - g_perf_frame.render_started_us);
                    g_perf_frame.render_started_us = 0;
                }
                break;
            case LV_EVENT_FLUSH_START:
                if (g_perf_frame.flush_started_us == 0) g_perf_frame.flush_started_us = now_us;
                ++g_perf_frame.flush_count;
                break;
            case LV_EVENT_FLUSH_FINISH:
                if (g_perf_frame.flush_started_us != 0) {
                    g_perf_frame.flush_us += static_cast<uint32_t>(now_us - g_perf_frame.flush_started_us);
                    g_perf_frame.flush_started_us = 0;
                }
                break;
            case LV_EVENT_FLUSH_WAIT_START:
                if (g_perf_frame.wait_started_us == 0) g_perf_frame.wait_started_us = now_us;
                break;
            case LV_EVENT_FLUSH_WAIT_FINISH:
                if (g_perf_frame.wait_started_us != 0) {
                    g_perf_frame.wait_us += static_cast<uint32_t>(now_us - g_perf_frame.wait_started_us);
                    g_perf_frame.wait_started_us = 0;
                }
                break;
            default:
                break;
        }
    }

    // 保留 R.23 的大刷新逐帧诊断，便于和历史日志直接对比。
    if (!g_large_refresh_profile.active) {
        return;
    }
    switch (code) {
        case LV_EVENT_RENDER_START:
            if (g_large_refresh_profile.render_started_us == 0) {
                g_large_refresh_profile.render_started_us = now_us;
            }
            ++g_large_refresh_profile.render_count;
            break;
        case LV_EVENT_RENDER_READY:
            if (g_large_refresh_profile.render_started_us != 0) {
                g_large_refresh_profile.render_us += static_cast<uint32_t>(
                    now_us - g_large_refresh_profile.render_started_us);
                g_large_refresh_profile.render_started_us = 0;
            }
            break;
        case LV_EVENT_FLUSH_START:
            if (g_large_refresh_profile.flush_started_us == 0) {
                g_large_refresh_profile.flush_started_us = now_us;
            }
            ++g_large_refresh_profile.flush_count;
            break;
        case LV_EVENT_FLUSH_FINISH:
            if (g_large_refresh_profile.flush_started_us != 0) {
                g_large_refresh_profile.flush_us += static_cast<uint32_t>(
                    now_us - g_large_refresh_profile.flush_started_us);
                g_large_refresh_profile.flush_started_us = 0;
            }
            break;
        case LV_EVENT_FLUSH_WAIT_START:
            if (g_large_refresh_profile.wait_started_us == 0) {
                g_large_refresh_profile.wait_started_us = now_us;
            }
            break;
        case LV_EVENT_FLUSH_WAIT_FINISH:
            if (g_large_refresh_profile.wait_started_us != 0) {
                g_large_refresh_profile.wait_us += static_cast<uint32_t>(
                    now_us - g_large_refresh_profile.wait_started_us);
                g_large_refresh_profile.wait_started_us = 0;
            }
            break;
        default:
            break;
    }
}


// R.22：刷新周期开始时先完成 R.21 TE 对齐，再按需暂停面板输出。
// display_present_take_hold_request() 只会被“跨 Track 封面 Source 替换”触发，
// 因此普通大面积页面切换继续只做 TE 同步，不会产生额外黑场。
static void ui_display_refresh_start_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_REFR_START) {
        return;
    }

    const int64_t refresh_started_us = esp_timer_get_time();
    ui_perf_begin_refresh(refresh_started_us);

    const bool large_refresh = g_te_sync_pending;
    const bool present_requested = display_present_take_hold_request();
    g_te_sync_pending = false;

    if (large_refresh) {
        g_large_refresh_profile = {};
        g_large_refresh_profile.active = true;
        g_large_refresh_profile.refr_started_us = esp_timer_get_time();
    }

    if (g_te_sync_runtime_enabled && large_refresh) {
        const int64_t wait_started_us = esp_timer_get_time();
        if (display_te_wait_next(g_te_sync_timeout_ms)) {
            const uint32_t waited_us = static_cast<uint32_t>(
                esp_timer_get_time() - wait_started_us);
            if (g_perf_frame.active) g_perf_frame.te_wait_us = waited_us;
            ++g_te_sync_success_count;
            g_te_sync_consecutive_timeouts = 0U;
            if (g_te_sync_success_count <= 3U || (g_te_sync_success_count % 60U) == 0U) {
                ESP_LOGI(TAG,
                    "R.21 TE对齐成功：count=%u wait=%uus",
                    static_cast<unsigned>(g_te_sync_success_count),
                    static_cast<unsigned>(waited_us));
            }
        } else {
            if (g_perf_frame.active) {
                g_perf_frame.te_wait_us = static_cast<uint32_t>(
                    esp_timer_get_time() - wait_started_us);
            }
            ++g_te_sync_timeout_count;
            ++g_te_sync_consecutive_timeouts;
            ESP_LOGW(TAG,
                "R.21 TE等待超时：%ums，连续=%u success=%u timeout=%u",
                static_cast<unsigned>(g_te_sync_timeout_ms),
                static_cast<unsigned>(g_te_sync_consecutive_timeouts),
                static_cast<unsigned>(g_te_sync_success_count),
                static_cast<unsigned>(g_te_sync_timeout_count));

            if (g_te_sync_consecutive_timeouts >= kTeSyncDisableAfterTimeouts) {
                g_te_sync_runtime_enabled = false;
                ESP_LOGW(TAG, "R.21 TE运行期自动降级：连续%u次超时，后续刷新不再等待TE",
                    static_cast<unsigned>(kTeSyncDisableAfterTimeouts));
            }
        }
    }

    if (!present_requested) {
        return;
    }

    // 请求是在 lv_image_set_src() 之前发出；正常情况下这一轮会包含 460x460 封面 invalidation。
    // 即便区域合并方式发生变化，也宁可只 hold 一轮刷新，不把请求泄漏到下一次 UI 更新。
    if (!display_present_set_output(false)) {
        ESP_LOGW(TAG, "R.23 PresentHold回退：无法暂停面板输出，本轮退回可见刷新");
        return;
    }

    g_present_hold_active = true;
    g_present_hold_started_us = esp_timer_get_time();
}


static void ui_display_refresh_ready_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_REFR_READY) {
        return;
    }

    ui_perf_finalize_refresh(esp_timer_get_time());

    if (g_large_refresh_profile.active) {
        const uint32_t total_us = static_cast<uint32_t>(
            esp_timer_get_time() - g_large_refresh_profile.refr_started_us);
        ++g_large_refresh_profile_count;
        if (g_large_refresh_profile_count <= 12U || (g_large_refresh_profile_count % 60U) == 0U) {
            ESP_LOGI(TAG,
                "R.23 LVGL大刷新画像：count=%u total=%uus render=%uus(%u) flush=%uus(%u) wait=%uus",
                static_cast<unsigned>(g_large_refresh_profile_count),
                static_cast<unsigned>(total_us),
                static_cast<unsigned>(g_large_refresh_profile.render_us),
                static_cast<unsigned>(g_large_refresh_profile.render_count),
                static_cast<unsigned>(g_large_refresh_profile.flush_us),
                static_cast<unsigned>(g_large_refresh_profile.flush_count),
                static_cast<unsigned>(g_large_refresh_profile.wait_us));
        }
        g_large_refresh_profile = {};
    }

    if (!g_present_hold_active) {
        return;
    }

    const uint32_t held_us = static_cast<uint32_t>(
        esp_timer_get_time() - g_present_hold_started_us);

    // R.23 正常跨Track应优先走 DirectPresent；这里只保留 R.22 兼容回退。
    const bool restored = display_present_set_output(true);
    g_present_hold_active = false;
    g_present_hold_started_us = 0;
    ++g_present_hold_count;

    if (g_present_hold_count <= 8U || (g_present_hold_count % 30U) == 0U || !restored) {
        ESP_LOGI(TAG,
            "R.23 PresentHold回退完成：count=%u hidden=%uus restored=%u",
            static_cast<unsigned>(g_present_hold_count),
            static_cast<unsigned>(held_us),
            static_cast<unsigned>(restored));
    }
}

// 输入设备一旦准备进入滚动状态，就在送到控件前终止本轮滚动处理。
static void ui_touch_block_scroll_cb(lv_event_t *event)
{
    (void)event;
    if (g_touch == nullptr) {
        return;
    }

    lv_obj_t *scroll_obj = lv_indev_get_scroll_obj(g_touch);
    if (scroll_obj != nullptr) {
        lv_obj_scroll_to(scroll_obj, 0, 0, LV_ANIM_OFF);
    }
    lv_indev_stop_processing(g_touch);
}

static uint16_t ui_clamp_coord(uint16_t value, uint16_t max_value)
{
    return value > max_value ? max_value : value;
}

// P1.5R.1.2：LVGL 输入回调只消费 TouchInputTask 发布的边沿队列/最新坐标快照。
// DOWN/UP 走边沿队列；R.33.2.3 在生产端做 RELEASE debounce + 满队列背压合并；MOVE 只按最新 snapshot 分发。
static void ui_touch_dispatch_pointer(bool pressed, int16_t x, int16_t y, uint32_t tick_ms)
{
    if (!library_view_is_visible()) {
        gesture_router_feed_pointer(pressed, x, y, tick_ms);
    } else {
        gesture_router_reset();
        library_view_feed_pointer(pressed, x, y, tick_ms);
    }
}

static void ui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    UiTouchEdgeEvent edge = {};
    if (ui_touch_input_take_edge(&edge)) {
        data->state = edge.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->point.x = edge.x;
        data->point.y = edge.y;
        g_touch_last_x = edge.x;
        g_touch_last_y = edge.y;
        ui_touch_dispatch_pointer(edge.pressed, edge.x, edge.y, edge.tick_ms);
        g_touch_last_dispatch_sequence = edge.sequence;
        g_touch_dispatch_sequence_valid = true;
        data->continue_reading = ui_touch_input_has_pending_edge();
        return;
    }

    UiTouchSnapshot snapshot = {};
    if (ui_touch_input_get_snapshot(&snapshot)) {
        data->state = snapshot.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        if (snapshot.pressed) {
            g_touch_last_x = snapshot.x;
            g_touch_last_y = snapshot.y;
        }
        data->point.x = g_touch_last_x;
        data->point.y = g_touch_last_y;
        if (!g_touch_dispatch_sequence_valid ||
            snapshot.sequence != g_touch_last_dispatch_sequence) {
            ui_touch_dispatch_pointer(
                snapshot.pressed, g_touch_last_x, g_touch_last_y, snapshot.tick_ms);
            g_touch_last_dispatch_sequence = snapshot.sequence;
            g_touch_dispatch_sequence_valid = true;
        }
        data->continue_reading = false;
        return;
    }

    // TouchInputTask 启动失败时保留旧同步读取作为降级路径，避免触摸完全失效。
    CST820Point point = {};
    const esp_err_t ret = cst820_read_point(&point);
    if (ret == ESP_OK && point.pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = ui_clamp_coord(point.x, FAKEPOD_LCD_WIDTH - 1);
        data->point.y = ui_clamp_coord(point.y, FAKEPOD_LCD_HEIGHT - 1);
        g_touch_last_x = data->point.x;
        g_touch_last_y = data->point.y;
        ui_touch_dispatch_pointer(
            true, g_touch_last_x, g_touch_last_y, static_cast<uint32_t>(lv_tick_get()));
        return;
    }

    data->state = LV_INDEV_STATE_RELEASED;
    data->point.x = g_touch_last_x;
    data->point.y = g_touch_last_y;
    ui_touch_dispatch_pointer(
        false, g_touch_last_x, g_touch_last_y, static_cast<uint32_t>(lv_tick_get()));
}

esp_err_t ui_manager_init()
{
    if (g_ready) {
        return ESP_OK;
    }

    if (!display_is_ready() || !cst820_is_ready()) {
        ESP_LOGE(TAG, "显示屏或触摸尚未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "正在初始化 LVGL 9");
    lvgl_port_cfg_t lvgl_cfg = {};
    // P1.5R.1：Core1 实时优先级阶梯。FLAC 预取固定 P4，LVGL 降为 P3，
    // 保证持续 UI 刷新时只要 FlacPrefetch Ready，就能先获得 CPU。
    lvgl_cfg.task_priority = 3;
    lvgl_cfg.task_stack = 6144;
    lvgl_cfg.task_affinity = 1;
    lvgl_cfg.task_max_sleep_ms = 100;
    lvgl_cfg.timer_period_ms = 5;

    esp_err_t ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL Port 初始化失败：%s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "LVGL任务配置：core=1 priority=%u stack=%uB",
        static_cast<unsigned>(lvgl_cfg.task_priority), static_cast<unsigned>(lvgl_cfg.task_stack));
    ESP_LOGI(TAG, "P1.5R.1 Core1 Priority Ladder：FlacPrefetch=P4 > LVGL=P3 > Artwork=P2 > Cover/Lyrics=P1");

    ESP_LOGI(TAG, "正在注册 CO5300 显示设备");
    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = display_get_panel_io();
    disp_cfg.panel_handle = display_get_panel();
    disp_cfg.buffer_size = FAKEPOD_LCD_WIDTH * kLvglDmaBufferLines;
    disp_cfg.double_buffer = true;
    disp_cfg.hres = FAKEPOD_LCD_WIDTH;
    disp_cfg.vres = FAKEPOD_LCD_HEIGHT;
    disp_cfg.monochrome = false;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    disp_cfg.rotation.swap_xy = false;
    disp_cfg.rotation.mirror_x = false;
    disp_cfg.rotation.mirror_y = false;
    disp_cfg.flags.buff_dma = true;
    disp_cfg.flags.swap_bytes = true;

    const size_t dma_free_before = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t dma_largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    g_display = lvgl_port_add_disp(&disp_cfg);
    if (g_display == nullptr) {
        ESP_LOGE(TAG, "注册 LVGL 显示设备失败");
        return ESP_FAIL;
    }

    // R.26：esp_lvgl_port_add_disp() 会覆盖 Panel IO 的 color-done callback。
    // 立即换成统一桥接，保持 LVGL flush_ready，同时给 DirectPresent 单独的 DMA 完成信号。
    ret = display_install_lvgl_color_done_bridge(g_display);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "R.26 安装显示color-done桥接失败：%s", esp_err_to_name(ret));
        return ret;
    }

    const size_t dma_free_after = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t dma_largest_after = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG,
        "R.18 LVGL DMA双缓冲：%u行/块，理论总计=%uB；DMA free=%u->%u largest=%u->%u",
        static_cast<unsigned>(kLvglDmaBufferLines),
        static_cast<unsigned>(FAKEPOD_LCD_WIDTH * kLvglDmaBufferLines * 2U * 2U),
        static_cast<unsigned>(dma_free_before),
        static_cast<unsigned>(dma_free_after),
        static_cast<unsigned>(dma_largest_before),
        static_cast<unsigned>(dma_largest_after));

    g_te_sync_runtime_enabled = display_te_is_ready();
    g_te_sync_pending = false;
    g_te_sync_consecutive_timeouts = 0U;
    g_te_sync_success_count = 0U;
    g_te_sync_timeout_count = 0U;

    const uint32_t te_period_us = display_te_get_period_us();
    if (te_period_us > 0U) {
        const uint32_t period_ms_ceil = (te_period_us + 999U) / 1000U;
        uint32_t timeout_ms = period_ms_ceil + 8U;
        if (timeout_ms < 25U) timeout_ms = 25U;
        if (timeout_ms > 50U) timeout_ms = 50U;
        g_te_sync_timeout_ms = timeout_ms;
    }

    // P1.5.3.2R.32.1：lvgl_port_init() 启动独立 LVGL task 后，所有直接 lv_* 调用都必须
    // 与 lv_timer_handler() 共用同一把 port mutex。此前 display event callback 注册发生在锁外，
    // 启动时若恰好与 Core1 LVGL task 并发修改 event/TLSF 链表，会卡在 lv_tlsf_malloc()。
    // 初始化阶段不需要抢占式失败，因此使用 -1 永久等待，并一直持锁到全部 UI 对象创建完成。
    if (!lvgl_port_lock(-1)) {
        ESP_LOGE(TAG, "R.32.1 获取 LVGL 初始化互斥锁失败");
        return ESP_FAIL;
    }

    lv_display_add_event_cb(g_display, ui_display_align_area_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_display_add_event_cb(g_display, ui_display_refresh_start_cb, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(g_display, ui_display_refresh_ready_cb, LV_EVENT_REFR_READY, nullptr);
    lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_RENDER_READY, nullptr);
    lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_FLUSH_START, nullptr);
    lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_FLUSH_FINISH, nullptr);
    lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_FLUSH_WAIT_START, nullptr);
    lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_FLUSH_WAIT_FINISH, nullptr);
    ESP_LOGI(TAG, "R.32.1 LVGL初始化互斥：display callbacks -> indev -> decoder -> screens 全程持锁");
    ESP_LOGI(TAG, "已启用 CO5300 局部刷新偶数对齐");
    if (g_te_sync_runtime_enabled) {
        ESP_LOGI(TAG,
            "R.31 TE策略已启用：静态大刷新阈值=%u像素；连续动画中等刷新绕过TE，>=90%%屏仍同步；timeout=%ums，TE period=%uus",
            static_cast<unsigned>(kTeSyncMinPixels),
            static_cast<unsigned>(g_te_sync_timeout_ms),
            static_cast<unsigned>(te_period_us));
    } else {
        ESP_LOGW(TAG, "R.21 LVGL TE同步未启用，保持无TE刷新路径");
    }
    ESP_LOGI(TAG, "R.23 Direct Surface Present：跨Track优先绕过LVGL整屏image render；R.22 Display Hold仅作失败回退，QSPI=50MHz");

    ESP_LOGI(TAG, "正在注册 CST820 触摸输入");
    gesture_router_reset();
    const esp_err_t touch_fast_ret = ui_touch_input_start();
    if (touch_fast_ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch Fast Path 启动失败，将降级为 LVGL 同步读取 CST820：%s",
            esp_err_to_name(touch_fast_ret));
    }
    // R.32.1：这里已经持有初始化互斥锁，不再二次 lock。
    g_touch = lv_indev_create();
    if (g_touch == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "创建 LVGL 触摸输入失败");
        return ESP_ERR_NO_MEM;
    }

    lv_indev_set_type(g_touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_touch, ui_touch_read_cb);
    lv_indev_set_display(g_touch, g_display);
    // P1.3.4.2：所有页面都不依赖 LVGL 原生滚动手势；曲库由 CST820 原始坐标直驱虚拟列表。
    // 这里保留 8px 仅用于 LVGL 自身的 click/drag 判定，真正滚动不再受它影响。
    lv_indev_set_scroll_limit(g_touch, 8);
    lv_indev_add_event_cb(g_touch, ui_touch_block_scroll_cb, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_indev_add_event_cb(g_touch, ui_touch_block_scroll_cb, LV_EVENT_SCROLL, nullptr);

    // Stage 12.2：注册 Espressif LVGL JPEG/PNG 内存解码器。
    // ArtworkLoader 已把压缩图放入 PSRAM，LVGL 只消费内存变量，不再访问 SD。
    const esp_err_t decoder_ret = esp_lv_decoder_init(&g_image_decoder);
    if (decoder_ret != ESP_OK) {
        ESP_LOGW(TAG, "JPEG/PNG 图片解码器初始化失败，首页将使用默认封面：%s", esp_err_to_name(decoder_ret));
        g_image_decoder = nullptr;
    } else {
        // P1.2 正常播放器封面已由 CoverSurfaceTask 预处理成 RGB565，不再依赖 LVGL decoded cache。
        // 这里只给 progressive JPEG 等兼容回退和后续普通图片控件保留小缓存，避免与两张 460x460
        // cover surface 同时长期占用数 MB PSRAM。
        lv_image_cache_resize(512U * 1024U, true);
    }

    const esp_err_t font_ret = font_manager_init();
    if (font_ret != ESP_OK) {
        ESP_LOGW(TAG, "原厂中文字体初始化失败，将使用 LVGL 默认字体：%s", esp_err_to_name(font_ret));
    }
    const esp_err_t lyrics_ret = lyrics_service_start();
    if (lyrics_ret != ESP_OK) {
        ESP_LOGW(TAG, "LyricsTask 启动失败，歌词页将显示不可用：%s", esp_err_to_name(lyrics_ret));
    }
    player_home_create(lv_screen_active());
    lyrics_view_create(lv_screen_active());
    spectrum_view_create(lv_screen_active());
    library_view_create(lv_screen_active());
    g_perf_audit_timer = lv_timer_create(ui_perf_audit_timer_cb, kPerfAuditWindowMs, nullptr);
    if (g_perf_audit_timer == nullptr) {
        ESP_LOGW(TAG, "R.30 性能审计汇总timer创建失败，仅保留逐刷新计数");
    }
    lvgl_port_unlock();

    ESP_LOGI(TAG,
        "R.30 全页面LVGL性能审计已启用：2s窗口；主页/Overlay/Launcher动画、歌词静态/缓动/Overlay、三种频谱、曲库列表/惯性/搜索分别统计");
    g_ready = true;
    ESP_LOGI(TAG, "Stage 6 正式 UI 基础框架初始化成功");
    return ESP_OK;
}

bool ui_manager_is_ready()
{
    return g_ready;
}
