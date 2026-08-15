#include "ebook_app.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "audio/audio_service.h"
#include "audio/decoders/flac_decoder.h"
#include "ebook_bookmark_store.h"
#include "ebook_page_index_cache.h"
#include "ebook_reader_model.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font/font_manager.h"
#include "gesture/gesture_router.h"
#include "lvgl.h"
#include "player/player_control.h"
#include "app_launcher_overlay.h"
#include "sdcard.h"
#include "ui/input/touch_input.h"
#include "ui_common.h"

static const char *TAG = "EbookAPP";

namespace {

enum class EbookPage : uint8_t
{
    Browser = 0,
    Reader,
};

enum class ReaderViewMode : uint8_t
{
    Fullscreen = 0,
    Labeled,
};

static constexpr int32_t kHeaderHeight = 68;
static constexpr int32_t kContentMargin = 12;
static constexpr int32_t kRowHeight = 58;
static constexpr int16_t kReaderTapMaxMovePx = 18;
// 全屏沿用上一版“大面积轻点”思路，但从左右改为上下：顶部/底部各120px，中央呼出标签视图。
static constexpr int16_t kReaderEdgeTapHeightPx = 120;
static constexpr int16_t kBrowserBackTouchExpandPx = 10;
static constexpr int16_t kReaderTopTouchExpandPx = 5;
static constexpr int16_t kReaderPageTouchExpandPx = 7;
static constexpr int32_t kReaderMarginX = 12;
static constexpr int32_t kReaderFullscreenTextY = 10;
static constexpr int32_t kReaderFullscreenTextHeight = 440;
// 带标签视图移除书名行，只保留顶部标签按钮和底部页码/翻页区。
// 正文从 y=70 开始，可比上一版多显示约1行，同时仍与顶部扩展热区/底部控件分离。
static constexpr int32_t kReaderLabeledTextY = 70;
// 带标签视图把“分页可用高度”和“LVGL 实际渲染高度”分离。
// 分页仍按 320px 计算约 9 行；Label 额外给 10px 底部绘制余量，
// 避免字体 glyph/anti-alias 边缘刚好落在对象 clip 边界而裁掉最后一行。
static constexpr int32_t kReaderLabeledLayoutHeight = 320;
static constexpr int32_t kReaderLabeledRenderHeight = 330;
// 实机字体在 320px 布局高度下可能动态算出10行，但第10行字形会碰到底部裁剪边界。
// 带标签视图固定最多9个完整可见行；额外文本留到下一页，避免显示/next_offset不一致。
static constexpr int32_t kReaderLabeledMaxLines = 9;
static constexpr int32_t kReaderLineSpace = 3;
static constexpr size_t kInitialPageIndexCapacity = 64;
static constexpr uint32_t kPageIndexBuildPeriodMs = 20;
static constexpr uint8_t kPageIndexBuildMaxPagesPerTick = 4;
static constexpr int64_t kPageIndexBuildBudgetUs = 3500;
// 非当前视图的影子索引只在用户空闲后低速追赶当前阅读 anchor。
// 每次最多扫描1页，并限制最短步进间隔，避免与 AudioTask 持续争抢 TF。
static constexpr uint32_t kPageIndexShadowIdleMs = 300;
static constexpr uint32_t kPageIndexShadowStepIntervalMs = 40;
static constexpr uint8_t kPageIndexShadowMaxPagesPerStep = 1;

// Browser V1 Final 音频 QoS：目录 FAT metadata 扫描与大批 LVGL Row 创建都属于可延后的工作。
// 有后台 SD-FLAC 时只在 ring >=90% 的安全窗口推进，并把一次性工作拆成小 batch。
static constexpr uint32_t kEbookBackgroundIoFlacSafePercent = 90;
static constexpr size_t kBrowserScanBatchNoFlac = 8;
static constexpr size_t kBrowserScanBatchWithFlac = 1;
static constexpr uint32_t kBrowserWaitLogIntervalMs = 1000;
// Virtual Browser：Directory Index 可按 PSRAM 动态扩展，但屏幕只复用5个固定 Row。
// 普通纵向 Flick 每次移动4项，保留1行上下文重叠；不再使用 LVGL native pixel scroll。
static constexpr size_t kBrowserVisibleRows = 5;
static constexpr size_t kBrowserGestureStepRows = 4;
static constexpr int32_t kBrowserRowGap = 8;
// PageIndex cache 布局签名世代。字体/正文几何等只要会改变页界就必须递增。
static constexpr uint32_t kPageIndexLayoutRevision = 1U;

static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_header_back = nullptr;
static lv_obj_t *g_header_title = nullptr;
static lv_obj_t *g_header_line = nullptr;
static lv_obj_t *g_browser_host = nullptr;
static lv_obj_t *g_browser_status = nullptr;
static lv_obj_t *g_browser_position = nullptr;
struct BrowserRowUi
{
    lv_obj_t *row = nullptr;
    lv_obj_t *name = nullptr;
    lv_obj_t *kind = nullptr;
};
static BrowserRowUi g_browser_rows[kBrowserVisibleRows] = {};
static size_t g_browser_first_index = 0;
static lv_obj_t *g_reader_host = nullptr;
static lv_obj_t *g_reader_text = nullptr;
static lv_obj_t *g_reader_overlay = nullptr;
static lv_obj_t *g_reader_page_info = nullptr;
static lv_obj_t *g_reader_prev = nullptr;
static lv_obj_t *g_reader_next = nullptr;
static lv_obj_t *g_reader_bookmark = nullptr;
static lv_obj_t *g_reader_bookmark_label = nullptr;
static lv_obj_t *g_reader_view = nullptr;
static lv_obj_t *g_reader_view_label = nullptr;
static lv_obj_t *g_reader_music = nullptr;
static lv_obj_t *g_reader_music_label = nullptr;

static lv_timer_t *g_gesture_timer = nullptr;
static lv_timer_t *g_page_index_timer = nullptr;

static bool g_press_tracking = false;
static lv_point_t g_press_start = {};
static EbookPage g_page = EbookPage::Browser;
static char *g_current_dir = nullptr; // PSRAM
static char *g_scratch_path = nullptr; // PSRAM
static char *g_book_path = nullptr; // PSRAM
static EbookReader::DirectorySnapshot g_directory = {};

enum class BrowserLoadPhase : uint8_t
{
    Idle = 0,
    WaitingForAudioWindow,
    Scanning,
};

struct BrowserLoadState
{
    BrowserLoadPhase phase = BrowserLoadPhase::Idle;
    EbookReader::DirectoryScanSession scan = {};
    uint32_t last_wait_log_ms = 0;
};
static BrowserLoadState g_browser_load = {};

// 两种视图各自维护一套“从书首开始”的规范页边界索引。
// 书签只保存内容 byte offset；切换视图时在目标索引中定位“包含该 offset 的完整页”，
// 不再制造截断半页，因此上一页/下一页始终是该视图自己的完整自然页。
struct ReaderPageIndex
{
    uint64_t *starts = nullptr; // PSRAM，每项8B
    size_t count = 0;
    size_t capacity = 0;
    size_t persisted_count = 0; // 已提交到 TF cache 的完整前缀页数
};
static ReaderPageIndex g_page_indexes[2] = {};
static size_t g_current_page_ordinal = 0;

enum class PageIndexBuildPurpose : uint8_t
{
    None = 0,
    OpenBookmark,
    SwitchView,
};

struct PageIndexBuilderState
{
    bool active = false;
    PageIndexBuildPurpose purpose = PageIndexBuildPurpose::None;
    ReaderViewMode mode = ReaderViewMode::Labeled;
    uint64_t anchor = 0;
    size_t ordinal = 0;
    ReaderViewMode previous_mode = ReaderViewMode::Labeled;
    size_t previous_ordinal = 0;
    uint64_t previous_anchor = 0;
    EbookReader::PageScanSession scan = {};
};
static PageIndexBuilderState g_page_index_builder = {};

struct PageIndexShadowState
{
    bool pending = false;
    bool active = false;
    ReaderViewMode mode = ReaderViewMode::Fullscreen;
    uint64_t anchor = 0;
    size_t ordinal = 0;
    uint32_t last_step_tick_ms = 0;
    EbookReader::PageScanSession scan = {};
};
static PageIndexShadowState g_page_index_shadow = {};

// 真实阅读锚点与当前显示页起点分离。跨视图重排时显示页可以从锚点之前开始，
// 但只要用户没有真正翻页，自动书签仍保存这个内容锚点。
static uint64_t g_read_anchor_offset = 0;
static uint64_t g_page_start = 0;
static uint64_t g_page_next = 0;
static uint64_t g_book_size = 0;
static bool g_page_at_end = false;
static bool g_reader_overlay_visible = false;
static ReaderViewMode g_reader_view_mode = ReaderViewMode::Labeled;
static bool g_reader_page_loaded = false;
static bool g_reader_position_dirty = false;
static bool g_bookmark_valid = false;
static uint64_t g_bookmark_offset = 0;

static bool ebook_flac_storage_window_safe(bool *out_competing = nullptr, uint32_t *out_percent = nullptr)
{
    if (out_competing != nullptr) *out_competing = false;
    if (out_percent != nullptr) *out_percent = 100U;

    FlacStorageWindowSnapshot window = {};
    if (!flac_decoder_get_storage_window(&window) || !window.active ||
        !window.storage_competes || window.capacity_bytes == 0U) {
        return true;
    }

    const uint32_t percent = static_cast<uint32_t>(
        (static_cast<uint64_t>(window.buffered_bytes) * 100ULL +
         static_cast<uint64_t>(window.capacity_bytes) / 2ULL) /
        static_cast<uint64_t>(window.capacity_bytes));
    if (out_competing != nullptr) *out_competing = true;
    if (out_percent != nullptr) *out_percent = percent;
    return percent >= kEbookBackgroundIoFlacSafePercent;
}


static const char *ebook_basename(const char *path)
{
    if (path == nullptr || path[0] == '\0') return "电子书";
    const char *slash = strrchr(path, '/');
    return (slash != nullptr && slash[1] != '\0') ? slash + 1 : path;
}

static void ebook_expand_click_area(lv_obj_t *obj, int32_t pixels)
{
    if (obj == nullptr || pixels <= 0) return;
    lv_obj_set_ext_click_area(obj, pixels);
}

static lv_obj_t *ebook_make_label(
    lv_obj_t *parent,
    const char *text,
    uint32_t rgb,
    lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == nullptr) return nullptr;
    ui_common_lock_object(label);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_font(label, font_manager_get_ui_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static void ebook_go_back();
static void ebook_browser_load_tick();
static void ebook_cancel_browser_load();
static void ebook_browser_shift_window(int direction);
static void ebook_update_browser_virtual_rows();

static void ebook_show_app_launcher()
{
    if (g_root == nullptr || g_page != EbookPage::Browser ||
        g_current_dir == nullptr || strcmp(g_current_dir, EbookReader::kRootDirectory) != 0) {
        return;
    }
    const esp_err_t ret = app_launcher_overlay_show(g_root, AppId::Ebook);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ebook圆环Launcher展开失败：%s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Ebook主页压暗并展开公共圆环Launcher：Ebook扇区点亮");
    }
}

static void ebook_gesture_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_root == nullptr || app_manager_foreground() != AppId::Ebook) {
        return;
    }

    // Browser 的目录扫描复用这颗 20ms LVGL timer，以 cooperative slice 方式推进。
    // 屏幕 Row 已固定为5个虚拟槽位，不再在目录加载阶段批量创建 LVGL 对象。
    ebook_browser_load_tick();

    // Reader 只使用自己的上/中/下轻点分页。不要在这里抢先 take 全局动作；
    // 返回 Browser 时会 reset Router，避免 Reader 期间的动作残留到目录页。
    if (g_page != EbookPage::Browser && !app_launcher_overlay_is_visible()) {
        return;
    }

    UiGestureAction action = UiGestureAction::None;
    if (!gesture_router_take_action(&action)) {
        return;
    }

    // 与 Music Launcher 一致：圆环已经显示时，新的页面级手势只负责先收起圆环。
    if (app_launcher_overlay_is_visible()) {
        app_launcher_overlay_hide();
        return;
    }

    switch (action) {
        case UiGestureAction::SwipeRight:
            ESP_LOGI(TAG, "Ebook目录手势：%s -> 返回一级",
                gesture_router_action_name(action));
            ebook_go_back();
            break;
        case UiGestureAction::SwipeUpTrack:
            ebook_browser_shift_window(+1);
            break;
        case UiGestureAction::SwipeDownTrack:
            ebook_browser_shift_window(-1);
            break;
        case UiGestureAction::PullUpFromBottom:
            if (g_current_dir != nullptr &&
                strcmp(g_current_dir, EbookReader::kRootDirectory) == 0) {
                ESP_LOGI(TAG, "Ebook主页手势：%s -> 公共圆环Launcher",
                    gesture_router_action_name(action));
                ebook_show_app_launcher();
            }
            break;
        default:
            break;
    }
}


static void ebook_set_object_visible(lv_obj_t *obj, bool visible)
{
    if (obj == nullptr) return;
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void ebook_set_browser_chrome_visible(bool visible)
{
    ebook_set_object_visible(g_header_back, visible);
    ebook_set_object_visible(g_header_title, visible);
    ebook_set_object_visible(g_header_line, visible);
}

static void ebook_update_music_control()
{
    if (g_reader_music_label == nullptr || g_reader_music == nullptr) return;
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot) || !snapshot.ready) {
        lv_label_set_text(g_reader_music_label, "音乐不可用");
        lv_obj_set_style_opa(g_reader_music, LV_OPA_40, 0);
        return;
    }
    lv_obj_set_style_opa(g_reader_music, LV_OPA_COVER, 0);
    if (snapshot.state == AudioPlaybackState::Playing ||
        snapshot.state == AudioPlaybackState::Seeking) {
        lv_label_set_text(g_reader_music_label, "暂停音乐");
    } else if (snapshot.state == AudioPlaybackState::Paused) {
        lv_label_set_text(g_reader_music_label, "继续音乐");
    } else {
        lv_label_set_text(g_reader_music_label, "播放音乐");
    }
}

static void ebook_update_bookmark_control()
{
    if (g_reader_bookmark_label == nullptr || g_reader_bookmark == nullptr) return;
    const bool current_is_saved = g_bookmark_valid &&
        g_bookmark_offset == g_read_anchor_offset && !g_reader_position_dirty;
    lv_label_set_text(g_reader_bookmark_label, current_is_saved ? "已保存" : "存书签");
    lv_obj_set_style_bg_color(g_reader_bookmark,
        lv_color_hex(current_is_saved ? 0x6B5524 : 0x151B24), 0);
}

static void ebook_update_reader_view_control()
{
    if (g_reader_view_label != nullptr) {
        lv_label_set_text(g_reader_view_label, "全屏");
    }
}

static int32_t ebook_reader_text_y_for_mode(ReaderViewMode mode)
{
    return mode == ReaderViewMode::Labeled
        ? kReaderLabeledTextY
        : kReaderFullscreenTextY;
}

static int32_t ebook_reader_text_layout_height_for_mode(ReaderViewMode mode)
{
    return mode == ReaderViewMode::Labeled
        ? kReaderLabeledLayoutHeight
        : kReaderFullscreenTextHeight;
}

static int32_t ebook_reader_text_render_height_for_mode(ReaderViewMode mode)
{
    return mode == ReaderViewMode::Labeled
        ? kReaderLabeledRenderHeight
        : kReaderFullscreenTextHeight;
}

static void ebook_apply_reader_text_geometry(ReaderViewMode mode)
{
    if (g_reader_text == nullptr) return;
    lv_obj_set_pos(g_reader_text, kReaderMarginX, ebook_reader_text_y_for_mode(mode));
    lv_obj_set_size(
        g_reader_text,
        460 - kReaderMarginX * 2,
        ebook_reader_text_render_height_for_mode(mode));
}

static EbookReader::PageLayout ebook_reader_page_layout_for_mode(ReaderViewMode mode);
static bool ebook_find_anchor_in_known_index(
    ReaderViewMode mode, uint64_t anchor, size_t *out_ordinal);
static esp_err_t ebook_load_indexed_page(
    ReaderViewMode mode, size_t ordinal, uint64_t semantic_anchor);
static void ebook_release_page_indexes();
static void ebook_restore_page_index_cache(ReaderViewMode mode);
static void ebook_flush_page_index_caches(bool force);
static void ebook_update_reader_controls();
static void ebook_cancel_page_index_builder();
static void ebook_cancel_shadow_warmup();
static void ebook_schedule_shadow_warmup();
static esp_err_t ebook_start_page_index_builder(
    ReaderViewMode mode, uint64_t anchor, PageIndexBuildPurpose purpose,
    ReaderViewMode previous_mode, size_t previous_ordinal, uint64_t previous_anchor);

static void ebook_apply_reader_view_chrome(ReaderViewMode mode)
{
    g_reader_overlay_visible = mode == ReaderViewMode::Labeled;
    ebook_set_object_visible(g_reader_overlay, g_reader_overlay_visible);
    ebook_apply_reader_text_geometry(mode);
    if (g_reader_overlay_visible) {
        ebook_update_music_control();
        ebook_update_bookmark_control();
        ebook_update_reader_view_control();
        if (g_reader_overlay != nullptr) lv_obj_move_foreground(g_reader_overlay);
    }
}

static void ebook_set_reader_view_mode(ReaderViewMode mode)
{
    if (g_page != EbookPage::Reader) mode = ReaderViewMode::Fullscreen;

    const ReaderViewMode previous_mode = g_reader_view_mode;
    if (mode == previous_mode) {
        ebook_apply_reader_view_chrome(mode);
        return;
    }
    if (g_page_index_builder.active) return;
    // 用户主动切视图优先级高于后台影子索引。关闭 Shadow FILE/scratch，
    // 但已写入目标 PageIndex 的自然页边界全部保留，可直接被本次切换复用。
    ebook_cancel_shadow_warmup();

    // Reader 尚未加载正文时（例如刚进入），只切换 chrome，不触发索引定位。
    if (g_page != EbookPage::Reader || !g_reader_page_loaded) {
        g_reader_view_mode = mode;
        ebook_apply_reader_view_chrome(mode);
        return;
    }

    const uint64_t anchor = g_read_anchor_offset;
    const size_t previous_ordinal = g_current_page_ordinal;
    const uint64_t previous_anchor = g_read_anchor_offset;
    size_t target_ordinal = 0;

    // Shadow Index 会在空闲窗口预热另一视图；若用户先主动切换，仍按需恢复其 cache，
    // 并优先复用 Shadow 已经建立的连续页界。
    ebook_restore_page_index_cache(mode);
    if (ebook_find_anchor_in_known_index(mode, anchor, &target_ordinal)) {
        g_reader_view_mode = mode;
        ebook_apply_reader_view_chrome(mode);
        const esp_err_t ret = ebook_load_indexed_page(mode, target_ordinal, anchor);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                "Reader视图重排完成：mode=%s anchor=%llu page=%u start=%llu next=%llu",
                mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
                static_cast<unsigned long long>(anchor),
                static_cast<unsigned>(g_current_page_ordinal + 1U),
                static_cast<unsigned long long>(g_page_start),
                static_cast<unsigned long long>(g_page_next));
            ebook_schedule_shadow_warmup();
            return;
        }

        g_reader_view_mode = previous_mode;
        ebook_apply_reader_view_chrome(previous_mode);
        const esp_err_t restore_ret = ebook_load_indexed_page(
            previous_mode, previous_ordinal, previous_anchor);
        if (restore_ret != ESP_OK) {
            ESP_LOGE(TAG, "Reader恢复原视图分页失败：%s", esp_err_to_name(restore_ret));
        }
        return;
    }

    // 目标视图索引尚未扩展到阅读锚点：保持当前完整页面和控制栏不变，
    // 后台增量建立目标索引。成功后再一次性切换 chrome + 正文，定位期间仍可返回/控音乐。
    const esp_err_t start_ret = ebook_start_page_index_builder(
        mode, anchor, PageIndexBuildPurpose::SwitchView,
        previous_mode, previous_ordinal, previous_anchor);
    if (start_ret == ESP_OK) {
        // 若从全屏返回带标签需要较长时间，临时露出控制层但不改变正文几何，
        // 让用户在 Builder 工作期间仍可“返回”取消或控制后台音乐。
        if (previous_mode == ReaderViewMode::Fullscreen && g_reader_overlay != nullptr) {
            g_reader_overlay_visible = true;
            ebook_set_object_visible(g_reader_overlay, true);
            ebook_update_music_control();
            lv_obj_move_foreground(g_reader_overlay);
        }
        ebook_update_reader_controls();
        ESP_LOGI(TAG, "Reader目标视图增量重排启动：from=%s to=%s anchor=%llu",
            previous_mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
            mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
            static_cast<unsigned long long>(anchor));
        return;
    }

    ESP_LOGW(TAG, "Reader异步重排启动失败：mode=%s ret=%s",
        mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
        esp_err_to_name(start_ret));
}

static void ebook_set_reader_overlay_visible(bool visible)
{
    ebook_set_reader_view_mode(visible ? ReaderViewMode::Labeled : ReaderViewMode::Fullscreen);
}

static esp_err_t ebook_save_current_position(const char *reason, bool force)
{
    if (g_page != EbookPage::Reader || !g_reader_page_loaded ||
        g_book_path == nullptr || g_book_path[0] == '\0') {
        return ESP_OK;
    }
    if (!force && !g_reader_position_dirty) return ESP_OK;

    const esp_err_t ret = EbookBookmarkStore::save(g_book_path, g_read_anchor_offset);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TXT阅读位置保存失败：reason=%s offset=%llu ret=%s",
            reason != nullptr ? reason : "unknown",
            static_cast<unsigned long long>(g_read_anchor_offset),
            esp_err_to_name(ret));
        return ret;
    }

    g_bookmark_valid = true;
    g_bookmark_offset = g_read_anchor_offset;
    g_reader_position_dirty = false;
    ebook_update_bookmark_control();
    ESP_LOGI(TAG, "TXT阅读位置已保存：reason=%s offset=%llu",
        reason != nullptr ? reason : "unknown",
        static_cast<unsigned long long>(g_read_anchor_offset));
    return ESP_OK;
}

static void ebook_set_status(const char *text)
{
    if (g_browser_status != nullptr) {
        lv_label_set_text(g_browser_status, text != nullptr ? text : "");
    }
}

static void ebook_update_header()
{
    if (g_header_title == nullptr) return;
    if (g_page == EbookPage::Reader) {
        lv_label_set_text(g_header_title, g_book_path != nullptr ? ebook_basename(g_book_path) : "TXT阅读");
    } else if (g_current_dir != nullptr && strcmp(g_current_dir, EbookReader::kRootDirectory) != 0) {
        lv_label_set_text(g_header_title, ebook_basename(g_current_dir));
    } else {
        lv_label_set_text(g_header_title, "电子书");
    }
}

static size_t ebook_view_slot(ReaderViewMode mode)
{
    return mode == ReaderViewMode::Labeled ? 1U : 0U;
}

static ReaderPageIndex *ebook_page_index_for_mode(ReaderViewMode mode)
{
    return &g_page_indexes[ebook_view_slot(mode)];
}

static void ebook_release_page_index(ReaderPageIndex *index)
{
    if (index == nullptr) return;
    if (index->starts != nullptr) heap_caps_free(index->starts);
    *index = {};
}

static void ebook_release_page_indexes()
{
    ebook_release_page_index(&g_page_indexes[0]);
    ebook_release_page_index(&g_page_indexes[1]);
    g_current_page_ordinal = 0;
}

static bool ebook_page_index_push(ReaderPageIndex *index, uint64_t offset)
{
    if (index == nullptr) return false;
    if (index->count > 0 && index->starts[index->count - 1U] == offset) return true;
    if (index->count >= index->capacity) {
        const size_t next_capacity = index->capacity == 0
            ? kInitialPageIndexCapacity
            : index->capacity * 2U;
        uint64_t *next = static_cast<uint64_t *>(heap_caps_malloc(
            next_capacity * sizeof(uint64_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (next == nullptr) return false;
        if (index->starts != nullptr && index->count > 0) {
            memcpy(next, index->starts, index->count * sizeof(uint64_t));
            heap_caps_free(index->starts);
        }
        index->starts = next;
        index->capacity = next_capacity;
    }
    index->starts[index->count++] = offset;
    return true;
}

static void ebook_reset_reader_session()
{
    ebook_cancel_page_index_builder();
    ebook_cancel_shadow_warmup();
    ebook_cancel_browser_load();
    ebook_flush_page_index_caches(true);
    ebook_release_page_indexes();
    g_read_anchor_offset = 0;
    g_page_start = 0;
    g_page_next = 0;
    g_book_size = 0;
    g_page_at_end = false;
    g_reader_page_loaded = false;
    g_reader_position_dirty = false;
    g_bookmark_valid = false;
    g_bookmark_offset = 0;
    g_reader_overlay_visible = false;
    g_reader_view_mode = ReaderViewMode::Labeled;
    if (g_book_path != nullptr) g_book_path[0] = '\0';
}

static void ebook_show_browser()
{
    gesture_router_reset();
    g_press_tracking = false;
    if (g_page == EbookPage::Reader) {
        ebook_save_current_position("退出TXT", false);
    }
    g_page = EbookPage::Browser;
    ebook_reset_reader_session();
    ebook_set_reader_overlay_visible(false);
    ebook_set_browser_chrome_visible(true);
    if (g_browser_host != nullptr) lv_obj_remove_flag(g_browser_host, LV_OBJ_FLAG_HIDDEN);
    if (g_reader_host != nullptr) lv_obj_add_flag(g_reader_host, LV_OBJ_FLAG_HIDDEN);
    if (g_reader_text != nullptr) lv_label_set_text(g_reader_text, "");
    if (g_browser_load.phase == BrowserLoadPhase::Idle && g_directory.count > 0) {
        ebook_update_browser_virtual_rows();
    }
    ebook_update_header();
}

static void ebook_show_reader()
{
    gesture_router_reset();
    g_press_tracking = false;
    g_page = EbookPage::Reader;
    ebook_set_browser_chrome_visible(false);
    if (g_browser_host != nullptr) lv_obj_add_flag(g_browser_host, LV_OBJ_FLAG_HIDDEN);
    if (g_reader_host != nullptr) {
        lv_obj_remove_flag(g_reader_host, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_reader_host);
    }
    ebook_set_reader_view_mode(ReaderViewMode::Labeled);
}

static void ebook_update_reader_controls()
{
    if (g_page_index_builder.active) {
        if (g_reader_page_info != nullptr) {
            char info[64] = {};
            snprintf(info, sizeof(info), "正在定位… 已扫描 %u 页",
                static_cast<unsigned>(g_page_index_builder.ordinal + 1U));
            lv_label_set_text(g_reader_page_info, info);
        }
        if (g_reader_prev != nullptr) lv_obj_set_style_opa(g_reader_prev, LV_OPA_40, 0);
        if (g_reader_next != nullptr) lv_obj_set_style_opa(g_reader_next, LV_OPA_40, 0);
        if (g_reader_bookmark != nullptr) lv_obj_set_style_opa(g_reader_bookmark, LV_OPA_40, 0);
        if (g_reader_view != nullptr) lv_obj_set_style_opa(g_reader_view, LV_OPA_40, 0);
        return;
    }

    if (g_reader_bookmark != nullptr) lv_obj_set_style_opa(g_reader_bookmark, LV_OPA_COVER, 0);
    if (g_reader_view != nullptr) lv_obj_set_style_opa(g_reader_view, LV_OPA_COVER, 0);
    if (g_reader_page_info != nullptr) {
        char info[96] = {};
        const uint64_t progress = g_book_size > 0
            ? ((g_read_anchor_offset > g_book_size ? g_book_size : g_read_anchor_offset) * 100U) / g_book_size
            : 0;
        snprintf(info, sizeof(info), "第 %u 页 · %llu%%",
            static_cast<unsigned>(g_current_page_ordinal + 1U),
            static_cast<unsigned long long>(progress));
        lv_label_set_text(g_reader_page_info, info);
    }
    if (g_reader_prev != nullptr) {
        const bool can_previous = g_current_page_ordinal > 0;
        lv_obj_set_style_opa(g_reader_prev, can_previous ? LV_OPA_COVER : LV_OPA_40, 0);
    }
    if (g_reader_next != nullptr) {
        lv_obj_set_style_opa(g_reader_next, !g_page_at_end ? LV_OPA_COVER : LV_OPA_40, 0);
    }
    ebook_update_bookmark_control();
}

static uint16_t ebook_reader_glyph_width(uint32_t codepoint, uint32_t next_codepoint)
{
    const lv_font_t *font = font_manager_get_ui_font();
    if (font == nullptr || font->get_glyph_dsc == nullptr) {
        return codepoint < 0x80U ? 12U : 24U;
    }
    return lv_font_get_glyph_width(font, codepoint, next_codepoint);
}

static EbookReader::PageLayout ebook_reader_page_layout_for_mode(ReaderViewMode mode)
{
    EbookReader::PageLayout layout = {};
    layout.text_width_px = static_cast<uint16_t>(460 - kReaderMarginX * 2);
    layout.glyph_width = ebook_reader_glyph_width;

    const lv_font_t *font = font_manager_get_ui_font();
    const int32_t line_height = font != nullptr ? font->line_height : 32;
    const int32_t stride = line_height + kReaderLineSpace;
    const int32_t text_height = ebook_reader_text_layout_height_for_mode(mode);
    int32_t lines = stride > 0
        ? (text_height + kReaderLineSpace) / stride
        : 1;
    if (lines < 1) lines = 1;
    if (lines > 32) lines = 32;
    if (mode == ReaderViewMode::Labeled && lines > kReaderLabeledMaxLines) {
        lines = kReaderLabeledMaxLines;
    }
    layout.max_lines = static_cast<uint8_t>(lines);
    return layout;
}

static uint32_t ebook_page_index_layout_signature(ReaderViewMode mode)
{
    const EbookReader::PageLayout layout = ebook_reader_page_layout_for_mode(mode);
    const lv_font_t *font = font_manager_get_ui_font();
    const uint32_t fields[] = {
        kPageIndexLayoutRevision,
        EbookReader::kPaginationAlgorithmRevision,
        static_cast<uint32_t>(mode),
        static_cast<uint32_t>(layout.text_width_px),
        static_cast<uint32_t>(layout.max_lines),
        static_cast<uint32_t>(font != nullptr ? font->line_height : 32),
        static_cast<uint32_t>(kReaderLineSpace),
    };
    uint32_t hash = 2166136261U;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(fields);
    for (size_t i = 0; i < sizeof(fields); ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash == 0 ? 1U : hash;
}

static void ebook_restore_page_index_cache(ReaderViewMode mode)
{
    if (g_book_path == nullptr || g_book_path[0] == '\0' || g_book_size == 0) return;
    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    if (index->count != 0 || index->starts != nullptr) return;

    EbookPageIndexCache::LoadResult loaded = {};
    const esp_err_t ret = EbookPageIndexCache::load(
        g_book_path,
        static_cast<uint8_t>(ebook_view_slot(mode)),
        g_book_size,
        ebook_page_index_layout_signature(mode),
        &loaded);
    if (ret != ESP_OK || loaded.count == 0 || loaded.starts == nullptr) {
        EbookPageIndexCache::release_load_result(&loaded);
        return;
    }

    index->starts = loaded.starts;
    index->count = loaded.count;
    index->capacity = loaded.capacity;
    index->persisted_count = loaded.count;
    loaded = {};
    ESP_LOGI(TAG, "Reader页索引缓存恢复：mode=%s pages=%u last=%llu",
        mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
        static_cast<unsigned>(index->count),
        static_cast<unsigned long long>(index->starts[index->count - 1U]));
}

static void ebook_checkpoint_page_index(ReaderViewMode mode, bool force)
{
    if (g_book_path == nullptr || g_book_path[0] == '\0' || g_book_size == 0) return;

    bool competing_flac = false;
    uint32_t flac_percent = 100U;
    if (!ebook_flac_storage_window_safe(&competing_flac, &flac_percent)) {
        if (force && competing_flac) {
            ESP_LOGI(TAG, "Reader页索引checkpoint让路FLAC：mode=%s ring=%u%%",
                mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
                static_cast<unsigned>(flac_percent));
        }
        return;
    }
    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    if (index->starts == nullptr || index->count < 2 || index->persisted_count >= index->count) return;
    const size_t pending = index->count - index->persisted_count;
    if (!force && pending < EbookPageIndexCache::kCommitStridePages) return;

    size_t persisted = index->persisted_count;
    const esp_err_t ret = EbookPageIndexCache::commit_prefix(
        g_book_path,
        static_cast<uint8_t>(ebook_view_slot(mode)),
        g_book_size,
        ebook_page_index_layout_signature(mode),
        index->starts,
        index->count,
        index->persisted_count,
        &persisted);
    if (ret == ESP_OK) {
        index->persisted_count = persisted;
        ESP_LOGI(TAG, "Reader页索引checkpoint：mode=%s pages=%u",
            mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
            static_cast<unsigned>(persisted));
    } else {
        ESP_LOGW(TAG, "Reader页索引checkpoint失败：mode=%s ret=%s",
            mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
            esp_err_to_name(ret));
    }
}

static void ebook_flush_page_index_caches(bool force)
{
    // 当前显示页若正好位于索引尾部，把已经由正文加载得到的 next_offset 一并纳入页表。
    // 这样书签位于当前页中间时，下次可以仅凭两个相邻页界直接判定归属，不必再扫描一页。
    if (g_reader_page_loaded && !g_page_at_end && g_page_next > g_page_start) {
        ReaderPageIndex *current = ebook_page_index_for_mode(g_reader_view_mode);
        if (current->starts != nullptr && current->count == g_current_page_ordinal + 1U &&
            current->starts[g_current_page_ordinal] == g_page_start) {
            (void)ebook_page_index_push(current, g_page_next);
        }
    }
    ebook_checkpoint_page_index(ReaderViewMode::Fullscreen, force);
    ebook_checkpoint_page_index(ReaderViewMode::Labeled, force);
}

static void ebook_apply_loaded_page(EbookReader::TextPage *page, uint64_t semantic_anchor)
{
    if (page == nullptr) return;
    lv_label_set_text(g_reader_text, page->text != nullptr ? page->text : "");
    g_page_start = page->start_offset;
    g_page_next = page->next_offset;
    g_book_size = page->file_size;
    g_page_at_end = page->at_end;
    g_reader_page_loaded = true;
    if (semantic_anchor < g_page_start || semantic_anchor >= g_page_next) {
        semantic_anchor = g_page_start;
    }
    g_read_anchor_offset = semantic_anchor;
    g_reader_position_dirty = !(g_bookmark_valid && g_bookmark_offset == g_read_anchor_offset);
}

static esp_err_t ebook_load_page_for_mode(
    ReaderViewMode mode, uint64_t offset, EbookReader::TextPage *out_page)
{
    if (g_book_path == nullptr || g_book_path[0] == '\0' || out_page == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const EbookReader::PageLayout layout = ebook_reader_page_layout_for_mode(mode);
    return EbookReader::load_text_page(g_book_path, offset, layout, out_page);
}

static bool ebook_find_anchor_in_known_index(
    ReaderViewMode mode, uint64_t anchor, size_t *out_ordinal)
{
    if (out_ordinal == nullptr || g_book_path == nullptr || g_book_path[0] == '\0' ||
        g_book_size == 0 || anchor >= g_book_size) {
        return false;
    }

    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    if (index->count == 0 && !ebook_page_index_push(index, 0)) {
        return false;
    }
    if (anchor > index->starts[index->count - 1U]) {
        return false;
    }

    size_t lo = 0;
    size_t hi = index->count;
    while (lo + 1U < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        if (index->starts[mid] <= anchor) lo = mid;
        else hi = mid;
    }
    *out_ordinal = lo;
    return true;
}

static esp_err_t ebook_load_indexed_page(
    ReaderViewMode mode, size_t ordinal, uint64_t semantic_anchor)
{
    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    if (index->starts == nullptr || ordinal >= index->count) return ESP_ERR_INVALID_ARG;

    EbookReader::TextPage page = {};
    const esp_err_t ret = ebook_load_page_for_mode(mode, index->starts[ordinal], &page);
    if (ret != ESP_OK) return ret;
    ebook_apply_loaded_page(&page, semantic_anchor);
    g_current_page_ordinal = ordinal;
    EbookReader::release_text_page(&page);
    ebook_update_reader_controls();
    return ESP_OK;
}

static ReaderViewMode ebook_opposite_reader_view(ReaderViewMode mode)
{
    return mode == ReaderViewMode::Labeled
        ? ReaderViewMode::Fullscreen
        : ReaderViewMode::Labeled;
}

static void ebook_refresh_page_index_timer()
{
    if (g_page_index_timer == nullptr) return;
    if (g_page_index_builder.active || g_page_index_shadow.pending || g_page_index_shadow.active) {
        lv_timer_resume(g_page_index_timer);
    } else {
        lv_timer_pause(g_page_index_timer);
    }
}

static void ebook_finish_shadow_scan(bool clear_pending)
{
    if (g_page_index_shadow.scan.file != nullptr || g_page_index_shadow.active) {
        EbookReader::end_page_scan(&g_page_index_shadow.scan);
    }
    g_page_index_shadow.active = false;
    g_page_index_shadow.ordinal = 0;
    g_page_index_shadow.last_step_tick_ms = 0;
    if (clear_pending) {
        g_page_index_shadow.pending = false;
        g_page_index_shadow.anchor = 0;
    }
    ebook_refresh_page_index_timer();
}

static void ebook_cancel_shadow_warmup()
{
    ebook_finish_shadow_scan(true);
    g_page_index_shadow = {};
    ebook_refresh_page_index_timer();
}

static void ebook_schedule_shadow_warmup()
{
    if (g_page_index_timer == nullptr || g_page != EbookPage::Reader ||
        !g_reader_page_loaded || g_page_index_builder.active ||
        g_book_path == nullptr || g_book_path[0] == '\0' ||
        g_book_size == 0 || g_read_anchor_offset >= g_book_size) {
        return;
    }

    const ReaderViewMode target_mode = ebook_opposite_reader_view(g_reader_view_mode);
    const uint64_t target_anchor = g_read_anchor_offset;
    ReaderPageIndex *target_index = ebook_page_index_for_mode(target_mode);
    if (target_index != nullptr && target_index->count > 0 &&
        target_anchor <= target_index->starts[target_index->count - 1U]) {
        // Shadow 成功时会连同目标页后的 next boundary 一并写入索引。只要当前 anchor
        // 不超过最后已知边界，就已经可以直接映射，无需每翻一页都重新启动后台 timer。
        ebook_cancel_shadow_warmup();
        return;
    }
    if ((g_page_index_shadow.pending || g_page_index_shadow.active) &&
        g_page_index_shadow.mode == target_mode &&
        g_page_index_shadow.anchor == target_anchor) {
        ebook_refresh_page_index_timer();
        return;
    }

    ebook_cancel_shadow_warmup();
    g_page_index_shadow.pending = true;
    g_page_index_shadow.mode = target_mode;
    g_page_index_shadow.anchor = target_anchor;
    g_page_index_shadow.last_step_tick_ms = static_cast<uint32_t>(lv_tick_get());
    lv_timer_reset(g_page_index_timer);
    ebook_refresh_page_index_timer();
}

static void ebook_shadow_warmup_fail(esp_err_t error)
{
    ESP_LOGW(TAG, "Reader影子索引暂停：mode=%s anchor=%llu ret=%s",
        g_page_index_shadow.mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
        static_cast<unsigned long long>(g_page_index_shadow.anchor),
        esp_err_to_name(error));
    ebook_finish_shadow_scan(true);
    g_page_index_shadow = {};
    ebook_refresh_page_index_timer();
}

static void ebook_shadow_warmup_success()
{
    const ReaderViewMode mode = g_page_index_shadow.mode;
    const uint64_t anchor = g_page_index_shadow.anchor;
    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    const size_t pages = index != nullptr ? index->count : 0;
    ebook_finish_shadow_scan(true);
    g_page_index_shadow = {};
    ebook_refresh_page_index_timer();
    ESP_LOGI(TAG, "Reader影子索引已追平：mode=%s anchor=%llu known_pages=%u",
        mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
        static_cast<unsigned long long>(anchor),
        static_cast<unsigned>(pages));
}

static void ebook_page_index_shadow_tick()
{
    if ((!g_page_index_shadow.pending && !g_page_index_shadow.active) ||
        g_page_index_builder.active || g_page != EbookPage::Reader ||
        !g_reader_page_loaded || app_manager_foreground() != AppId::Ebook ||
        app_launcher_overlay_is_visible()) {
        return;
    }

    // 阅读 anchor 或当前视图已经变化时，放弃旧 FILE 会话并从已建立的索引尾部重新追最新目标。
    const ReaderViewMode expected_mode = ebook_opposite_reader_view(g_reader_view_mode);
    if (g_page_index_shadow.mode != expected_mode ||
        g_page_index_shadow.anchor != g_read_anchor_offset) {
        ebook_schedule_shadow_warmup();
        return;
    }

    if (ui_touch_input_recent_activity(kPageIndexShadowIdleMs)) return;
    if (!ebook_flac_storage_window_safe()) return;

    const uint32_t now_ms = static_cast<uint32_t>(lv_tick_get());
    if (g_page_index_shadow.last_step_tick_ms != 0U &&
        static_cast<uint32_t>(now_ms - g_page_index_shadow.last_step_tick_ms) <
            kPageIndexShadowStepIntervalMs) {
        return;
    }
    g_page_index_shadow.last_step_tick_ms = now_ms;

    ReaderPageIndex *index = ebook_page_index_for_mode(g_page_index_shadow.mode);
    if (index == nullptr) {
        ebook_shadow_warmup_fail(ESP_ERR_INVALID_STATE);
        return;
    }

    if (!g_page_index_shadow.active) {
        // 另一视图 cache 的恢复也放到空闲窗口，而不是阻塞打开 TXT 的前台路径。
        ebook_restore_page_index_cache(g_page_index_shadow.mode);
        size_t known_ordinal = 0;
        if (ebook_find_anchor_in_known_index(
                g_page_index_shadow.mode, g_page_index_shadow.anchor, &known_ordinal)) {
            ebook_shadow_warmup_success();
            return;
        }
        if (index->count == 0 && !ebook_page_index_push(index, 0)) {
            ebook_shadow_warmup_fail(ESP_ERR_NO_MEM);
            return;
        }

        EbookReader::PageScanSession scan = {};
        const esp_err_t begin_ret = EbookReader::begin_page_scan(g_book_path, &scan);
        if (begin_ret != ESP_OK) {
            ebook_shadow_warmup_fail(begin_ret);
            return;
        }
        g_page_index_shadow.scan = scan;
        g_page_index_shadow.ordinal = index->count - 1U;
        g_page_index_shadow.active = true;
        ESP_LOGI(TAG, "Reader影子索引启动：mode=%s anchor=%llu from_page=%u idle=%ums step=%ums",
            g_page_index_shadow.mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
            static_cast<unsigned long long>(g_page_index_shadow.anchor),
            static_cast<unsigned>(g_page_index_shadow.ordinal + 1U),
            static_cast<unsigned>(kPageIndexShadowIdleMs),
            static_cast<unsigned>(kPageIndexShadowStepIntervalMs));
    }

    const EbookReader::PageLayout layout =
        ebook_reader_page_layout_for_mode(g_page_index_shadow.mode);
    for (uint8_t page_count = 0; page_count < kPageIndexShadowMaxPagesPerStep; ++page_count) {
        if (g_page_index_shadow.ordinal >= index->count) {
            ebook_shadow_warmup_fail(ESP_ERR_INVALID_STATE);
            return;
        }
        const size_t ordinal = g_page_index_shadow.ordinal;
        const uint64_t start_offset = index->starts[ordinal];
        EbookReader::PageScanResult result = {};
        const esp_err_t ret = EbookReader::scan_page(
            &g_page_index_shadow.scan, start_offset, layout, &result);
        if (ret != ESP_OK) {
            ebook_shadow_warmup_fail(ret);
            return;
        }
        if (result.next_offset <= start_offset) {
            ebook_shadow_warmup_fail(ESP_ERR_INVALID_RESPONSE);
            return;
        }

        const uint64_t anchor = g_page_index_shadow.anchor;
        if (anchor < result.next_offset || result.at_end) {
            if (!result.at_end && !ebook_page_index_push(index, result.next_offset)) {
                ebook_shadow_warmup_fail(ESP_ERR_NO_MEM);
                return;
            }
            ebook_shadow_warmup_success();
            return;
        }
        if (anchor == result.next_offset) {
            if (!result.at_end && !ebook_page_index_push(index, result.next_offset)) {
                ebook_shadow_warmup_fail(ESP_ERR_NO_MEM);
                return;
            }
            ebook_shadow_warmup_success();
            return;
        }

        if (!ebook_page_index_push(index, result.next_offset)) {
            ebook_shadow_warmup_fail(ESP_ERR_NO_MEM);
            return;
        }
        g_page_index_shadow.ordinal = ordinal + 1U;
    }
}

static void ebook_finish_page_index_builder_session()
{
    EbookReader::end_page_scan(&g_page_index_builder.scan);
    g_page_index_builder.active = false;
    ebook_refresh_page_index_timer();
}

static void ebook_cancel_page_index_builder()
{
    if (g_page_index_builder.scan.file != nullptr || g_page_index_builder.active) {
        EbookReader::end_page_scan(&g_page_index_builder.scan);
    }
    g_page_index_builder = {};
    ebook_refresh_page_index_timer();
}

static void ebook_restore_after_builder_failure(
    PageIndexBuildPurpose purpose, ReaderViewMode mode, uint64_t anchor,
    ReaderViewMode previous_mode, size_t previous_ordinal, uint64_t previous_anchor,
    esp_err_t error)
{
    ESP_LOGW(TAG, "Reader增量页索引失败：purpose=%u mode=%s anchor=%llu ret=%s",
        static_cast<unsigned>(purpose),
        mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
        static_cast<unsigned long long>(anchor), esp_err_to_name(error));

    if (purpose == PageIndexBuildPurpose::SwitchView) {
        g_reader_view_mode = previous_mode;
        ebook_apply_reader_view_chrome(previous_mode);
        const esp_err_t restore_ret = ebook_load_indexed_page(
            previous_mode, previous_ordinal, previous_anchor);
        if (restore_ret != ESP_OK) {
            ESP_LOGE(TAG, "Reader异步重排失败后恢复原页失败：%s", esp_err_to_name(restore_ret));
        }
        return;
    }

    if (purpose == PageIndexBuildPurpose::OpenBookmark && anchor != 0 && g_book_path != nullptr) {
        EbookBookmarkStore::clear(g_book_path);
        g_bookmark_valid = false;
        g_bookmark_offset = 0;
        ReaderPageIndex *index = ebook_page_index_for_mode(mode);
        if (index->count == 0 && !ebook_page_index_push(index, 0)) {
            if (g_reader_text != nullptr) lv_label_set_text(g_reader_text, "分页索引内存不足");
            return;
        }
        const esp_err_t fallback_ret = ebook_load_indexed_page(mode, 0, 0);
        if (fallback_ret != ESP_OK && g_reader_text != nullptr) {
            lv_label_set_text(g_reader_text, "阅读位置定位失败");
        }
    }
}

static void ebook_page_index_builder_fail(esp_err_t error)
{
    const PageIndexBuildPurpose purpose = g_page_index_builder.purpose;
    const ReaderViewMode mode = g_page_index_builder.mode;
    const uint64_t anchor = g_page_index_builder.anchor;
    const ReaderViewMode previous_mode = g_page_index_builder.previous_mode;
    const size_t previous_ordinal = g_page_index_builder.previous_ordinal;
    const uint64_t previous_anchor = g_page_index_builder.previous_anchor;

    ebook_finish_page_index_builder_session();
    g_page_index_builder = {};
    ebook_restore_after_builder_failure(
        purpose, mode, anchor, previous_mode, previous_ordinal, previous_anchor, error);
}

static void ebook_page_index_builder_success(size_t ordinal)
{
    const PageIndexBuildPurpose purpose = g_page_index_builder.purpose;
    const ReaderViewMode mode = g_page_index_builder.mode;
    const uint64_t anchor = g_page_index_builder.anchor;
    const ReaderViewMode previous_mode = g_page_index_builder.previous_mode;
    const size_t previous_ordinal = g_page_index_builder.previous_ordinal;
    const uint64_t previous_anchor = g_page_index_builder.previous_anchor;

    ebook_finish_page_index_builder_session();
    ebook_checkpoint_page_index(mode, true);
    g_page_index_builder = {};

    if (purpose == PageIndexBuildPurpose::SwitchView) {
        g_reader_view_mode = mode;
        ebook_apply_reader_view_chrome(mode);
    }
    const esp_err_t ret = ebook_load_indexed_page(mode, ordinal, anchor);
    if (ret != ESP_OK) {
        ebook_restore_after_builder_failure(
            purpose, mode, anchor, previous_mode, previous_ordinal, previous_anchor, ret);
        return;
    }

    ESP_LOGI(TAG, "Reader增量页索引完成：purpose=%u mode=%s anchor=%llu page=%u start=%llu next=%llu",
        static_cast<unsigned>(purpose),
        mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
        static_cast<unsigned long long>(anchor),
        static_cast<unsigned>(ordinal + 1U),
        static_cast<unsigned long long>(g_page_start),
        static_cast<unsigned long long>(g_page_next));
    ebook_schedule_shadow_warmup();
}

static void ebook_page_index_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g_page_index_builder.active) {
        ebook_page_index_shadow_tick();
        return;
    }
    if (g_page != EbookPage::Reader || app_manager_foreground() != AppId::Ebook) {
        return;
    }
    if (!ebook_flac_storage_window_safe()) {
        return;
    }

    ReaderPageIndex *index = ebook_page_index_for_mode(g_page_index_builder.mode);
    if (index->starts == nullptr || g_page_index_builder.ordinal >= index->count) {
        ebook_page_index_builder_fail(ESP_ERR_INVALID_STATE);
        return;
    }

    const EbookReader::PageLayout layout =
        ebook_reader_page_layout_for_mode(g_page_index_builder.mode);
    const int64_t tick_start_us = esp_timer_get_time();
    uint8_t pages_scanned = 0;

    while (g_page_index_builder.active &&
           pages_scanned < kPageIndexBuildMaxPagesPerTick) {
        const size_t ordinal = g_page_index_builder.ordinal;
        const uint64_t start_offset = index->starts[ordinal];
        EbookReader::PageScanResult result = {};
        const esp_err_t ret = EbookReader::scan_page(
            &g_page_index_builder.scan, start_offset, layout, &result);
        if (ret != ESP_OK) {
            ebook_page_index_builder_fail(ret);
            return;
        }
        ++pages_scanned;

        if (result.next_offset <= start_offset) {
            ebook_page_index_builder_fail(ESP_ERR_INVALID_RESPONSE);
            return;
        }

        const uint64_t anchor = g_page_index_builder.anchor;
        if (anchor < result.next_offset || result.at_end) {
            // 非末页时连同目标页的 next boundary 一起缓存；否则相同页内书签下次仍需重扫该页。
            if (!result.at_end && !ebook_page_index_push(index, result.next_offset)) {
                ebook_page_index_builder_fail(ESP_ERR_NO_MEM);
                return;
            }
            ebook_page_index_builder_success(ordinal);
            return;
        }
        if (anchor == result.next_offset) {
            if (result.at_end) {
                ebook_page_index_builder_success(ordinal);
                return;
            }
            if (!ebook_page_index_push(index, result.next_offset)) {
                ebook_page_index_builder_fail(ESP_ERR_NO_MEM);
                return;
            }
            ebook_page_index_builder_success(ordinal + 1U);
            return;
        }

        if (!ebook_page_index_push(index, result.next_offset)) {
            ebook_page_index_builder_fail(ESP_ERR_NO_MEM);
            return;
        }
        g_page_index_builder.ordinal = ordinal + 1U;

        if (esp_timer_get_time() - tick_start_us >= kPageIndexBuildBudgetUs) break;
    }

    if (g_reader_page_info != nullptr && g_page_index_builder.active) {
        char info[64] = {};
        snprintf(info, sizeof(info), "正在定位… 已扫描 %u 页",
            static_cast<unsigned>(g_page_index_builder.ordinal + 1U));
        lv_label_set_text(g_reader_page_info, info);
    }
}

static esp_err_t ebook_start_page_index_builder(
    ReaderViewMode mode, uint64_t anchor, PageIndexBuildPurpose purpose,
    ReaderViewMode previous_mode, size_t previous_ordinal, uint64_t previous_anchor)
{
    if (g_page_index_timer == nullptr || g_page_index_builder.active ||
        g_book_path == nullptr || g_book_path[0] == '\0' ||
        g_book_size == 0 || anchor >= g_book_size) {
        return ESP_ERR_INVALID_STATE;
    }

    ebook_cancel_shadow_warmup();
    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    if (index->count == 0 && !ebook_page_index_push(index, 0)) {
        return ESP_ERR_NO_MEM;
    }
    if (anchor <= index->starts[index->count - 1U]) {
        return ESP_ERR_INVALID_STATE;
    }

    EbookReader::PageScanSession scan = {};
    const esp_err_t ret = EbookReader::begin_page_scan(g_book_path, &scan);
    if (ret != ESP_OK) return ret;

    g_page_index_builder = {};
    g_page_index_builder.active = true;
    g_page_index_builder.purpose = purpose;
    g_page_index_builder.mode = mode;
    g_page_index_builder.anchor = anchor;
    g_page_index_builder.ordinal = index->count - 1U;
    g_page_index_builder.previous_mode = previous_mode;
    g_page_index_builder.previous_ordinal = previous_ordinal;
    g_page_index_builder.previous_anchor = previous_anchor;
    g_page_index_builder.scan = scan;

    lv_timer_reset(g_page_index_timer);
    ebook_refresh_page_index_timer();
    ESP_LOGI(TAG, "Reader增量页索引启动：purpose=%u mode=%s anchor=%llu from_page=%u budget=%lldus max=%u",
        static_cast<unsigned>(purpose),
        mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
        static_cast<unsigned long long>(anchor),
        static_cast<unsigned>(g_page_index_builder.ordinal + 1U),
        static_cast<long long>(kPageIndexBuildBudgetUs),
        static_cast<unsigned>(kPageIndexBuildMaxPagesPerTick));
    return ESP_OK;
}

static esp_err_t ebook_ensure_next_page_index(ReaderViewMode mode, size_t ordinal)
{
    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    if (index->starts == nullptr || ordinal >= index->count) return ESP_ERR_INVALID_ARG;
    if (ordinal + 1U < index->count) return ESP_OK;

    if (mode == g_reader_view_mode && ordinal == g_current_page_ordinal &&
        g_page_start == index->starts[ordinal] && g_page_next > g_page_start) {
        if (g_page_at_end) return ESP_ERR_NOT_FOUND;
        return ebook_page_index_push(index, g_page_next) ? ESP_OK : ESP_ERR_NO_MEM;
    }

    EbookReader::TextPage page = {};
    const esp_err_t ret = ebook_load_page_for_mode(mode, index->starts[ordinal], &page);
    if (ret != ESP_OK) return ret;
    const uint64_t next = page.next_offset;
    const bool at_end = page.at_end;
    EbookReader::release_text_page(&page);
    if (at_end) return ESP_ERR_NOT_FOUND;
    if (next <= index->starts[ordinal]) return ESP_ERR_INVALID_RESPONSE;
    return ebook_page_index_push(index, next) ? ESP_OK : ESP_ERR_NO_MEM;
}

static void ebook_reader_next_page()
{
    if (g_page != EbookPage::Reader || g_page_at_end || g_page_index_builder.active) return;
    ebook_cancel_shadow_warmup();
    const ReaderViewMode mode = g_reader_view_mode;
    const size_t next_ordinal = g_current_page_ordinal + 1U;
    const esp_err_t index_ret = ebook_ensure_next_page_index(mode, g_current_page_ordinal);
    if (index_ret != ESP_OK) {
        if (index_ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "TXT下一页索引失败：page=%u ret=%s",
                static_cast<unsigned>(g_current_page_ordinal + 1U), esp_err_to_name(index_ret));
        }
        return;
    }

    ebook_checkpoint_page_index(mode, false);
    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    const uint64_t next_start = index->starts[next_ordinal];
    const esp_err_t ret = ebook_load_indexed_page(mode, next_ordinal, next_start);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TXT下一页失败：offset=%llu ret=%s",
            static_cast<unsigned long long>(next_start), esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "TXT翻页：page=%u offset=%llu/%llu",
        static_cast<unsigned>(g_current_page_ordinal + 1U),
        static_cast<unsigned long long>(g_page_start),
        static_cast<unsigned long long>(g_book_size));
    ebook_schedule_shadow_warmup();
}

static void ebook_reader_previous_page()
{
    if (g_page != EbookPage::Reader || g_current_page_ordinal == 0 || g_page_index_builder.active) return;
    ebook_cancel_shadow_warmup();
    const ReaderViewMode mode = g_reader_view_mode;
    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    const size_t previous_ordinal = g_current_page_ordinal - 1U;
    if (index->starts == nullptr || previous_ordinal >= index->count) return;
    const uint64_t previous_start = index->starts[previous_ordinal];
    const esp_err_t ret = ebook_load_indexed_page(mode, previous_ordinal, previous_start);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TXT上一页失败：offset=%llu ret=%s",
            static_cast<unsigned long long>(previous_start), esp_err_to_name(ret));
        return;
    }
    ebook_schedule_shadow_warmup();
}

static const char *ebook_error_message(esp_err_t err)
{
    switch (err) {
        case ESP_ERR_NOT_FOUND:
            return "未找到 /txt\n请在TF卡根目录创建 txt 文件夹";
        case ESP_ERR_TIMEOUT:
            return "TF卡正忙\n请稍后再试";
        case ESP_ERR_NO_MEM:
            return "内存不足\n无法打开Reader";
        case ESP_ERR_NOT_SUPPORTED:
            return "暂不支持该TXT编码\nV1仅支持UTF-8";
        case ESP_ERR_INVALID_RESPONSE:
            return "TXT不是有效UTF-8文本";
        case ESP_ERR_INVALID_SIZE:
            return "TXT为空或大小异常";
        default:
            return "读取失败";
    }
}

static esp_err_t ebook_schedule_current_directory_load();

static void ebook_row_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click() ||
        g_browser_load.phase != BrowserLoadPhase::Idle ||
        g_current_dir == nullptr || g_scratch_path == nullptr) {
        return;
    }

    const uintptr_t raw_slot = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (raw_slot == 0) return;
    const size_t slot = static_cast<size_t>(raw_slot - 1U);
    if (slot >= kBrowserVisibleRows) return;
    const size_t index = g_browser_first_index + slot;
    const EbookReader::DirectoryEntryIndex *entry =
        EbookReader::directory_entry_at(&g_directory, index);
    const char *entry_name = EbookReader::directory_entry_name(&g_directory, index);
    if (entry == nullptr || entry_name == nullptr) return;

    const esp_err_t join_ret = EbookReader::join_child_path(
        g_current_dir, entry_name, g_scratch_path, EbookReader::kPathBytes);
    if (join_ret != ESP_OK) {
        ebook_set_status("路径过长，无法进入");
        return;
    }

    if (EbookReader::directory_entry_is_directory(entry)) {
        snprintf(g_current_dir, EbookReader::kPathBytes, "%s", g_scratch_path);
        const esp_err_t ret = ebook_schedule_current_directory_load();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Ebook目录打开失败：path=%s ret=%s",
                g_current_dir, esp_err_to_name(ret));
        }
        return;
    }

    ebook_reset_reader_session();
    snprintf(g_book_path, EbookReader::kPathBytes, "%s", g_scratch_path);

    uint64_t initial_offset = 0;
    bool bookmark_found = false;
    const esp_err_t bookmark_ret = EbookBookmarkStore::load(
        g_book_path, entry->size_bytes, &initial_offset, &bookmark_found);
    if (bookmark_ret != ESP_OK) {
        ESP_LOGW(TAG, "TXT书签读取失败：path=%s ret=%s", g_book_path, esp_err_to_name(bookmark_ret));
        initial_offset = 0;
        bookmark_found = false;
    }
    g_bookmark_valid = bookmark_found;
    g_bookmark_offset = bookmark_found ? initial_offset : 0;

    g_book_size = entry->size_bytes;
    // 前台只同步恢复默认 Labeled cache；Fullscreen cache 由 Shadow Index 在用户空闲后恢复，
    // 避免打开 TXT 的关键路径同步读取两套索引，同时让第一次切全屏也尽量命中预热结果。
    ebook_restore_page_index_cache(ReaderViewMode::Labeled);

    ReaderPageIndex *labeled_index = ebook_page_index_for_mode(ReaderViewMode::Labeled);
    esp_err_t ret = labeled_index->count > 0 || ebook_page_index_push(labeled_index, 0)
        ? ESP_OK : ESP_ERR_NO_MEM;

    if (ret == ESP_OK && initial_offset == 0) {
        ret = ebook_load_indexed_page(ReaderViewMode::Labeled, 0, 0);
        if (ret == ESP_OK) {
            ebook_show_reader();
            ebook_schedule_shadow_warmup();
            ESP_LOGI(TAG, "TXT小说重排打开：首页直达 %s bytes=%llu view=labeled",
                entry_name, static_cast<unsigned long long>(g_book_size));
            return;
        }
    } else if (ret == ESP_OK) {
        size_t cached_ordinal = 0;
        if (ebook_find_anchor_in_known_index(ReaderViewMode::Labeled, initial_offset, &cached_ordinal)) {
            ret = ebook_load_indexed_page(ReaderViewMode::Labeled, cached_ordinal, initial_offset);
            if (ret == ESP_OK) {
                ebook_show_reader();
                ebook_schedule_shadow_warmup();
                ESP_LOGI(TAG, "TXT深书签缓存直达：%s anchor=%llu page=%u cached_pages=%u",
                    entry_name,
                    static_cast<unsigned long long>(initial_offset),
                    static_cast<unsigned>(cached_ordinal + 1U),
                    static_cast<unsigned>(labeled_index->count));
                return;
            }
        }

        // 缓存尚未覆盖该书签时，继续沿用 V2 cooperative Builder；它从缓存尾页起算，
        // 不再固定从书首0开始。
        g_read_anchor_offset = initial_offset;
        g_reader_page_loaded = false;
        g_reader_position_dirty = false;
        g_current_page_ordinal = 0;
        g_page_at_end = false;
        ebook_show_reader();
        if (g_reader_text != nullptr) lv_label_set_text(g_reader_text, "正在定位阅读位置…");

        ret = ebook_start_page_index_builder(
            ReaderViewMode::Labeled, initial_offset, PageIndexBuildPurpose::OpenBookmark,
            ReaderViewMode::Labeled, 0, 0);
        if (ret == ESP_OK) {
            ebook_update_reader_controls();
            ESP_LOGI(TAG, "TXT深书签增量定位：%s bytes=%llu anchor=%llu cached_pages=%u",
                entry_name,
                static_cast<unsigned long long>(g_book_size),
                static_cast<unsigned long long>(initial_offset),
                static_cast<unsigned>(labeled_index->count));
            return;
        }

        ESP_LOGW(TAG, "TXT深书签Builder启动失败，回退首页：offset=%llu ret=%s",
            static_cast<unsigned long long>(initial_offset), esp_err_to_name(ret));
        EbookBookmarkStore::clear(g_book_path);
        g_bookmark_valid = false;
        g_bookmark_offset = 0;
        bookmark_found = false;
        g_read_anchor_offset = 0;
        ret = ebook_load_indexed_page(ReaderViewMode::Labeled, 0, 0);
        if (ret == ESP_OK) return;
    }

    ESP_LOGW(TAG, "TXT打开失败：path=%s ret=%s", g_book_path, esp_err_to_name(ret));
    if (g_reader_text != nullptr) lv_label_set_text(g_reader_text, ebook_error_message(ret));
    g_page_start = 0;
    g_page_next = 0;
    g_book_size = entry->size_bytes;
    g_page_at_end = true;
    ebook_show_reader();
    ebook_update_reader_controls();


}

static void ebook_set_browser_virtual_rows_visible(bool visible)
{
    for (size_t slot = 0; slot < kBrowserVisibleRows; ++slot) {
        if (g_browser_rows[slot].row == nullptr) continue;
        if (visible) lv_obj_remove_flag(g_browser_rows[slot].row, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_browser_rows[slot].row, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_browser_position != nullptr) {
        if (visible) lv_obj_remove_flag(g_browser_position, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_browser_position, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ebook_browser_show_status(const char *message, uint32_t color)
{
    if (g_browser_host == nullptr) return;
    ebook_set_browser_virtual_rows_visible(false);
    if (g_browser_status == nullptr) {
        g_browser_status = ebook_make_label(
            g_browser_host,
            message != nullptr ? message : "",
            color,
            LV_TEXT_ALIGN_CENTER);
        if (g_browser_status != nullptr) {
            lv_obj_set_width(g_browser_status, 370);
            lv_label_set_long_mode(g_browser_status, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(g_browser_status, 8, 0);
            lv_obj_align(g_browser_status, LV_ALIGN_TOP_MID, 0, 100);
        }
    } else {
        lv_label_set_text(g_browser_status, message != nullptr ? message : "");
        lv_obj_set_style_text_color(g_browser_status, lv_color_hex(color), 0);
        lv_obj_remove_flag(g_browser_status, LV_OBJ_FLAG_HIDDEN);
    }
    ebook_update_header();
}

static void ebook_reset_browser_virtual_ui_refs()
{
    for (size_t slot = 0; slot < kBrowserVisibleRows; ++slot) {
        g_browser_rows[slot] = {};
    }
    g_browser_position = nullptr;
    g_browser_first_index = 0;
}

static esp_err_t ebook_create_browser_virtual_rows()
{
    if (g_browser_host == nullptr) return ESP_ERR_INVALID_STATE;
    ebook_reset_browser_virtual_ui_refs();

    for (size_t slot = 0; slot < kBrowserVisibleRows; ++slot) {
        BrowserRowUi &ui = g_browser_rows[slot];
        ui.row = lv_button_create(g_browser_host);
        if (ui.row == nullptr) return ESP_ERR_NO_MEM;
        ui_common_lock_object(ui.row);
        lv_obj_set_pos(
            ui.row,
            0,
            static_cast<int32_t>(slot) * (kRowHeight + kBrowserRowGap));
        lv_obj_set_size(ui.row, LV_PCT(100), kRowHeight);
        lv_obj_set_style_radius(ui.row, 12, 0);
        lv_obj_set_style_border_width(ui.row, 0, 0);
        lv_obj_set_style_bg_opa(ui.row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(ui.row, 16, 0);
        lv_obj_set_style_pad_right(ui.row, 14, 0);
        lv_obj_add_event_cb(
            ui.row,
            ebook_row_clicked_cb,
            LV_EVENT_CLICKED,
            reinterpret_cast<void *>(static_cast<uintptr_t>(slot + 1U)));

        ui.name = ebook_make_label(ui.row, "", 0xEEF1F5, LV_TEXT_ALIGN_LEFT);
        if (ui.name == nullptr) return ESP_ERR_NO_MEM;
        lv_obj_set_width(ui.name, 344);
        lv_label_set_long_mode(ui.name, LV_LABEL_LONG_DOT);
        lv_obj_align(ui.name, LV_ALIGN_LEFT_MID, 0, 0);

        ui.kind = ebook_make_label(ui.row, "", 0x677385, LV_TEXT_ALIGN_RIGHT);
        if (ui.kind == nullptr) return ESP_ERR_NO_MEM;
        lv_obj_set_width(ui.kind, 54);
        lv_obj_align(ui.kind, LV_ALIGN_RIGHT_MID, 0, 0);

        lv_obj_add_flag(ui.row, LV_OBJ_FLAG_HIDDEN);
    }

    g_browser_position = ebook_make_label(
        g_browser_host, "", 0x697687, LV_TEXT_ALIGN_CENTER);
    if (g_browser_position == nullptr) return ESP_ERR_NO_MEM;
    lv_obj_set_width(g_browser_position, LV_PCT(100));
    lv_obj_align(g_browser_position, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_add_flag(g_browser_position, LV_OBJ_FLAG_HIDDEN);
    return ESP_OK;
}

static void ebook_update_browser_virtual_rows()
{
    if (g_directory.entries == nullptr || g_directory.count == 0) {
        ebook_set_browser_virtual_rows_visible(false);
        return;
    }

    const size_t max_first = g_directory.count > kBrowserVisibleRows
        ? g_directory.count - kBrowserVisibleRows
        : 0U;
    if (g_browser_first_index > max_first) g_browser_first_index = max_first;

    if (g_browser_status != nullptr) {
        lv_obj_add_flag(g_browser_status, LV_OBJ_FLAG_HIDDEN);
    }

    size_t last_visible = g_browser_first_index;
    for (size_t slot = 0; slot < kBrowserVisibleRows; ++slot) {
        BrowserRowUi &ui = g_browser_rows[slot];
        if (ui.row == nullptr || ui.name == nullptr || ui.kind == nullptr) continue;

        const size_t index = g_browser_first_index + slot;
        if (index >= g_directory.count) {
            lv_obj_add_flag(ui.row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const EbookReader::DirectoryEntryIndex *entry =
            EbookReader::directory_entry_at(&g_directory, index);
        const char *entry_name = EbookReader::directory_entry_name(&g_directory, index);
        if (entry == nullptr || entry_name == nullptr) {
            lv_obj_add_flag(ui.row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const bool is_directory = EbookReader::directory_entry_is_directory(entry);
        lv_label_set_text(ui.name, entry_name);
        lv_label_set_text(ui.kind, is_directory ? ">" : "TXT");
        lv_obj_set_style_text_color(
            ui.name,
            lv_color_hex(is_directory ? 0xBCD7FF : 0xEEF1F5),
            0);
        lv_obj_set_style_text_color(
            ui.kind,
            lv_color_hex(is_directory ? 0x6EA5F2 : 0x677385),
            0);
        lv_obj_set_style_bg_color(
            ui.row,
            lv_color_hex(is_directory ? 0x172230 : 0x151A21),
            0);
        lv_obj_remove_flag(ui.row, LV_OBJ_FLAG_HIDDEN);
        last_visible = index;
    }

    if (g_browser_position != nullptr) {
        char position[64] = {};
        snprintf(
            position,
            sizeof(position),
            "%u-%u / %u",
            static_cast<unsigned>(g_browser_first_index + 1U),
            static_cast<unsigned>(last_visible + 1U),
            static_cast<unsigned>(g_directory.count));
        lv_label_set_text(g_browser_position, position);
        lv_obj_remove_flag(g_browser_position, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ebook_browser_shift_window(int direction)
{
    if (g_page != EbookPage::Browser || g_browser_load.phase != BrowserLoadPhase::Idle ||
        g_directory.entries == nullptr || g_directory.count <= kBrowserVisibleRows || direction == 0) {
        return;
    }

    const size_t old_first = g_browser_first_index;
    const size_t max_first = g_directory.count - kBrowserVisibleRows;
    if (direction > 0) {
        const size_t candidate = old_first + kBrowserGestureStepRows;
        g_browser_first_index = candidate < max_first ? candidate : max_first;
    } else {
        g_browser_first_index = old_first > kBrowserGestureStepRows
            ? old_first - kBrowserGestureStepRows
            : 0U;
    }
    if (g_browser_first_index == old_first) return;

    ebook_update_browser_virtual_rows();
    ESP_LOGI(
        TAG,
        "Ebook虚拟目录：first=%u visible=%u total=%u direction=%s",
        static_cast<unsigned>(g_browser_first_index),
        static_cast<unsigned>(kBrowserVisibleRows),
        static_cast<unsigned>(g_directory.count),
        direction > 0 ? "NEXT" : "PREV");
}

static void ebook_cancel_browser_load()
{
    if (g_browser_load.scan.dir != nullptr || g_browser_load.scan.entries != nullptr ||
        g_browser_load.scan.string_pool != nullptr) {
        EbookReader::cancel_directory_scan(&g_browser_load.scan);
    }
    g_browser_load = {};
}

static esp_err_t ebook_schedule_current_directory_load()
{
    if (g_current_dir == nullptr || g_browser_host == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    ebook_cancel_browser_load();
    EbookReader::release_directory(&g_directory);
    g_browser_first_index = 0;
    ebook_browser_show_status("正在加载书库…", 0x8490A0);
    g_browser_load.phase = BrowserLoadPhase::WaitingForAudioWindow;
    ESP_LOGI(TAG, "Ebook目录协作加载已排队：%s", g_current_dir);
    return ESP_OK;
}

static void ebook_finish_browser_render()
{
    if (g_browser_host == nullptr) return;

    ebook_update_browser_virtual_rows();
    ebook_update_header();
    ESP_LOGI(TAG,
        "Ebook目录索引：%s items=%u index=%uB strings=%uB cooperative=YES virtual_rows=%u",
        g_current_dir != nullptr ? g_current_dir : "?",
        static_cast<unsigned>(g_directory.count),
        static_cast<unsigned>(g_directory.count * sizeof(EbookReader::DirectoryEntryIndex)),
        static_cast<unsigned>(g_directory.pool_size),
        static_cast<unsigned>(kBrowserVisibleRows));
    g_browser_load = {};
}

static void ebook_browser_load_fail(esp_err_t error)
{
    EbookReader::cancel_directory_scan(&g_browser_load.scan);
    EbookReader::release_directory(&g_directory);
    g_browser_load = {};
    g_browser_first_index = 0;
    ebook_browser_show_status(ebook_error_message(error), 0xD08C8C);
    ESP_LOGW(TAG, "Ebook目录协作加载失败：path=%s ret=%s",
        g_current_dir != nullptr ? g_current_dir : "?",
        esp_err_to_name(error));
}

static void ebook_browser_load_tick()
{
    if (g_browser_load.phase == BrowserLoadPhase::Idle ||
        g_page != EbookPage::Browser || g_browser_host == nullptr ||
        app_manager_foreground() != AppId::Ebook ||
        app_launcher_overlay_is_visible()) {
        return;
    }

    bool competing_flac = false;
    uint32_t flac_percent = 100U;
    const bool safe = ebook_flac_storage_window_safe(&competing_flac, &flac_percent);
    if (!safe) {
        const uint32_t now_ms = static_cast<uint32_t>(lv_tick_get());
        if (g_browser_load.last_wait_log_ms == 0U ||
            static_cast<uint32_t>(now_ms - g_browser_load.last_wait_log_ms) >=
                kBrowserWaitLogIntervalMs) {
            g_browser_load.last_wait_log_ms = now_ms;
            ESP_LOGI(TAG, "Ebook目录加载让路FLAC：ring=%u%% < %u%% phase=%u",
                static_cast<unsigned>(flac_percent),
                static_cast<unsigned>(kEbookBackgroundIoFlacSafePercent),
                static_cast<unsigned>(g_browser_load.phase));
        }
        return;
    }

    if (g_browser_load.phase == BrowserLoadPhase::WaitingForAudioWindow) {
        EbookReader::DirectoryScanSession scan = {};
        const esp_err_t ret = EbookReader::begin_directory_scan(g_current_dir, &scan);
        if (ret != ESP_OK) {
            ebook_browser_load_fail(ret);
            return;
        }
        g_browser_load.scan = scan;
        g_browser_load.phase = BrowserLoadPhase::Scanning;
        g_browser_load.last_wait_log_ms = 0;
    }

    if (g_browser_load.phase == BrowserLoadPhase::Scanning) {
        const size_t batch = competing_flac
            ? kBrowserScanBatchWithFlac
            : kBrowserScanBatchNoFlac;
        bool done = false;
        const esp_err_t ret = EbookReader::scan_directory_step(
            &g_browser_load.scan, batch, &done);
        if (ret != ESP_OK) {
            ebook_browser_load_fail(ret);
            return;
        }
        if (!done) return;

        const esp_err_t finish_ret =
            EbookReader::finish_directory_scan(&g_browser_load.scan, &g_directory);
        if (finish_ret != ESP_OK) {
            ebook_browser_load_fail(finish_ret);
            return;
        }

        g_browser_first_index = 0;
        if (g_directory.count == 0) {
            g_browser_load = {};
            ebook_browser_show_status(
                "这里还没有TXT文件\n支持子文件夹和 UTF-8 .txt",
                0x8490A0);
            ebook_update_header();
            ESP_LOGI(TAG,
                "Ebook目录索引：%s items=0 index=0B strings=%uB cooperative=YES virtual_rows=%u",
                g_current_dir != nullptr ? g_current_dir : "?",
                static_cast<unsigned>(g_directory.pool_size),
                static_cast<unsigned>(kBrowserVisibleRows));
            return;
        }
        ebook_finish_browser_render();
    }
}


static void ebook_go_back()
{
    if (g_page == EbookPage::Reader) {
        ebook_show_browser();
        return;
    }
    if (g_current_dir != nullptr && strcmp(g_current_dir, EbookReader::kRootDirectory) != 0 &&
        g_scratch_path != nullptr &&
        EbookReader::parent_path(g_current_dir, g_scratch_path, EbookReader::kPathBytes) == ESP_OK) {
        snprintf(g_current_dir, EbookReader::kPathBytes, "%s", g_scratch_path);
        ebook_schedule_current_directory_load();
        return;
    }
    ebook_show_app_launcher();
}

static void ebook_back_button_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click()) {
        return;
    }
    ebook_go_back();
}

static void ebook_reader_prev_button_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    ebook_reader_previous_page();
}

static void ebook_reader_next_button_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    ebook_reader_next_page();
}

static void ebook_reader_back_button_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    ebook_show_browser();
}

static void ebook_reader_bookmark_button_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        g_page_index_builder.active) return;
    ebook_save_current_position("手动书签", true);
}

static void ebook_reader_view_button_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        g_page_index_builder.active) return;
    ebook_set_reader_view_mode(ReaderViewMode::Fullscreen);
    ESP_LOGI(TAG, "Reader视图切换：带标签 -> 全屏");
}

static void ebook_reader_music_button_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;

    AudioStateSnapshot before = {};
    const bool has_before = audio_service_get_snapshot(&before) && before.ready;
    if (!player_control_toggle_play_pause()) {
        ESP_LOGW(TAG, "Reader后台音乐播放/暂停请求失败");
        ebook_update_music_control();
        return;
    }

    // 命令异步入 AudioTask；先按用户意图更新文案，下一次打开控制层再以真实 Snapshot 校正。
    if (g_reader_music_label != nullptr && has_before) {
        if (before.state == AudioPlaybackState::Playing || before.state == AudioPlaybackState::Seeking) {
            lv_label_set_text(g_reader_music_label, "继续音乐");
        } else {
            lv_label_set_text(g_reader_music_label, "暂停音乐");
        }
    }
    ESP_LOGI(TAG, "Reader后台音乐控制已提交：before=%s",
        has_before ? audio_playback_state_name_cn(before.state) : "未知");
}

static void ebook_reader_overlay_tap_cb(lv_event_t *event)
{
    if (g_page_index_builder.active || event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        static_cast<lv_obj_t *>(lv_event_get_target(event)) != g_reader_overlay ||
        g_reader_view_mode != ReaderViewMode::Labeled) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point = {};
    lv_indev_get_point(indev, &point);

    // 带标签视图中，只有顶部“全屏”标签按钮负责切换视图。
    // 正文区域本身只负责上下翻页：上半正文上一页，下半正文下一页。
    const int32_t text_top = kReaderLabeledTextY;
    const int32_t text_bottom = kReaderLabeledTextY + kReaderLabeledLayoutHeight;
    if (point.y < text_top || point.y >= text_bottom) return;

    const int32_t text_mid = text_top + kReaderLabeledLayoutHeight / 2;
    if (point.y < text_mid) {
        if (g_current_page_ordinal > 0) ebook_reader_previous_page();
    } else {
        if (!g_page_at_end && g_page_next > g_page_start) ebook_reader_next_page();
    }
}

static void ebook_reader_pointer_cb(lv_event_t *event)
{
    if (event == nullptr || g_page != EbookPage::Reader || g_page_index_builder.active) return;
    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) return;
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &g_press_start);
        g_press_tracking = true;
        return;
    }
    if ((code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) || !g_press_tracking) return;

    lv_point_t end = {};
    lv_indev_get_point(indev, &end);
    g_press_tracking = false;
    const int32_t dx = static_cast<int32_t>(end.x) - g_press_start.x;
    const int32_t dy = static_cast<int32_t>(end.y) - g_press_start.y;
    if (abs(dx) <= kReaderTapMaxMovePx && abs(dy) <= kReaderTapMaxMovePx) {
        // 全屏阅读从“左/中/右”改为“上/中/下”：
        // 顶部120px上一页，中央区域呼出带标签视图，底部120px下一页。
        // 不再提供任何左右点击或左右滑动翻页。
        if (g_reader_view_mode != ReaderViewMode::Fullscreen) return;
        if (end.y < kReaderEdgeTapHeightPx) {
            if (g_current_page_ordinal > 0) ebook_reader_previous_page();
        } else if (end.y >= 460 - kReaderEdgeTapHeightPx) {
            if (!g_page_at_end && g_page_next > g_page_start) ebook_reader_next_page();
        } else {
            ebook_set_reader_overlay_visible(true);
        }
        return;
    }
    // Reader不再响应左右滑动翻页；非轻点移动仅交还给触摸/手势系统。
}

static esp_err_t ebook_create()
{
    if (g_root != nullptr) return ESP_OK;
    lv_obj_t *screen = lv_screen_active();
    if (screen == nullptr) return ESP_ERR_INVALID_STATE;

    g_current_dir = static_cast<char *>(heap_caps_calloc(
        EbookReader::kPathBytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_scratch_path = static_cast<char *>(heap_caps_calloc(
        EbookReader::kPathBytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_book_path = static_cast<char *>(heap_caps_calloc(
        EbookReader::kPathBytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_current_dir == nullptr || g_scratch_path == nullptr || g_book_path == nullptr) {
        if (g_current_dir != nullptr) heap_caps_free(g_current_dir);
        if (g_scratch_path != nullptr) heap_caps_free(g_scratch_path);
        if (g_book_path != nullptr) heap_caps_free(g_book_path);
        g_current_dir = nullptr;
        g_scratch_path = nullptr;
        g_book_path = nullptr;
        return ESP_ERR_NO_MEM;
    }
    snprintf(g_current_dir, EbookReader::kPathBytes, "%s", EbookReader::kRootDirectory);

    // App Manager 会先 create 目标再 leave 当前 APP。create 期间禁止 invalidation，
    // root 保持 Hidden，直到 Music/Launcher 已安全退出前台后才由 enter() 产生第一帧。
    lv_display_t *display = lv_display_get_default();
    const bool restore_invalidation =
        display != nullptr && lv_display_is_invalidation_enabled(display);
    if (restore_invalidation) lv_display_enable_invalidation(display, false);

    g_root = lv_obj_create(screen);
    if (g_root == nullptr) {
        if (restore_invalidation) lv_display_enable_invalidation(display, true);
        heap_caps_free(g_current_dir);
        heap_caps_free(g_scratch_path);
        heap_caps_free(g_book_path);
        g_current_dir = nullptr;
        g_scratch_path = nullptr;
        g_book_path = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ui_common_lock_object(g_root);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x0A0D12), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);

    g_header_back = lv_button_create(g_root);
    if (g_header_back != nullptr) {
        ui_common_lock_object(g_header_back);
        lv_obj_set_size(g_header_back, 56, 44);
        lv_obj_align(g_header_back, LV_ALIGN_TOP_LEFT, 14, 12);
        ebook_expand_click_area(g_header_back, kBrowserBackTouchExpandPx);
        lv_obj_set_style_radius(g_header_back, 22, 0);
        lv_obj_set_style_bg_color(g_header_back, lv_color_hex(0x151B24), 0);
        lv_obj_set_style_bg_opa(g_header_back, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(g_header_back, 0, 0);
        lv_obj_add_event_cb(g_header_back, ebook_back_button_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *back_text = ebook_make_label(g_header_back, "<", 0xF0F3F7, LV_TEXT_ALIGN_CENTER);
        if (back_text != nullptr) lv_obj_center(back_text);
    }

    g_header_title = ebook_make_label(g_root, "电子书", 0xF2F4F7, LV_TEXT_ALIGN_CENTER);
    if (g_header_title != nullptr) {
        lv_obj_set_width(g_header_title, 300);
        lv_label_set_long_mode(g_header_title, LV_LABEL_LONG_DOT);
        lv_obj_align(g_header_title, LV_ALIGN_TOP_MID, 0, 20);
    }

    g_header_line = lv_obj_create(g_root);
    if (g_header_line != nullptr) {
        ui_common_lock_object(g_header_line);
        lv_obj_set_size(g_header_line, 390, 1);
        lv_obj_align(g_header_line, LV_ALIGN_TOP_MID, 0, kHeaderHeight - 1);
        lv_obj_set_style_border_width(g_header_line, 0, 0);
        lv_obj_set_style_bg_color(g_header_line, lv_color_hex(0x1C2430), 0);
        lv_obj_set_style_bg_opa(g_header_line, LV_OPA_COVER, 0);
    }

    esp_err_t browser_rows_ret = ESP_ERR_INVALID_STATE;
    g_browser_host = lv_obj_create(g_root);
    if (g_browser_host != nullptr) {
        ui_common_lock_object(g_browser_host);
        lv_obj_set_pos(g_browser_host, kContentMargin, kHeaderHeight + 8);
        lv_obj_set_size(g_browser_host,
            460 - kContentMargin * 2,
            460 - (kHeaderHeight + 8) - kContentMargin);
        lv_obj_set_style_radius(g_browser_host, 0, 0);
        lv_obj_set_style_border_width(g_browser_host, 0, 0);
        lv_obj_set_style_bg_opa(g_browser_host, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(g_browser_host, 0, 0);
        // Virtual Browser 不使用 LVGL native scroll：对象坐标固定，纵向 Flick 只改绑定的数据索引。
        lv_obj_remove_flag(g_browser_host, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(g_browser_host, LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_remove_flag(g_browser_host, LV_OBJ_FLAG_SCROLL_ELASTIC);
        lv_obj_set_scroll_dir(g_browser_host, LV_DIR_NONE);
        lv_obj_set_scrollbar_mode(g_browser_host, LV_SCROLLBAR_MODE_OFF);
        browser_rows_ret = ebook_create_browser_virtual_rows();
    }

    // Reader 真正占满 460x460；标题/返回/翻页/书签/后台音乐位于 Labeled 控制层。
    // 默认进入带标签分页，需要时由“全屏”按钮切换到 Fullscreen。
    g_reader_host = lv_obj_create(g_root);
    if (g_reader_host != nullptr) {
        lv_obj_set_pos(g_reader_host, 0, 0);
        lv_obj_set_size(g_reader_host, 460, 460);
        lv_obj_set_style_radius(g_reader_host, 0, 0);
        lv_obj_set_style_border_width(g_reader_host, 0, 0);
        lv_obj_set_style_bg_color(g_reader_host, lv_color_hex(0x0A0D12), 0);
        lv_obj_set_style_bg_opa(g_reader_host, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(g_reader_host, 0, 0);
        lv_obj_remove_flag(g_reader_host, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(g_reader_host, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_reader_host, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(g_reader_host, ebook_reader_pointer_cb, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(g_reader_host, ebook_reader_pointer_cb, LV_EVENT_RELEASED, nullptr);
        lv_obj_add_event_cb(g_reader_host, ebook_reader_pointer_cb, LV_EVENT_PRESS_LOST, nullptr);

        g_reader_text = ebook_make_label(g_reader_host, "", 0xE8EBEF, LV_TEXT_ALIGN_LEFT);
        if (g_reader_text != nullptr) {
            lv_obj_set_pos(g_reader_text, kReaderMarginX, kReaderFullscreenTextY);
            lv_obj_set_size(
                g_reader_text,
                460 - kReaderMarginX * 2,
                kReaderFullscreenTextHeight);
            lv_label_set_long_mode(g_reader_text, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(g_reader_text, kReaderLineSpace, 0);
            lv_obj_add_flag(g_reader_text, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(g_reader_text, ebook_reader_pointer_cb, LV_EVENT_PRESSED, nullptr);
            lv_obj_add_event_cb(g_reader_text, ebook_reader_pointer_cb, LV_EVENT_RELEASED, nullptr);
            lv_obj_add_event_cb(g_reader_text, ebook_reader_pointer_cb, LV_EVENT_PRESS_LOST, nullptr);
        }

        g_reader_overlay = lv_obj_create(g_reader_host);
        if (g_reader_overlay != nullptr) {
            ui_common_lock_object(g_reader_overlay);
            lv_obj_set_pos(g_reader_overlay, 0, 0);
            lv_obj_set_size(g_reader_overlay, 460, 460);
            lv_obj_set_style_radius(g_reader_overlay, 0, 0);
            lv_obj_set_style_border_width(g_reader_overlay, 0, 0);
            lv_obj_set_style_pad_all(g_reader_overlay, 0, 0);
            // 带标签视图不使用全屏遮罩；正文通过缩小真实布局区域避开顶部/底部标签。
            lv_obj_set_style_bg_opa(g_reader_overlay, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(g_reader_overlay, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(g_reader_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_event_cb(g_reader_overlay, ebook_reader_overlay_tap_cb, LV_EVENT_CLICKED, nullptr);

            lv_obj_t *reader_back = lv_button_create(g_reader_overlay);
            if (reader_back != nullptr) {
                ui_common_lock_object(reader_back);
                lv_obj_set_pos(reader_back, 12, 12);
                lv_obj_set_size(reader_back, 64, 44);
                ebook_expand_click_area(reader_back, kReaderTopTouchExpandPx);
                lv_obj_set_style_radius(reader_back, 20, 0);
                lv_obj_set_style_border_width(reader_back, 0, 0);
                lv_obj_set_style_bg_color(reader_back, lv_color_hex(0x151B24), 0);
                lv_obj_add_event_cb(reader_back, ebook_reader_back_button_cb, LV_EVENT_CLICKED, nullptr);
                lv_obj_t *text = ebook_make_label(reader_back, "返回", 0xEEF1F5, LV_TEXT_ALIGN_CENTER);
                if (text != nullptr) lv_obj_center(text);
            }

            g_reader_bookmark = lv_button_create(g_reader_overlay);
            if (g_reader_bookmark != nullptr) {
                ui_common_lock_object(g_reader_bookmark);
                lv_obj_set_pos(g_reader_bookmark, 96, 12);
                lv_obj_set_size(g_reader_bookmark, 96, 44);
                ebook_expand_click_area(g_reader_bookmark, kReaderTopTouchExpandPx);
                lv_obj_set_style_radius(g_reader_bookmark, 20, 0);
                lv_obj_set_style_border_width(g_reader_bookmark, 0, 0);
                lv_obj_set_style_bg_color(g_reader_bookmark, lv_color_hex(0x151B24), 0);
                lv_obj_add_event_cb(g_reader_bookmark, ebook_reader_bookmark_button_cb, LV_EVENT_CLICKED, nullptr);
                g_reader_bookmark_label = ebook_make_label(
                    g_reader_bookmark, "存书签", 0xF2E5B6, LV_TEXT_ALIGN_CENTER);
                if (g_reader_bookmark_label != nullptr) lv_obj_center(g_reader_bookmark_label);
            }

            g_reader_view = lv_button_create(g_reader_overlay);
            if (g_reader_view != nullptr) {
                ui_common_lock_object(g_reader_view);
                lv_obj_set_pos(g_reader_view, 212, 12);
                lv_obj_set_size(g_reader_view, 96, 44);
                ebook_expand_click_area(g_reader_view, kReaderTopTouchExpandPx);
                lv_obj_set_style_radius(g_reader_view, 20, 0);
                lv_obj_set_style_border_width(g_reader_view, 0, 0);
                lv_obj_set_style_bg_color(g_reader_view, lv_color_hex(0x151B24), 0);
                lv_obj_add_event_cb(g_reader_view, ebook_reader_view_button_cb, LV_EVENT_CLICKED, nullptr);
                g_reader_view_label = ebook_make_label(
                    g_reader_view, "全屏", 0xD7E8FF, LV_TEXT_ALIGN_CENTER);
                if (g_reader_view_label != nullptr) lv_obj_center(g_reader_view_label);
            }

            g_reader_music = lv_button_create(g_reader_overlay);
            if (g_reader_music != nullptr) {
                ui_common_lock_object(g_reader_music);
                lv_obj_set_pos(g_reader_music, 328, 12);
                lv_obj_set_size(g_reader_music, 120, 44);
                ebook_expand_click_area(g_reader_music, kReaderTopTouchExpandPx);
                lv_obj_set_style_radius(g_reader_music, 20, 0);
                lv_obj_set_style_border_width(g_reader_music, 0, 0);
                lv_obj_set_style_bg_color(g_reader_music, lv_color_hex(0x151B24), 0);
                lv_obj_add_event_cb(g_reader_music, ebook_reader_music_button_cb, LV_EVENT_CLICKED, nullptr);
                g_reader_music_label = ebook_make_label(
                    g_reader_music, "暂停音乐", 0xCFE0FF, LV_TEXT_ALIGN_CENTER);
                if (g_reader_music_label != nullptr) lv_obj_center(g_reader_music_label);
            }

            g_reader_prev = lv_button_create(g_reader_overlay);
            if (g_reader_prev != nullptr) {
                ui_common_lock_object(g_reader_prev);
                lv_obj_set_size(g_reader_prev, 82, 42);
                lv_obj_align(g_reader_prev, LV_ALIGN_BOTTOM_LEFT, 14, -12);
                ebook_expand_click_area(g_reader_prev, kReaderPageTouchExpandPx);
                lv_obj_set_style_radius(g_reader_prev, 18, 0);
                lv_obj_set_style_bg_color(g_reader_prev, lv_color_hex(0x151B24), 0);
                lv_obj_set_style_border_width(g_reader_prev, 0, 0);
                lv_obj_add_event_cb(g_reader_prev, ebook_reader_prev_button_cb, LV_EVENT_CLICKED, nullptr);
                lv_obj_t *text = ebook_make_label(g_reader_prev, "上页", 0xE7EBF1, LV_TEXT_ALIGN_CENTER);
                if (text != nullptr) lv_obj_center(text);
            }

            g_reader_next = lv_button_create(g_reader_overlay);
            if (g_reader_next != nullptr) {
                ui_common_lock_object(g_reader_next);
                lv_obj_set_size(g_reader_next, 82, 42);
                lv_obj_align(g_reader_next, LV_ALIGN_BOTTOM_RIGHT, -14, -12);
                ebook_expand_click_area(g_reader_next, kReaderPageTouchExpandPx);
                lv_obj_set_style_radius(g_reader_next, 18, 0);
                lv_obj_set_style_bg_color(g_reader_next, lv_color_hex(0x151B24), 0);
                lv_obj_set_style_border_width(g_reader_next, 0, 0);
                lv_obj_add_event_cb(g_reader_next, ebook_reader_next_button_cb, LV_EVENT_CLICKED, nullptr);
                lv_obj_t *text = ebook_make_label(g_reader_next, "下页", 0xE7EBF1, LV_TEXT_ALIGN_CENTER);
                if (text != nullptr) lv_obj_center(text);
            }

            g_reader_page_info = ebook_make_label(
                g_reader_overlay, "第 1 页", 0xA8B4C3, LV_TEXT_ALIGN_CENTER);
            if (g_reader_page_info != nullptr) {
                lv_obj_set_width(g_reader_page_info, 250);
                lv_obj_align(g_reader_page_info, LV_ALIGN_BOTTOM_MID, 0, -20);
            }
        }
    }

    // Ebook 与 Music 共用 GestureRouter 的底边手势；只增加一个 LVGL 20ms 轻量消费 timer，
    // 不新增 FreeRTOS Task。create 阶段先暂停，直到 App Manager 完成前台切换后由 enter() 恢复。
    g_gesture_timer = lv_timer_create(ebook_gesture_timer_cb, 20, nullptr);
    if (g_gesture_timer != nullptr) {
        lv_timer_pause(g_gesture_timer);
    }
    g_page_index_timer = lv_timer_create(
        ebook_page_index_timer_cb, kPageIndexBuildPeriodMs, nullptr);
    if (g_page_index_timer != nullptr) {
        lv_timer_pause(g_page_index_timer);
    }

    if (g_gesture_timer == nullptr || g_page_index_timer == nullptr ||
        g_browser_host == nullptr || browser_rows_ret != ESP_OK || g_browser_position == nullptr ||
        g_reader_host == nullptr ||
        g_header_back == nullptr || g_header_title == nullptr || g_header_line == nullptr ||
        g_reader_text == nullptr || g_reader_overlay == nullptr ||
        g_reader_page_info == nullptr || g_reader_prev == nullptr || g_reader_next == nullptr ||
        g_reader_bookmark == nullptr || g_reader_bookmark_label == nullptr ||
        g_reader_view == nullptr || g_reader_view_label == nullptr ||
        g_reader_music == nullptr || g_reader_music_label == nullptr) {
        if (g_gesture_timer != nullptr) {
            lv_timer_delete(g_gesture_timer);
            g_gesture_timer = nullptr;
        }
        if (g_page_index_timer != nullptr) {
            lv_timer_delete(g_page_index_timer);
            g_page_index_timer = nullptr;
        }
        lv_obj_delete(g_root);
        g_root = nullptr;
        g_header_back = nullptr;
        g_header_title = nullptr;
        g_header_line = nullptr;
        g_browser_host = nullptr;
        g_browser_status = nullptr;
        ebook_reset_browser_virtual_ui_refs();
        g_reader_host = nullptr;
        g_reader_text = nullptr;
        g_reader_overlay = nullptr;
        g_reader_page_info = nullptr;
        g_reader_prev = nullptr;
        g_reader_next = nullptr;
        g_reader_bookmark = nullptr;
        g_reader_bookmark_label = nullptr;
        g_reader_view = nullptr;
        g_reader_view_label = nullptr;
        g_reader_music = nullptr;
        g_reader_music_label = nullptr;
        if (restore_invalidation) lv_display_enable_invalidation(display, true);
        heap_caps_free(g_current_dir);
        heap_caps_free(g_scratch_path);
        heap_caps_free(g_book_path);
        g_current_dir = nullptr;
        g_scratch_path = nullptr;
        g_book_path = nullptr;
        return ESP_ERR_NO_MEM;
    }

    if (restore_invalidation) lv_display_enable_invalidation(display, true);
    ESP_LOGI(TAG, "Ebook create完成：Virtual Browser 5-Row + Cooperative QoS + 双视图 Reader + Builder V2 + Shadow Index + PageIndex Cache + 自动书签");
    return ESP_OK;
}

static esp_err_t ebook_enter()
{
    if (g_root == nullptr || g_current_dir == nullptr) return ESP_ERR_INVALID_STATE;
    g_press_tracking = false;
    gesture_router_reset();
    if (g_gesture_timer != nullptr) {
        lv_timer_reset(g_gesture_timer);
        lv_timer_resume(g_gesture_timer);
    }

    ebook_show_browser();
    if (!sdcard_is_mounted()) {
        ebook_cancel_browser_load();
        EbookReader::release_directory(&g_directory);
        g_browser_first_index = 0;
        ebook_browser_show_status("TF卡未挂载\nReader不可用", 0xD08C8C);
    } else {
        ebook_schedule_current_directory_load();
    }
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    lv_obj_invalidate(g_root);
    ESP_LOGI(TAG, "Ebook进入Foreground：/txt Browser已显示，Music可继续后台播放");
    return ESP_OK;
}

static esp_err_t ebook_leave(AppRunState next_state)
{
    if (next_state != AppRunState::Stopped) {
        ESP_LOGW(TAG, "Ebook不支持Background：next=%s", app_manager_state_name(next_state));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (g_page == EbookPage::Reader) {
        ebook_save_current_position("切出Ebook", false);
    }
    ebook_cancel_page_index_builder();
    ebook_cancel_shadow_warmup();
    ebook_cancel_browser_load();
    ebook_flush_page_index_caches(true);
    g_press_tracking = false;
    if (g_gesture_timer != nullptr) {
        lv_timer_pause(g_gesture_timer);
    }
    gesture_router_reset();
    app_launcher_overlay_hide();
    if (g_root != nullptr) lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Ebook离开Foreground：准备destroy Reader私有资源");
    return ESP_OK;
}

static void ebook_destroy()
{
    if (g_page == EbookPage::Reader) {
        ebook_save_current_position("销毁Ebook", false);
    }
    ebook_cancel_page_index_builder();
    ebook_cancel_shadow_warmup();
    ebook_cancel_browser_load();
    ebook_flush_page_index_caches(true);
    g_press_tracking = false;
    if (g_gesture_timer != nullptr) {
        lv_timer_delete(g_gesture_timer);
        g_gesture_timer = nullptr;
    }
    if (g_page_index_timer != nullptr) {
        lv_timer_delete(g_page_index_timer);
        g_page_index_timer = nullptr;
    }
    app_launcher_overlay_destroy();
    EbookReader::release_directory(&g_directory);
    ebook_release_page_indexes();

    g_header_back = nullptr;
    g_header_title = nullptr;
    g_header_line = nullptr;
    g_browser_host = nullptr;
    g_browser_status = nullptr;
    ebook_reset_browser_virtual_ui_refs();
    g_reader_host = nullptr;
    g_reader_text = nullptr;
    g_reader_overlay = nullptr;
    g_reader_page_info = nullptr;
    g_reader_prev = nullptr;
    g_reader_next = nullptr;
    g_reader_bookmark = nullptr;
    g_reader_bookmark_label = nullptr;
    g_reader_view = nullptr;
    g_reader_view_label = nullptr;
    g_reader_music = nullptr;
    g_reader_music_label = nullptr;
    g_reader_overlay_visible = false;
    g_reader_view_mode = ReaderViewMode::Labeled;
    g_read_anchor_offset = 0;
    g_current_page_ordinal = 0;
    g_reader_page_loaded = false;
    g_reader_position_dirty = false;
    g_bookmark_valid = false;
    g_bookmark_offset = 0;
    if (g_root != nullptr) {
        lv_obj_delete(g_root);
        g_root = nullptr;
    }
    if (g_current_dir != nullptr) {
        heap_caps_free(g_current_dir);
        g_current_dir = nullptr;
    }
    if (g_scratch_path != nullptr) {
        heap_caps_free(g_scratch_path);
        g_scratch_path = nullptr;
    }
    if (g_book_path != nullptr) {
        heap_caps_free(g_book_path);
        g_book_path = nullptr;
    }
    g_page = EbookPage::Browser;
    ESP_LOGI(TAG, "Ebook destroy完成：LVGL/目录/分页历史/路径PSRAM已释放");
}

} // namespace

esp_err_t ebook_app_register()
{
    AppDescriptor descriptor = {};
    descriptor.id = AppId::Ebook;
    descriptor.name = "电子书";
    descriptor.supports_background = false;
    descriptor.lifecycle.create = ebook_create;
    descriptor.lifecycle.enter = ebook_enter;
    descriptor.lifecycle.leave = ebook_leave;
    descriptor.lifecycle.destroy = ebook_destroy;

    const esp_err_t ret = app_manager_register(descriptor);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Ebook APP已注册：双视图规范分页 + 自动书签 + 后台音乐 + 共享GestureRouter圆环Launcher");
    }
    return ret;
}
