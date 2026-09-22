#include "ui_manager.h"
#include "ui_common.h"

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_lvgl_port.h"
#include "esp_lv_decoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "app_diag_config.h"
#include "board_pins.h"
#include "cst820.h"
#include "sdcard.h"
#include "media_library.h"
#include "display.h"
#include "display_backend.h"
#include "font/font_manager.h"
#include "font/usb_service_font.h"
#include "gesture/gesture_router.h"
#include "input/touch_input.h"
#include "lyrics/lyrics_view.h"
#include "spectrum/spectrum_view.h"
#include "screens/player_home.h"
#include "screens/library_view.h"
#include "system/screen_lock_simple.h"

static const char *TAG = "界面";

#if APP_DIAG_BOOT_VERBOSE
#define UI_BOOT_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define UI_BOOT_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif

#if APP_DIAG_UI_PERFORMANCE
#define UI_PERF_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define UI_PERF_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif
static lv_display_t *g_display = nullptr;
static lv_indev_t *g_touch = nullptr;
static esp_err_t g_touch_error = ESP_ERR_INVALID_STATE;
static lv_obj_t *g_boot_root = nullptr;
static lv_obj_t *g_boot_title = nullptr;
static lv_obj_t *g_boot_status = nullptr;
static bool g_bootstrap_ready = false;
static bool g_boot_reveal_pending = false;
static int64_t g_boot_reveal_retry_started_us = 0;
static uint8_t g_boot_reveal_retry_count = 0U;
static lv_timer_t *g_display_visibility_guard_timer = nullptr;
static bool g_ready = false;

// 首次建库进度由扫描任务只写入轻量计数，真正的 LVGL 文本更新由
// taskLVGL 自己的 timer 完成。这样扫描线程不需要等待 LVGL 互斥锁，
// 即使显示正处于 DMA flush，也不会把 TF 扫描主循环一起阻塞。
static lv_timer_t *g_boot_library_progress_timer = nullptr;
static std::atomic<uint32_t> g_boot_library_progress_count{0U};
static std::atomic<uint32_t> g_boot_library_progress_generation{0U};
static uint32_t g_boot_library_progress_applied_generation = 0U;
static std::atomic<bool> g_boot_library_progress_active{false};
// 增量扫描和首次建库共用同一个 LVGL timer，但分别保存状态，避免扫描线程直接抢 LVGL 锁。
static std::atomic<uint32_t> g_boot_library_update_added_count{0U};
static std::atomic<uint32_t> g_boot_library_update_generation{0U};
static uint32_t g_boot_library_update_applied_generation = 0U;
static std::atomic<bool> g_boot_library_update_active{false};
static esp_lv_decoder_handle_t g_image_decoder = nullptr;
static int16_t g_touch_last_x = 0;
static int16_t g_touch_last_y = 0;
static uint32_t g_touch_last_dispatch_sequence = 0U;
static bool g_touch_dispatch_sequence_valid = false;

// LVGL 9.2 在没有 flush_wait_cb 时会在 disp->flushing 上忙等。
// 这里不接管 esp_lvgl_port 已注册的 Panel IO / flush 回调，只监听 LVGL 自己的
// FLUSH_START / FLUSH_FINISH 事件来驱动信号量：既保留官方显示事务闭环，又让
// taskLVGL 在等待 DMA 时阻塞让出 CPU，避免 IDLE1 因纯忙等触发 Task WDT。
// 若 250ms 仍收不到 FLUSH_FINISH，说明官方显示事务没有正常闭环；此时受控重启，
// 不能让 LVGL 继续复用可能仍被 DMA 持有的 draw buffer。
static SemaphoreHandle_t g_display_flush_done = nullptr;
static volatile uint32_t g_display_flush_submit_sequence = 0U;
static volatile uint32_t g_display_flush_done_sequence = 0U;
static uint32_t g_display_flush_wait_timeout_count = 0U;
static lv_area_t g_display_flush_last_area = {};
static constexpr uint32_t kDisplayFlushWaitTimeoutMs = 250U;
// 显示可见性保护只处理“理论上应该可见但状态机没有闭环”的异常。
// 正常首帧/PresentHold 都远短于这些阈值，因此不会干扰正常页面切换。
static constexpr uint32_t kBootRevealRetryIntervalMs = 300U;
static constexpr uint32_t kPresentHoldRecoveryMs = 400U;

// 大面积刷新才等待 TE。
// 小型进度条/按钮局部更新继续立即刷新，避免所有 UI 交互都额外等待一帧。
static bool g_te_sync_pending = false;
static bool g_te_sync_runtime_enabled = false;
static uint8_t g_te_sync_consecutive_timeouts = 0U;
static uint32_t g_te_sync_success_count = 0U;
static uint32_t g_te_sync_timeout_count = 0U;
static uint32_t g_te_sync_timeout_ms = 25U;

// 只有 Artwork 明确请求“另一首封面整帧提交”时才临时关闭面板输出。
// 这不是全局大刷新策略，歌词/频谱/Launcher 不会因此黑屏。
static bool g_present_hold_active = false;
static uint32_t g_present_hold_count = 0U;
static int64_t g_present_hold_started_us = 0;

// 把整屏刷新周期拆成 render / flush / flush-wait，便于定位耗时来源。
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

// 全页面 LVGL 性能审计。
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
// 连续动画不能每帧都支付 0~16.7ms 的 TE 等待；但真正接近整屏的
// 页面切换仍保留 TE，因此用 90% 屏作为“强制同步”门槛。
static constexpr uint32_t kTeSyncForceFullFramePixels =
    (FAKEPOD_LCD_WIDTH * FAKEPOD_LCD_HEIGHT * 9U) / 10U;
static uint32_t g_te_animation_bypass_count = 0U;

// LVGL RGB565 DMA 条带固定为 24 行，为 BoundedSPI 和音频留出更多内部 DMA headroom。
// 双缓冲总像素 RAM = 460 * 24 * 2B * 2 = 44,160B，相比 40 行配置回收 29,440B。
static constexpr uint32_t kLvglDmaBufferLines = 24U;

static void ui_display_flush_sync_event_cb(lv_event_t *event)
{
    if (event == nullptr) return;

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FLUSH_START) {
        // 新事务开始前清掉上一事务可能残留的 token。若 DMA 在 wait_cb 进入前已经
        // 完成，本事务自己的 FLUSH_FINISH 会随后重新 give，因此不会漏掉快速完成。
        if (g_display_flush_done != nullptr) {
            while (xSemaphoreTake(g_display_flush_done, 0) == pdTRUE) {
            }
        }

        ++g_display_flush_submit_sequence;
        // LV_EVENT_FLUSH_START 的参数在不同 LVGL 小版本中并不保证暴露刷新区域；
        // 这里不依赖私有 display 状态，超时诊断以事务序号为主。
        g_display_flush_last_area = {};
        return;
    }

    if (code != LV_EVENT_FLUSH_FINISH) return;

    g_display_flush_done_sequence = g_display_flush_submit_sequence;
    if (g_display_flush_done == nullptr) return;

    // esp_lvgl_port 的颜色完成回调通常从 Panel IO ISR 进入 lv_display_flush_ready()。
    // LVGL 的 FLUSH_FINISH 因此也可能运行在 ISR 上下文，必须选择对应的 FreeRTOS API。
    if (xPortInIsrContext()) {
        BaseType_t task_woken = pdFALSE;
        xSemaphoreGiveFromISR(g_display_flush_done, &task_woken);
        if (task_woken == pdTRUE) portYIELD_FROM_ISR();
    } else {
        xSemaphoreGive(g_display_flush_done);
    }
}

static void ui_display_flush_wait_cb(lv_display_t *display)
{
    (void)display;

    if (g_display_flush_done != nullptr &&
        xSemaphoreTake(g_display_flush_done, pdMS_TO_TICKS(kDisplayFlushWaitTimeoutMs)) == pdTRUE) {
        return;
    }

    ++g_display_flush_wait_timeout_count;
    ESP_LOGE(TAG,
        "LVGL刷屏完成等待超时：%ums submit=%u done=%u area=(%d,%d)-(%d,%d) timeout_count=%u，准备受控重启",
        static_cast<unsigned>(kDisplayFlushWaitTimeoutMs),
        static_cast<unsigned>(g_display_flush_submit_sequence),
        static_cast<unsigned>(g_display_flush_done_sequence),
        g_display_flush_last_area.x1, g_display_flush_last_area.y1,
        g_display_flush_last_area.x2, g_display_flush_last_area.y2,
        static_cast<unsigned>(g_display_flush_wait_timeout_count));

    // 不能简单返回：wait_cb 返回后 LVGL 会继续复用 draw buffer；若底层 DMA 只是
    // 丢了完成闭环而仍在运行，会造成更严重的内存竞争。这里直接受控重启。
    esp_restart();
}

static void ui_display_visibility_guard_timer_cb(lv_timer_t *)
{
    const int64_t now_us = esp_timer_get_time();

    // 首帧揭屏失败时不能把 pending 永久清掉。这里周期性制造一次新的整屏刷新，
    // 让 REFR_READY 再次进入揭屏流程。只重试刷新，不绕过“首帧必须先完成”的约束。
    if (g_boot_reveal_pending && g_display != nullptr &&
        g_boot_reveal_retry_started_us > 0 &&
        now_us - g_boot_reveal_retry_started_us >=
            static_cast<int64_t>(kBootRevealRetryIntervalMs) * 1000LL) {
        g_boot_reveal_retry_started_us = now_us;
        ++g_boot_reveal_retry_count;
        lv_obj_t *screen = lv_screen_active();
        if (screen != nullptr) {
            lv_obj_invalidate(screen);
        }
        if (g_boot_reveal_retry_count <= 5U || (g_boot_reveal_retry_count % 10U) == 0U) {
            ESP_LOGW(TAG, "首帧揭屏仍未完成：retry=%u，重新请求整屏刷新",
                static_cast<unsigned>(g_boot_reveal_retry_count));
        }
    }

    // PresentHold 的目的只是隐藏一次全屏封面替换。若 REFR_READY 因异常没有闭环，
    // 过去会让面板输出永久停在 OFF。超过保护窗口后优先恢复可见性，再让 LVGL
    // 重绘当前屏幕；若用户已主动进入 AOD/熄屏，则绝不能把屏幕强行点亮。
    if (g_present_hold_active && g_present_hold_started_us > 0 &&
        now_us - g_present_hold_started_us >=
            static_cast<int64_t>(kPresentHoldRecoveryMs) * 1000LL) {
        const uint32_t held_ms = static_cast<uint32_t>(
            (now_us - g_present_hold_started_us) / 1000LL);
        const bool normal_power =
            screen_lock_simple_get_power() == ScreenPowerNormal;
        bool restored = true;
        if (normal_power) {
            restored = display_present_set_output(true);
        }
        g_present_hold_active = false;
        g_present_hold_started_us = 0;

        lv_obj_t *screen = lv_screen_active();
        if (screen != nullptr && normal_power) {
            lv_obj_invalidate(screen);
        }
        ESP_LOGW(TAG,
            "PresentHold 超时自愈：held=%ums power=%s restored=%u",
            static_cast<unsigned>(held_ms),
            normal_power ? "Normal" : "NonNormal",
            static_cast<unsigned>(restored));
    }
}

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

    UI_PERF_LOGI(
        "LVGL审计[%s] window=%ums refresh=%u hz=%u.%u total(avg/max)=%u/%uus render=%u/%uus flush=%u/%uus wait=%u/%uus TE=%u/%uus gap=%u/%uus late=%u%s",
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

    UI_PERF_LOGI(
        "LVGL失效[%s] inv(avg/max)=%u.%u/%u次 sum(avg/max)=%u/%upx bbox(avg/max)=%u%%/%u%% flushN(avg/max)=%u.%u/%u DMAfree=%u largest=%u PSRAM=%u",
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

    if (APP_DIAG_UI_PERFORMANCE) {
        ui_perf_note_invalidation(*area);
    }

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
                // 频谱 / Launcher / 歌词缓动 / 惯性滚动的中等面积动画帧
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

    // 所有刷新都记录，供页面级2秒窗口汇总。
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

    // 保留大刷新逐帧诊断，便于分析 render / flush / wait 的耗时分布。
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


// 刷新周期开始时先完成 TE 对齐，再按需暂停面板输出。
// display_present_take_hold_request() 只会被“跨 Track 封面 Source 替换”触发，
// 因此普通大面积页面切换继续只做 TE 同步，不会产生额外黑场。
static void ui_display_refresh_start_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_REFR_START) {
        return;
    }

    const int64_t refresh_started_us = esp_timer_get_time();
    if (APP_DIAG_UI_PERFORMANCE) {
        ui_perf_begin_refresh(refresh_started_us);
    }

    const bool large_refresh = g_te_sync_pending;
    const bool present_requested = display_present_take_hold_request();
    g_te_sync_pending = false;

    if (APP_DIAG_UI_PERFORMANCE && large_refresh) {
        g_large_refresh_profile = {};
        g_large_refresh_profile.active = true;
        g_large_refresh_profile.refr_started_us = esp_timer_get_time();
    }

    if (g_te_sync_runtime_enabled && large_refresh) {
        const int64_t wait_started_us = esp_timer_get_time();
        if (display_te_wait_next(g_te_sync_timeout_ms)) {
            const uint32_t waited_us = static_cast<uint32_t>(
                esp_timer_get_time() - wait_started_us);
            if (APP_DIAG_UI_PERFORMANCE && g_perf_frame.active) g_perf_frame.te_wait_us = waited_us;
            ++g_te_sync_success_count;
            g_te_sync_consecutive_timeouts = 0U;
            if (APP_DIAG_UI_PERFORMANCE &&
                (g_te_sync_success_count <= 3U || (g_te_sync_success_count % 60U) == 0U)) {
                UI_PERF_LOGI(
                    "TE对齐：count=%u wait=%uus",
                    static_cast<unsigned>(g_te_sync_success_count),
                    static_cast<unsigned>(waited_us));
            }
        } else {
            if (APP_DIAG_UI_PERFORMANCE && g_perf_frame.active) {
                g_perf_frame.te_wait_us = static_cast<uint32_t>(
                    esp_timer_get_time() - wait_started_us);
            }
            ++g_te_sync_timeout_count;
            ++g_te_sync_consecutive_timeouts;
            ESP_LOGW(TAG,
                "TE 等待超时：%ums，连续=%u success=%u timeout=%u",
                static_cast<unsigned>(g_te_sync_timeout_ms),
                static_cast<unsigned>(g_te_sync_consecutive_timeouts),
                static_cast<unsigned>(g_te_sync_success_count),
                static_cast<unsigned>(g_te_sync_timeout_count));

            if (g_te_sync_consecutive_timeouts >= kTeSyncDisableAfterTimeouts) {
                g_te_sync_runtime_enabled = false;
                ESP_LOGW(TAG, "TE 运行期自动降级：连续%u次超时，后续刷新不再等待TE",
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
        ESP_LOGW(TAG, "PresentHold 回退：无法暂停面板输出，本轮退回可见刷新");
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

    if (g_boot_reveal_pending) {
        const esp_err_t reveal_ret = display_reveal_after_first_frame();
        if (reveal_ret != ESP_OK) {
            // 不清 pending。下一次刷新完成后继续尝试，避免一次瞬态面板命令失败
            // 就把设备永久留在 brightness=0 / display OFF。
            g_boot_reveal_retry_started_us = esp_timer_get_time();
            ESP_LOGE(TAG, "启动页首帧揭屏失败：%s，将自动重试",
                esp_err_to_name(reveal_ret));
        } else {
            g_boot_reveal_pending = false;
            g_boot_reveal_retry_started_us = 0;
            g_boot_reveal_retry_count = 0U;
            ESP_LOGI(TAG, "启动页首帧已完成并揭屏");
        }
    }

    if (APP_DIAG_UI_PERFORMANCE) {
        ui_perf_finalize_refresh(esp_timer_get_time());
    }

    if (g_large_refresh_profile.active) {
        const uint32_t total_us = static_cast<uint32_t>(
            esp_timer_get_time() - g_large_refresh_profile.refr_started_us);
        ++g_large_refresh_profile_count;
        if (g_large_refresh_profile_count <= 12U || (g_large_refresh_profile_count % 60U) == 0U) {
            UI_PERF_LOGI(
                "LVGL大刷新：count=%u total=%uus render=%uus(%u) flush=%uus(%u) wait=%uus",
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

    // 正常跨 Track 应优先走 BoundedSPI；这里只保留面板输出恢复的兼容回退。
    const bool restored = display_present_set_output(true);
    g_present_hold_active = false;
    g_present_hold_started_us = 0;
    ++g_present_hold_count;

    if (g_present_hold_count <= 8U || (g_present_hold_count % 30U) == 0U || !restored) {
        ESP_LOGI(TAG,
            "PresentHold 回退完成：count=%u hidden=%uus restored=%u",
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

// LVGL 输入回调只消费 TouchInputTask 发布的边沿队列/最新坐标快照。
// DOWN/UP 走边沿队列；生产端做 RELEASE debounce + 满队列背压合并；MOVE 只按最新 snapshot 分发。
static void ui_touch_dispatch_pointer(bool pressed, int16_t x, int16_t y, uint32_t tick_ms)
{
    if (pressed) {
        screen_lock_simple_notify_user_activity();
    }
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

    // ===================== 屏幕动作菜单优先 =====================
    // 打开菜单时：触摸完全由这里处理（不进入 LVGL），
    // 用 press(x,y)/release(x,y) 的行矩形命中（纯坐标）直接执行，
    // 完全绕开 LVGL 对象的 CLICKABLE/CLICKED 事件链（之前因顶层 root 是 CLICKABLE
    // 导致子行永远收不到点击）。
    if (screen_action_menu_is_open()) {
        UiTouchEdgeEvent edge = {};
        const bool got_edge = ui_touch_input_take_edge(&edge);
        UiTouchSnapshot snapshot = {};
        const bool got_snap = !got_edge && ui_touch_input_get_snapshot(&snapshot);

        int x = g_touch_last_x;
        int y = g_touch_last_y;

        if (got_edge) {
            x = edge.x; y = edge.y;
            g_touch_last_x = x; g_touch_last_y = y;
            if (edge.pressed) {
                // DOWN 边沿：按下记录 + 高亮
                (void)screen_action_menu_on_touch_press(x, y);
            } else {
                // UP 边沿：按 press/release 同行规则决定是否执行
                (void)screen_action_menu_on_touch_release(x, y);
            }
        } else if (got_snap) {
            if (snapshot.pressed) {
                x = snapshot.x; y = snapshot.y;
                g_touch_last_x = x; g_touch_last_y = y;
                // MOVE 期间：如果手指在移动，支持上下滑来切换高亮
                screen_action_menu_handle_drag(y);
            }
        } else {
            // 降级同步路径
            CST820Point pt = {};
            if (cst820_read_point(&pt) == ESP_OK && pt.pressed) {
                x = ui_clamp_coord(pt.x, FAKEPOD_LCD_WIDTH - 1);
                y = ui_clamp_coord(pt.y, FAKEPOD_LCD_HEIGHT - 1);
                g_touch_last_x = x; g_touch_last_y = y;
                screen_action_menu_handle_drag(y);
            }
        }

        // 始终吞掉（不再传给 LVGL 分发）
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = g_touch_last_x;
        data->point.y = g_touch_last_y;
        data->continue_reading = false;
        return;
    }

    // ===================== 屏幕锁拦截 =====================
    // Locked 状态：屏显保持，但把所有触摸都报告为 RELEASED，
    // LVGL / gesture_router / 各视图的 ui_touch_dispatch_pointer 都不会响应。
    // ScreenPowerOff 下同样拦截（屏幕全黑，没有任何点击的意义）。
    if (screen_lock_should_block_touch()) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = g_touch_last_x;
        data->point.y = g_touch_last_y;
        data->continue_reading = false;
        // 清空一下边沿队列，避免解锁瞬间“积压事件”触发误操作
        UiTouchEdgeEvent drain = {};
        while (ui_touch_input_take_edge(&drain)) { /* 丢弃 */ }
        // 同时把 gesture_router 也重置，避免残留上次的手势状态
        gesture_router_reset();
        return;
    }

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

static void ui_manager_apply_library_build_progress_locked(uint32_t scanned_count)
{
    if (g_boot_status == nullptr || g_boot_root == nullptr) {
        return;
    }

    char status[96] = {};
    const int written = scanned_count == 0U
        ? snprintf(status, sizeof(status), "正在建立音乐库...")
        : snprintf(
            status,
            sizeof(status),
            "正在建立音乐库...\n已扫描到 %lu 首音乐",
            static_cast<unsigned long>(scanned_count));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(status)) {
        return;
    }

    lv_label_set_text(g_boot_status, status);
    lv_obj_set_style_text_font(g_boot_status, usb_service_font_get(), 0);
    lv_obj_set_style_text_color(g_boot_status, lv_color_hex(0xC7D5E8), 0);
    lv_obj_set_style_text_align(g_boot_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(g_boot_status, 6, 0);
    lv_obj_set_width(g_boot_status, 410);
    lv_obj_align(g_boot_status, LV_ALIGN_CENTER, 0, 18);
    lv_obj_invalidate(g_boot_root);
}

static void ui_manager_apply_library_update_progress_locked(uint32_t added_count)
{
    if (g_boot_status == nullptr || g_boot_root == nullptr) {
        return;
    }

    char status[96] = {};
    const int written = added_count == 0U
        ? snprintf(status, sizeof(status), "正在更新音乐库...")
        : snprintf(
            status,
            sizeof(status),
            "正在更新音乐库...\n新增 %lu 首歌曲",
            static_cast<unsigned long>(added_count));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(status)) {
        return;
    }

    lv_label_set_text(g_boot_status, status);
    lv_obj_set_style_text_font(g_boot_status, usb_service_font_get(), 0);
    lv_obj_set_style_text_color(g_boot_status, lv_color_hex(0xC7D5E8), 0);
    lv_obj_set_style_text_align(g_boot_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(g_boot_status, 6, 0);
    lv_obj_set_width(g_boot_status, 410);
    lv_obj_align(g_boot_status, LV_ALIGN_CENTER, 0, 18);
    lv_obj_invalidate(g_boot_root);
}

static void ui_manager_boot_library_progress_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_boot_status == nullptr || g_boot_root == nullptr) {
        return;
    }

    if (g_boot_library_progress_active.load(std::memory_order_acquire)) {
        const uint32_t generation = g_boot_library_progress_generation.load(std::memory_order_acquire);
        if (generation != g_boot_library_progress_applied_generation) {
            // 先取最新计数再记录 generation。若扫描线程恰好在两次读取之间更新，
            // 下一次 timer 仍会看到 generation 变化，不会永久丢掉一次进度。
            const uint32_t count = g_boot_library_progress_count.load(std::memory_order_relaxed);
            ui_manager_apply_library_build_progress_locked(count);
            g_boot_library_progress_applied_generation = generation;
        }
    }

    if (g_boot_library_update_active.load(std::memory_order_acquire)) {
        const uint32_t generation = g_boot_library_update_generation.load(std::memory_order_acquire);
        if (generation != g_boot_library_update_applied_generation) {
            const uint32_t added_count = g_boot_library_update_added_count.load(std::memory_order_relaxed);
            ui_manager_apply_library_update_progress_locked(added_count);
            g_boot_library_update_applied_generation = generation;
        }
    }
}

esp_err_t ui_manager_bootstrap_init()
{
    if (g_bootstrap_ready) {
        return ESP_OK;
    }

    if (!display_is_ready()) {
        ESP_LOGE(TAG, "显示屏尚未初始化，无法建立 LVGL 启动核心");
        return ESP_ERR_INVALID_STATE;
    }

    UI_BOOT_LOGI("初始化 LVGL 9 启动核心");
    lvgl_port_cfg_t lvgl_cfg = {};
    // Core1 实时优先级阶梯：FLAC 预取固定 P4，LVGL 保持 P3。
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
    UI_BOOT_LOGI("LVGL task：core=1 priority=%u stack=%uB",
        static_cast<unsigned>(lvgl_cfg.task_priority), static_cast<unsigned>(lvgl_cfg.task_stack));
    UI_BOOT_LOGI("Core1 priority：FlacPrefetch=P4 > LVGL=P3 > Artwork=P2 > Cover/Lyrics=P1");

    if (g_display_flush_done == nullptr) {
        g_display_flush_done = xSemaphoreCreateBinary();
        if (g_display_flush_done == nullptr) {
            ESP_LOGE(TAG, "创建 LVGL 刷屏完成信号量失败");
            return ESP_ERR_NO_MEM;
        }
    }

    UI_BOOT_LOGI("注册 CO5300 显示设备");
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

#if APP_DIAG_BOOT_VERBOSE
    const size_t dma_free_before = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t dma_largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#endif
    g_display = lvgl_port_add_disp(&disp_cfg);
    if (g_display == nullptr) {
        ESP_LOGE(TAG, "注册 LVGL 显示设备失败");
        return ESP_FAIL;
    }

    // 不覆盖 esp_lvgl_port 已经安装的 flush_cb / on_color_trans_done。
    // 只给 LVGL 增加等待策略，并通过 FLUSH_START/FINISH 事件观察官方事务是否闭环。
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "安装 LVGL 刷屏等待钩子时获取互斥锁超时");
        return ESP_ERR_TIMEOUT;
    }
    lv_display_add_event_cb(
        g_display, ui_display_flush_sync_event_cb, LV_EVENT_FLUSH_START, nullptr);
    lv_display_add_event_cb(
        g_display, ui_display_flush_sync_event_cb, LV_EVENT_FLUSH_FINISH, nullptr);
    lv_display_set_flush_wait_cb(g_display, ui_display_flush_wait_cb);
    lvgl_port_unlock();

    UI_BOOT_LOGI("LVGL flush：保留esp_lvgl_port官方回调，启用信号量等待 + %ums超时保护",
        static_cast<unsigned>(kDisplayFlushWaitTimeoutMs));

#if APP_DIAG_BOOT_VERBOSE
    const size_t dma_free_after = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t dma_largest_after = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    UI_BOOT_LOGI(
        "LVGL DMA双缓冲：%u行/块 total=%uB DMAfree=%u->%u largest=%u->%u",
        static_cast<unsigned>(kLvglDmaBufferLines),
        static_cast<unsigned>(FAKEPOD_LCD_WIDTH * kLvglDmaBufferLines * 2U * 2U),
        static_cast<unsigned>(dma_free_before),
        static_cast<unsigned>(dma_free_after),
        static_cast<unsigned>(dma_largest_before),
        static_cast<unsigned>(dma_largest_after));
#endif

    // TE 同步只在视频播放（BoundedSPI 路径，自有 wait_for_te 参数）时使用。
    // LVGL 刷新路径默认关闭 TE 等待，避免频谱/主页/歌词等页面每帧额外等待 16-20ms vsync。
    g_te_sync_runtime_enabled = false;
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

    // LVGL task 已经启动，所有直接 lv_* 调用必须使用同一把 port mutex。
    // 启动阶段禁止无限等待；1000ms 内拿不到锁直接报告故障。
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "获取 LVGL 启动核心互斥锁超时");
        return ESP_ERR_TIMEOUT;
    }

    lv_display_add_event_cb(g_display, ui_display_align_area_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_display_add_event_cb(g_display, ui_display_refresh_start_cb, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(g_display, ui_display_refresh_ready_cb, LV_EVENT_REFR_READY, nullptr);
    if (APP_DIAG_UI_PERFORMANCE) {
        lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_RENDER_START, nullptr);
        lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_RENDER_READY, nullptr);
        lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_FLUSH_START, nullptr);
        lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_FLUSH_FINISH, nullptr);
        lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_FLUSH_WAIT_START, nullptr);
        lv_display_add_event_cb(g_display, ui_display_profile_cb, LV_EVENT_FLUSH_WAIT_FINISH, nullptr);
    }

    UI_BOOT_LOGI("CO5300局部刷新偶数对齐已启用");
    if (g_te_sync_runtime_enabled) {
        UI_BOOT_LOGI(
            "TE策略：静态大刷新阈值=%u像素；连续动画中等刷新绕过TE；timeout=%ums period=%uus",
            static_cast<unsigned>(kTeSyncMinPixels),
            static_cast<unsigned>(g_te_sync_timeout_ms),
            static_cast<unsigned>(te_period_us));
    } else {
        ESP_LOGW(TAG, "LVGL TE 同步未启用，保持无 TE 刷新路径");
    }
    UI_BOOT_LOGI("Bounded Surface Present：跨Track走BoundedSPI，Display Hold仅作失败回退");

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    g_boot_root = lv_obj_create(screen);
    if (g_boot_root == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "创建启动页根对象失败");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(g_boot_root);
    lv_obj_set_size(g_boot_root, LV_PCT(100), LV_PCT(100));
    lv_obj_center(g_boot_root);
    lv_obj_set_style_bg_color(g_boot_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_boot_root, LV_OPA_COVER, 0);

    g_boot_title = lv_label_create(g_boot_root);
    lv_label_set_text(g_boot_title, "FakePod");
    lv_obj_set_style_text_color(g_boot_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(g_boot_title, LV_ALIGN_CENTER, 0, -14);

    g_boot_status = lv_label_create(g_boot_root);
    lv_label_set_text(g_boot_status, "Starting...");
    lv_obj_set_style_text_color(g_boot_status, lv_color_hex(0xB0B0B0), 0);
    lv_obj_set_style_text_align(g_boot_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_boot_status, 360);
    lv_obj_align(g_boot_status, LV_ALIGN_CENTER, 0, 18);

    g_boot_library_progress_count.store(0U, std::memory_order_relaxed);
    g_boot_library_progress_generation.store(0U, std::memory_order_relaxed);
    g_boot_library_progress_applied_generation = 0U;
    g_boot_library_progress_active.store(false, std::memory_order_release);
    g_boot_library_update_added_count.store(0U, std::memory_order_relaxed);
    g_boot_library_update_generation.store(0U, std::memory_order_relaxed);
    g_boot_library_update_applied_generation = 0U;
    g_boot_library_update_active.store(false, std::memory_order_release);
    g_boot_library_progress_timer = lv_timer_create(
        ui_manager_boot_library_progress_timer_cb, 100U, nullptr);
    if (g_boot_library_progress_timer == nullptr) {
        ESP_LOGW(TAG, "创建首次建库进度timer失败，将降级为仅显示静态启动状态");
    }

    if (g_display_visibility_guard_timer == nullptr) {
        g_display_visibility_guard_timer = lv_timer_create(
            ui_display_visibility_guard_timer_cb, 100U, nullptr);
        if (g_display_visibility_guard_timer == nullptr) {
            ESP_LOGW(TAG, "创建显示可见性保护timer失败，揭屏/PresentHold将失去超时自愈");
        }
    }

    // 创建对象期间已经产生 invalidation；显式标记整屏，首轮 REFR_READY 才执行物理揭屏。
    g_boot_reveal_pending = true;
    g_boot_reveal_retry_started_us = esp_timer_get_time();
    g_boot_reveal_retry_count = 0U;
    lv_obj_invalidate(screen);
    lvgl_port_unlock();

    g_bootstrap_ready = true;
    ESP_LOGI(TAG, "LVGL 启动核心就绪：黑色启动页等待首帧揭屏");
    return ESP_OK;
}


esp_err_t ui_manager_init()
{
    const int64_t ui_init_started_us = esp_timer_get_time();
    uint32_t ui_touch_ms = 0U;
    uint32_t ui_decoder_ms = 0U;
    uint32_t ui_font_ms = 0U;
    uint32_t ui_pages_ms = 0U;

    if (g_ready) {
        return ESP_OK;
    }

    if (!g_bootstrap_ready || g_display == nullptr) {
        ESP_LOGE(TAG, "LVGL 启动核心尚未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    // 字体加载只访问 TF / Flash 缓存和字体结构，不需要持有 LVGL 锁。
    // 尤其首次建立 Flash 缓存可能包含擦除/写入，放在锁外可避免启动页冻结和显示任务饥饿。
    const int64_t font_started_us = esp_timer_get_time();
    if (sdcard_is_mounted()) {
        const esp_err_t font_ret = font_manager_init();
        if (font_ret != ESP_OK) {
            ESP_LOGW(TAG, "原厂中文字体初始化失败，将使用 LVGL 默认字体：%s", esp_err_to_name(font_ret));
        }
    } else {
        UI_BOOT_LOGI("TF 卡不可用，跳过中文字体加载并使用 LVGL 默认字体");
    }
    ui_font_ms = static_cast<uint32_t>((esp_timer_get_time() - font_started_us) / 1000);

    // 完整 UI 只补齐触摸、图片解码器和业务页面，不重新初始化 LVGL/Display。
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "获取完整 UI 初始化互斥锁超时");
        return ESP_ERR_TIMEOUT;
    }

    const int64_t touch_started_us = esp_timer_get_time();
    gesture_router_reset();
    if (cst820_is_ready()) {
        UI_BOOT_LOGI("注册CST820触摸输入");
        const esp_err_t touch_fast_ret = ui_touch_input_start();
        if (touch_fast_ret != ESP_OK) {
            ESP_LOGW(TAG, "Touch Fast Path 启动失败，将降级为 LVGL 同步读取 CST820：%s",
                esp_err_to_name(touch_fast_ret));
        }

        g_touch = lv_indev_create();
        if (g_touch == nullptr) {
            g_touch_error = ESP_ERR_NO_MEM;
            ESP_LOGW(TAG, "创建 LVGL 触摸输入失败，继续无触摸运行");
        } else {
            g_touch_error = ESP_OK;
            lv_indev_set_type(g_touch, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(g_touch, ui_touch_read_cb);
            lv_indev_set_display(g_touch, g_display);
            lv_indev_set_scroll_limit(g_touch, 8);
            lv_indev_add_event_cb(g_touch, ui_touch_block_scroll_cb, LV_EVENT_SCROLL_BEGIN, nullptr);
            lv_indev_add_event_cb(g_touch, ui_touch_block_scroll_cb, LV_EVENT_SCROLL, nullptr);
        }
    } else {
        g_touch_error = ESP_ERR_INVALID_STATE;
        UI_BOOT_LOGI("CST820 不可用，完整 UI 继续以无触摸模式运行");
    }
    ui_touch_ms = static_cast<uint32_t>((esp_timer_get_time() - touch_started_us) / 1000);

    const int64_t decoder_started_us = esp_timer_get_time();
    // ArtworkLoader 已把压缩图放入 PSRAM，LVGL 只消费内存变量，不再访问 SD。
    const esp_err_t decoder_ret = esp_lv_decoder_init(&g_image_decoder);
    if (decoder_ret != ESP_OK) {
        ESP_LOGW(TAG, "JPEG/PNG 图片解码器初始化失败，首页将使用默认封面：%s", esp_err_to_name(decoder_ret));
        g_image_decoder = nullptr;
    } else {
        lv_image_cache_resize(512U * 1024U, true);
    }
    ui_decoder_ms = static_cast<uint32_t>((esp_timer_get_time() - decoder_started_us) / 1000);

    const int64_t pages_started_us = esp_timer_get_time();
    g_boot_library_progress_active.store(false, std::memory_order_release);
    g_boot_library_update_active.store(false, std::memory_order_release);
    if (g_boot_library_progress_timer != nullptr) {
        lv_timer_delete(g_boot_library_progress_timer);
        g_boot_library_progress_timer = nullptr;
    }
    if (g_boot_root != nullptr) {
        lv_obj_delete(g_boot_root);
        g_boot_root = nullptr;
        g_boot_title = nullptr;
        g_boot_status = nullptr;
    }

    player_home_create(lv_screen_active());
    lyrics_view_create(lv_screen_active());
    spectrum_view_create(lv_screen_active());
    library_view_create(lv_screen_active());

    // AOD / 锁屏悬浮层：挂在 lv_screen_active() 顶层，放在所有业务视图之后。
    // 每次页面 move_foreground 后会再调用 screen_lock_simple_raise（可选），
    // 这里保证至少创建时是最上层。
    {
        const esp_err_t sl_ret = screen_lock_simple_create();
        if (sl_ret != ESP_OK) {
            ESP_LOGW(TAG, "AOD/锁屏悬浮层创建失败：%s", esp_err_to_name(sl_ret));
        }
    }

    // 存储/曲库降级时仍进入正式 LVGL，但用内置 ASCII 字体给出明确状态。
    // 这里不依赖 TF 字体，因此“无卡启动”不会再次被资源文件反向阻塞。
    if (!sdcard_is_mounted() || !media_library_is_ready()) {
        lv_obj_t *status = lv_label_create(lv_screen_active());
        if (status != nullptr) {
            ui_common_lock_object(status);
            lv_label_set_text(status, !sdcard_is_mounted()
                ? "TF card unavailable\nInsert card and restart"
                : "Music library unavailable\nRestart to retry");
            lv_obj_set_style_text_font(status, lv_font_default(), 0);
            lv_obj_set_style_text_color(status, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(status, 360);
            lv_obj_center(status);
        }
    }

    ui_pages_ms = static_cast<uint32_t>((esp_timer_get_time() - pages_started_us) / 1000);

    if (APP_DIAG_UI_PERFORMANCE) {
        g_perf_audit_timer = lv_timer_create(ui_perf_audit_timer_cb, kPerfAuditWindowMs, nullptr);
        if (g_perf_audit_timer == nullptr) {
            ESP_LOGW(TAG, "LVGL性能审计timer创建失败，仅保留逐刷新计数");
        } else {
            UI_PERF_LOGI("LVGL性能审计已启用：window=%ums",
                static_cast<unsigned>(kPerfAuditWindowMs));
        }
    }
    lvgl_port_unlock();

    g_ready = true;
    ESP_LOGI(TAG, "完整UI初始化耗时：总计=%u ms Touch=%u Decoder=%u Font=%u Pages=%u",
        static_cast<unsigned>((esp_timer_get_time() - ui_init_started_us) / 1000),
        static_cast<unsigned>(ui_touch_ms),
        static_cast<unsigned>(ui_decoder_ms),
        static_cast<unsigned>(ui_font_ms),
        static_cast<unsigned>(ui_pages_ms));
    ESP_LOGI(
        TAG,
        "UI ready：启动核心复用，Touch=%s Storage=%s Library=%s；后台服务等待系统 READY",
        g_touch != nullptr ? "READY" : "DISABLED",
        sdcard_is_mounted() ? "READY" : "UNAVAILABLE",
        media_library_is_ready() ? "READY" : "UNAVAILABLE"
    );
    return ESP_OK;
}

bool ui_manager_is_ready()
{
    return g_ready;
}

bool ui_manager_show_library_build_progress(uint32_t scanned_count)
{
    if (!g_bootstrap_ready || g_display == nullptr || g_boot_root == nullptr ||
        g_boot_status == nullptr) {
        return false;
    }

    // 这里只发布轻量状态，不从建库扫描线程进入 LVGL。
    // 具体文本刷新由 taskLVGL 的 100ms timer 完成，避免扫描线程等待显示互斥锁。
    g_boot_library_progress_count.store(scanned_count, std::memory_order_relaxed);
    g_boot_library_progress_active.store(true, std::memory_order_release);
    g_boot_library_progress_generation.fetch_add(1U, std::memory_order_release);
    return true;
}

bool ui_manager_show_library_build_complete(uint32_t total_count)
{
    if (!g_bootstrap_ready || g_display == nullptr || g_boot_root == nullptr ||
        g_boot_status == nullptr) {
        return false;
    }

    // 完成状态优先级高于异步进度，先停掉 timer 的进度提交，
    // 防止最后一次“已扫描到 X 首”在完成文案之后又覆盖回来。
    g_boot_library_progress_active.store(false, std::memory_order_release);

    char status[96] = {};
    const int written = snprintf(
        status,
        sizeof(status),
        "音乐库建立完成\n共 %lu 首歌曲",
        static_cast<unsigned long>(total_count));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(status)) {
        return false;
    }

    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "曲库建立完成提示获取LVGL互斥锁超时");
        return false;
    }

    lv_label_set_text(g_boot_status, status);
    lv_obj_set_style_text_font(g_boot_status, usb_service_font_get(), 0);
    lv_obj_set_style_text_color(g_boot_status, lv_color_hex(0xA9D6B4), 0);
    lv_obj_set_style_text_align(g_boot_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(g_boot_status, 5, 0);
    lv_obj_set_width(g_boot_status, 420);
    lv_obj_align(g_boot_status, LV_ALIGN_CENTER, 0, 24);
    lv_obj_invalidate(g_boot_root);
    lvgl_port_unlock();
    return true;
}

bool ui_manager_show_library_update_progress(uint32_t added_count)
{
    if (!g_bootstrap_ready || g_display == nullptr || g_boot_root == nullptr ||
        g_boot_status == nullptr) {
        return false;
    }

    // 增量扫描线程只发布新增数量，不直接进入 LVGL。
    // 读卡器加歌后的开机扫描和 USB MSC 归还后的扫描都可以复用同一事件语义。
    g_boot_library_update_added_count.store(added_count, std::memory_order_relaxed);
    g_boot_library_update_active.store(true, std::memory_order_release);
    g_boot_library_update_generation.fetch_add(1U, std::memory_order_release);
    return true;
}

bool ui_manager_show_library_update_complete(
    uint32_t added_count,
    uint32_t removed_count,
    uint32_t updated_count)
{
    if (!g_bootstrap_ready || g_display == nullptr || g_boot_root == nullptr ||
        g_boot_status == nullptr) {
        return false;
    }

    g_boot_library_update_active.store(false, std::memory_order_release);

    char status[160] = {};
    size_t used = static_cast<size_t>(
        snprintf(status, sizeof(status), "音乐库已更新"));
    const auto append_count = [&](const char *label, uint32_t count) {
        if (count == 0U || used >= sizeof(status) - 1U) {
            return;
        }

        char line[48] = {};
        const int written = snprintf(
            line,
            sizeof(line),
            "\n%s %lu 首歌曲",
            label,
            static_cast<unsigned long>(count));
        if (written <= 0) {
            return;
        }

        const size_t line_len = static_cast<size_t>(written);
        const size_t remaining = sizeof(status) - 1U - used;
        if (line_len > remaining || line_len >= sizeof(line)) {
            return;
        }
        memcpy(status + used, line, line_len);
        used += line_len;
        status[used] = '\0';
    };
    append_count("新增", added_count);
    append_count("删除", removed_count);
    append_count("更新", updated_count);

    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "曲库更新完成提示获取LVGL互斥锁超时");
        return false;
    }

    lv_label_set_text(g_boot_status, status);
    lv_obj_set_style_text_font(g_boot_status, usb_service_font_get(), 0);
    lv_obj_set_style_text_color(g_boot_status, lv_color_hex(0xA9D6B4), 0);
    lv_obj_set_style_text_align(g_boot_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(g_boot_status, 5, 0);
    lv_obj_set_width(g_boot_status, 420);
    lv_obj_align(g_boot_status, LV_ALIGN_CENTER, 0, 24);
    lv_obj_invalidate(g_boot_root);
    lvgl_port_unlock();
    return true;
}

bool ui_manager_show_usb_storage_service()
{
    if (!g_bootstrap_ready || g_display == nullptr || g_boot_root == nullptr ||
        g_boot_title == nullptr || g_boot_status == nullptr) {
        return false;
    }

    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "USB服务提示页获取LVGL互斥锁超时");
        return false;
    }

    // 服务模式不会挂载 TF 卡，因此使用编译进 Flash 的精简中文字体。
    // ASCII 的 USB 由该字体 fallback 到 LVGL 内置字体，汉字本身完全不依赖 /sdcard/FONTS。
    const lv_font_t *service_font = usb_service_font_get();
    lv_label_set_text(g_boot_title, "USB 磁盘模式");
    lv_obj_set_style_text_font(g_boot_title, service_font, 0);
    lv_obj_set_style_text_color(g_boot_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(g_boot_title, LV_ALIGN_CENTER, 0, -62);

    lv_label_set_text(
        g_boot_status,
        "可在电脑上访问存储卡\n"
        "请先安全弹出后再关机\n"
        "下次开机恢复串口模式");
    lv_obj_set_style_text_font(g_boot_status, service_font, 0);
    lv_obj_set_style_text_color(g_boot_status, lv_color_hex(0xC7D5E8), 0);
    lv_obj_set_style_text_align(g_boot_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(g_boot_status, 8, 0);
    lv_obj_set_width(g_boot_status, 410);
    lv_obj_align(g_boot_status, LV_ALIGN_CENTER, 0, 24);
    lv_obj_invalidate(g_boot_root);
    lvgl_port_unlock();
    return true;
}

bool ui_manager_show_boot_fatal(const char *reason, esp_err_t error)
{
    if (!g_bootstrap_ready || g_display == nullptr || g_boot_root == nullptr ||
        g_boot_title == nullptr || g_boot_status == nullptr) {
        return false;
    }

    // 这里只复用已经建立并经过首帧验证的 LVGL 启动核心，不创建第二条显示路径。
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "致命错误页获取 LVGL 互斥锁超时，仅保留串口错误");
        return false;
    }

    lv_label_set_text(g_boot_title, "FakePod");
    lv_label_set_text_fmt(
        g_boot_status,
        "Startup failed\n%s\n%s",
        reason != nullptr ? reason : "Core startup failure",
        esp_err_to_name(error)
    );
    lv_obj_set_style_text_color(g_boot_status, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(g_boot_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_boot_status, 360);
    lv_obj_align(g_boot_status, LV_ALIGN_CENTER, 0, 22);
    lv_obj_invalidate(g_boot_root);
    lvgl_port_unlock();
    return true;
}

bool ui_manager_touch_available()
{
    return g_touch != nullptr;
}

esp_err_t ui_manager_touch_error()
{
    return ui_manager_touch_available() ? ESP_OK : g_touch_error;
}
