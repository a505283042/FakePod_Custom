#include "ebook_app.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "audio/audio_service.h"
#include "ebook_bookmark_store.h"
#include "ebook_reader_model.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "font/font_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gesture/gesture_router.h"
#include "lvgl.h"
#include "player/player_control.h"
#include "app_launcher_overlay.h"
#include "sdcard.h"
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

static constexpr int16_t kBackSwipeThresholdPx = 72;
static constexpr int16_t kBackSwipeVerticalTolerancePx = 64;
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

static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_header_back = nullptr;
static lv_obj_t *g_header_title = nullptr;
static lv_obj_t *g_header_line = nullptr;
static lv_obj_t *g_browser_host = nullptr;
static lv_obj_t *g_browser_status = nullptr;
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

static bool g_press_tracking = false;
static lv_point_t g_press_start = {};
static EbookPage g_page = EbookPage::Browser;
static char *g_current_dir = nullptr; // PSRAM
static char *g_scratch_path = nullptr; // PSRAM
static char *g_book_path = nullptr; // PSRAM
static EbookReader::DirectorySnapshot g_directory = {};

// 两种视图各自维护一套“从书首开始”的规范页边界索引。
// 书签只保存内容 byte offset；切换视图时在目标索引中定位“包含该 offset 的完整页”，
// 不再制造截断半页，因此上一页/下一页始终是该视图自己的完整自然页。
struct ReaderPageIndex
{
    uint64_t *starts = nullptr; // PSRAM，每项8B
    size_t count = 0;
    size_t capacity = 0;
};
static ReaderPageIndex g_page_indexes[2] = {};
static size_t g_current_page_ordinal = 0;
// 真实阅读锚点与当前显示页起点分离。跨视图重排时显示页可以从锚点之前开始，
// 但只要用户没有真正翻页，自动书签仍保存这个内容锚点。
static uint64_t g_read_anchor_offset = 0;
static uint64_t g_page_start = 0;
static uint64_t g_page_next = 0;
static uint64_t g_book_size = 0;
static bool g_page_at_end = false;
static bool g_reader_overlay_visible = false;
static ReaderViewMode g_reader_view_mode = ReaderViewMode::Fullscreen;
static bool g_reader_page_loaded = false;
static bool g_reader_position_dirty = false;
static bool g_bookmark_valid = false;
static uint64_t g_bookmark_offset = 0;


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

    UiGestureAction action = UiGestureAction::None;
    if (!gesture_router_take_action(&action)) {
        return;
    }

    // 与 Music Launcher 一致：圆环已经显示时，新的页面级手势只负责先收起圆环。
    if (app_launcher_overlay_is_visible()) {
        app_launcher_overlay_hide();
        return;
    }

    // Reader 的翻页仍由自己的点击区负责；这里只消费全局手势，避免动作残留到 Browser。
    if (g_page != EbookPage::Browser) {
        return;
    }

    if (action == UiGestureAction::PullUpFromBottom &&
        g_current_dir != nullptr &&
        strcmp(g_current_dir, EbookReader::kRootDirectory) == 0) {
        ESP_LOGI(TAG, "Ebook主页手势：%s -> 公共圆环Launcher",
            gesture_router_action_name(action));
        ebook_show_app_launcher();
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
static esp_err_t ebook_resolve_anchor_for_mode(
    ReaderViewMode mode, uint64_t anchor, size_t *out_ordinal);
static esp_err_t ebook_load_indexed_page(
    ReaderViewMode mode, size_t ordinal, uint64_t semantic_anchor);
static void ebook_release_page_indexes();
static void ebook_update_reader_controls();

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

    const uint64_t anchor = g_read_anchor_offset;
    const size_t previous_ordinal = g_current_page_ordinal;
    const uint64_t previous_anchor = g_read_anchor_offset;

    g_reader_view_mode = mode;
    ebook_apply_reader_view_chrome(mode);

    // 两种视图拥有独立的规范分页索引。阅读锚点是内容位置，不强迫它成为目标视图页首；
    // 目标视图显示“包含锚点的完整自然页”。这样页首可能因布局不同而变化，但不会产生
    // partial page/底部空白，也不会让上一页混入另一套页界。
    if (g_page == EbookPage::Reader && g_reader_page_loaded) {
        size_t target_ordinal = 0;
        esp_err_t ret = ebook_resolve_anchor_for_mode(mode, anchor, &target_ordinal);
        if (ret == ESP_OK) {
            ret = ebook_load_indexed_page(mode, target_ordinal, anchor);
        }
        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                "Reader视图重排完成：mode=%s anchor=%llu page=%u start=%llu next=%llu",
                mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
                static_cast<unsigned long long>(anchor),
                static_cast<unsigned>(g_current_page_ordinal + 1U),
                static_cast<unsigned long long>(g_page_start),
                static_cast<unsigned long long>(g_page_next));
            return;
        }

        ESP_LOGW(TAG, "Reader视图重排失败：mode=%s anchor=%llu ret=%s，恢复原视图",
            mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
            static_cast<unsigned long long>(anchor), esp_err_to_name(ret));
        g_reader_view_mode = previous_mode;
        ebook_apply_reader_view_chrome(previous_mode);
        const esp_err_t restore_ret = ebook_load_indexed_page(
            previous_mode, previous_ordinal, previous_anchor);
        if (restore_ret != ESP_OK) {
            ESP_LOGE(TAG, "Reader恢复原视图分页失败：%s", esp_err_to_name(restore_ret));
        }
    }
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
    g_reader_view_mode = ReaderViewMode::Fullscreen;
    if (g_book_path != nullptr) g_book_path[0] = '\0';
}

static void ebook_show_browser()
{
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
    ebook_update_header();
}

static void ebook_show_reader()
{
    g_page = EbookPage::Reader;
    ebook_set_browser_chrome_visible(false);
    if (g_browser_host != nullptr) lv_obj_add_flag(g_browser_host, LV_OBJ_FLAG_HIDDEN);
    if (g_reader_host != nullptr) {
        lv_obj_remove_flag(g_reader_host, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_reader_host);
    }
    ebook_set_reader_view_mode(ReaderViewMode::Fullscreen);
}

static void ebook_update_reader_controls()
{
    if (g_reader_page_info != nullptr) {
        char info[96] = {};
        // 阅读进度使用语义锚点，而不是当前视图页尾；切换全屏/带标签时百分比保持稳定。
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
        lv_obj_set_style_opa(g_reader_next,
            !g_page_at_end ? LV_OPA_COVER : LV_OPA_40, 0);
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

static esp_err_t ebook_resolve_anchor_for_mode(
    ReaderViewMode mode, uint64_t anchor, size_t *out_ordinal)
{
    if (out_ordinal == nullptr || g_book_path == nullptr || g_book_path[0] == '\0' ||
        g_book_size == 0 || anchor >= g_book_size) {
        return ESP_ERR_INVALID_ARG;
    }

    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    if (index->count == 0 && !ebook_page_index_push(index, 0)) {
        return ESP_ERR_NO_MEM;
    }

    // 已知索引范围内先二分定位；若 anchor 超过当前已知最后页首，再只向前扩展到包含它的页。
    if (anchor <= index->starts[index->count - 1U]) {
        size_t lo = 0;
        size_t hi = index->count;
        while (lo + 1U < hi) {
            const size_t mid = lo + (hi - lo) / 2U;
            if (index->starts[mid] <= anchor) lo = mid;
            else hi = mid;
        }
        *out_ordinal = lo;
        return ESP_OK;
    }

    size_t ordinal = index->count - 1U;
    ESP_LOGI(TAG, "Reader页索引扩展：mode=%s anchor=%llu from_page=%u",
        mode == ReaderViewMode::Labeled ? "labeled" : "fullscreen",
        static_cast<unsigned long long>(anchor),
        static_cast<unsigned>(ordinal + 1U));

    while (true) {
        const uint64_t start_offset = index->starts[ordinal];
        EbookReader::TextPage page = {};
        const esp_err_t ret = ebook_load_page_for_mode(mode, start_offset, &page);
        if (ret != ESP_OK) {
            EbookReader::release_text_page(&page);
            return ret;
        }
        const uint64_t next = page.next_offset;
        const bool at_end = page.at_end;
        EbookReader::release_text_page(&page);
        if (next <= start_offset) return ESP_ERR_INVALID_RESPONSE;

        if (anchor < next || at_end) {
            *out_ordinal = ordinal;
            return ESP_OK;
        }
        if (anchor == next) {
            if (at_end) {
                *out_ordinal = ordinal;
                return ESP_OK;
            }
            if (!ebook_page_index_push(index, next)) return ESP_ERR_NO_MEM;
            *out_ordinal = ordinal + 1U;
            return ESP_OK;
        }

        if (!ebook_page_index_push(index, next)) return ESP_ERR_NO_MEM;
        ++ordinal;
        taskYIELD();
    }
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

static esp_err_t ebook_ensure_next_page_index(ReaderViewMode mode, size_t ordinal)
{
    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    if (index->starts == nullptr || ordinal >= index->count) return ESP_ERR_INVALID_ARG;
    if (ordinal + 1U < index->count) return ESP_OK;

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
    if (g_page != EbookPage::Reader || g_page_at_end) return;
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
}

static void ebook_reader_previous_page()
{
    if (g_page != EbookPage::Reader || g_current_page_ordinal == 0) return;
    const ReaderViewMode mode = g_reader_view_mode;
    ReaderPageIndex *index = ebook_page_index_for_mode(mode);
    const size_t previous_ordinal = g_current_page_ordinal - 1U;
    if (index->starts == nullptr || previous_ordinal >= index->count) return;
    const uint64_t previous_start = index->starts[previous_ordinal];
    const esp_err_t ret = ebook_load_indexed_page(mode, previous_ordinal, previous_start);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TXT上一页失败：offset=%llu ret=%s",
            static_cast<unsigned long long>(previous_start), esp_err_to_name(ret));
    }
}

static const char *ebook_error_message(esp_err_t err)
{
    switch (err) {
        case ESP_ERR_NOT_FOUND:
            return "未找到 /BOOKS\n请在TF卡根目录创建 BOOKS 文件夹";
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

static esp_err_t ebook_scan_current_directory();

static void ebook_row_clicked_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        gesture_router_should_suppress_click() ||
        g_current_dir == nullptr || g_scratch_path == nullptr) {
        return;
    }

    const uintptr_t raw_index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (raw_index == 0) return;
    const size_t index = static_cast<size_t>(raw_index - 1U);
    if (index >= g_directory.count || g_directory.entries == nullptr) return;

    const EbookReader::Entry &entry = g_directory.entries[index];
    const esp_err_t join_ret = EbookReader::join_child_path(
        g_current_dir, entry.name, g_scratch_path, EbookReader::kPathBytes);
    if (join_ret != ESP_OK) {
        ebook_set_status("路径过长，无法进入");
        return;
    }

    if (entry.is_directory) {
        snprintf(g_current_dir, EbookReader::kPathBytes, "%s", g_scratch_path);
        const esp_err_t ret = ebook_scan_current_directory();
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
        g_book_path, entry.size_bytes, &initial_offset, &bookmark_found);
    if (bookmark_ret != ESP_OK) {
        ESP_LOGW(TAG, "TXT书签读取失败：path=%s ret=%s", g_book_path, esp_err_to_name(bookmark_ret));
        initial_offset = 0;
        bookmark_found = false;
    }
    g_bookmark_valid = bookmark_found;
    g_bookmark_offset = bookmark_found ? initial_offset : 0;

    g_book_size = entry.size_bytes;
    size_t initial_ordinal = 0;
    esp_err_t ret = ebook_resolve_anchor_for_mode(
        ReaderViewMode::Fullscreen, initial_offset, &initial_ordinal);
    if (ret == ESP_OK) {
        ret = ebook_load_indexed_page(
            ReaderViewMode::Fullscreen, initial_ordinal, initial_offset);
    }
    if (ret != ESP_OK && initial_offset != 0) {
        ESP_LOGW(TAG, "书签位置不可定位，回退首页并清理旧书签：offset=%llu ret=%s",
            static_cast<unsigned long long>(initial_offset), esp_err_to_name(ret));
        EbookBookmarkStore::clear(g_book_path);
        g_bookmark_valid = false;
        g_bookmark_offset = 0;
        bookmark_found = false;
        initial_offset = 0;
        ebook_release_page_indexes();
        if (!ebook_page_index_push(ebook_page_index_for_mode(ReaderViewMode::Fullscreen), 0)) {
            ret = ESP_ERR_NO_MEM;
        } else {
            ret = ebook_load_indexed_page(ReaderViewMode::Fullscreen, 0, 0);
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TXT打开失败：path=%s ret=%s", g_book_path, esp_err_to_name(ret));
        if (g_reader_text != nullptr) lv_label_set_text(g_reader_text, ebook_error_message(ret));
        g_page_start = 0;
        g_page_next = 0;
        g_book_size = entry.size_bytes;
        g_page_at_end = true;
        ebook_update_reader_controls();
        ebook_show_reader();
        return;
    }
    ebook_show_reader();
    ESP_LOGI(TAG, "TXT小说重排打开：%s bytes=%llu anchor=%llu start=%llu next=%llu bookmark=%s view=fullscreen",
        entry.name,
        static_cast<unsigned long long>(g_book_size),
        static_cast<unsigned long long>(g_read_anchor_offset),
        static_cast<unsigned long long>(g_page_start),
        static_cast<unsigned long long>(g_page_next),
        bookmark_found ? "YES" : "NO");

}

static void ebook_render_directory()
{
    if (g_browser_host == nullptr) return;
    lv_obj_clean(g_browser_host);
    g_browser_status = nullptr;

    if (g_directory.count == 0) {
        g_browser_status = ebook_make_label(
            g_browser_host,
            "这里还没有TXT文件\n支持子文件夹和 UTF-8 .txt",
            0x8490A0,
            LV_TEXT_ALIGN_CENTER);
        if (g_browser_status != nullptr) {
            lv_obj_set_width(g_browser_status, 360);
            lv_label_set_long_mode(g_browser_status, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(g_browser_status, 8, 0);
            lv_obj_align(g_browser_status, LV_ALIGN_TOP_MID, 0, 100);
        }
        ebook_update_header();
        return;
    }

    for (size_t i = 0; i < g_directory.count; ++i) {
        const EbookReader::Entry &entry = g_directory.entries[i];
        lv_obj_t *row = lv_button_create(g_browser_host);
        if (row == nullptr) break;
        ui_common_lock_object(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, kRowHeight);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(entry.is_directory ? 0x172230 : 0x151A21), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(row, 16, 0);
        lv_obj_set_style_pad_right(row, 14, 0);
        lv_obj_add_event_cb(
            row,
            ebook_row_clicked_cb,
            LV_EVENT_CLICKED,
            reinterpret_cast<void *>(static_cast<uintptr_t>(i + 1U)));

        lv_obj_t *name = ebook_make_label(
            row,
            entry.name,
            entry.is_directory ? 0xBCD7FF : 0xEEF1F5,
            LV_TEXT_ALIGN_LEFT);
        if (name != nullptr) {
            lv_obj_set_width(name, 344);
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        }

        lv_obj_t *arrow = ebook_make_label(
            row,
            entry.is_directory ? ">" : "TXT",
            entry.is_directory ? 0x6EA5F2 : 0x677385,
            LV_TEXT_ALIGN_RIGHT);
        if (arrow != nullptr) {
            lv_obj_set_width(arrow, 54);
            lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }

    if (g_directory.truncated) {
        lv_obj_t *hint = ebook_make_label(
            g_browser_host,
            "仅显示前48项",
            0x697687,
            LV_TEXT_ALIGN_CENTER);
        if (hint != nullptr) {
            lv_obj_set_width(hint, LV_PCT(100));
        }
    }
    lv_obj_scroll_to_y(g_browser_host, 0, LV_ANIM_OFF);
    ebook_update_header();
}

static esp_err_t ebook_scan_current_directory()
{
    if (g_current_dir == nullptr || g_browser_host == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    EbookReader::release_directory(&g_directory);
    const esp_err_t ret = EbookReader::scan_directory(g_current_dir, &g_directory);
    if (ret != ESP_OK) {
        lv_obj_clean(g_browser_host);
        g_browser_status = ebook_make_label(
            g_browser_host,
            ebook_error_message(ret),
            0xD08C8C,
            LV_TEXT_ALIGN_CENTER);
        if (g_browser_status != nullptr) {
            lv_obj_set_width(g_browser_status, 370);
            lv_label_set_long_mode(g_browser_status, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(g_browser_status, 8, 0);
            lv_obj_align(g_browser_status, LV_ALIGN_TOP_MID, 0, 100);
        }
        ebook_update_header();
        return ret;
    }
    ebook_render_directory();
    ESP_LOGI(TAG, "Ebook目录：%s items=%u truncated=%s",
        g_current_dir,
        static_cast<unsigned>(g_directory.count),
        g_directory.truncated ? "YES" : "NO");
    return ESP_OK;
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
        ebook_scan_current_directory();
        return;
    }
    ebook_show_app_launcher();
}

static void ebook_back_button_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    ebook_go_back();
}

static void ebook_root_pointer_cb(lv_event_t *event)
{
    if (event == nullptr || g_root == nullptr) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (g_page == EbookPage::Reader) return;
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
    if (dx >= kBackSwipeThresholdPx &&
        abs(dy) <= kBackSwipeVerticalTolerancePx &&
        dx > abs(dy)) {
        ESP_LOGI(TAG, "Ebook右滑返回一级：dx=%ld dy=%ld",
            static_cast<long>(dx), static_cast<long>(dy));
        ebook_go_back();
    }
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
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    ebook_save_current_position("手动书签", true);
}

static void ebook_reader_view_button_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED) return;
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
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
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
    if (event == nullptr || g_page != EbookPage::Reader) return;
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
    lv_obj_add_event_cb(g_root, ebook_root_pointer_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(g_root, ebook_root_pointer_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(g_root, ebook_root_pointer_cb, LV_EVENT_PRESS_LOST, nullptr);

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

    g_browser_host = lv_obj_create(g_root);
    if (g_browser_host != nullptr) {
        lv_obj_set_pos(g_browser_host, kContentMargin, kHeaderHeight + 8);
        lv_obj_set_size(g_browser_host,
            460 - kContentMargin * 2,
            460 - (kHeaderHeight + 8) - kContentMargin);
        lv_obj_set_style_radius(g_browser_host, 0, 0);
        lv_obj_set_style_border_width(g_browser_host, 0, 0);
        lv_obj_set_style_bg_opa(g_browser_host, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(g_browser_host, 0, 0);
        lv_obj_set_style_pad_row(g_browser_host, 8, 0);
        lv_obj_set_flex_flow(g_browser_host, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(g_browser_host, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(g_browser_host, LV_OBJ_FLAG_SCROLL_ELASTIC);
        lv_obj_remove_flag(g_browser_host, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
        lv_obj_set_scroll_dir(g_browser_host, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(g_browser_host, LV_SCROLLBAR_MODE_AUTO);
    }

    // Reader 真正占满 460x460；标题/返回/翻页/书签/后台音乐都进入按需 Overlay，
    // 默认只留下正文，单击正文再显示控制层。
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

    if (g_gesture_timer == nullptr ||
        g_browser_host == nullptr || g_reader_host == nullptr ||
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
        lv_obj_delete(g_root);
        g_root = nullptr;
        g_header_back = nullptr;
        g_header_title = nullptr;
        g_header_line = nullptr;
        g_browser_host = nullptr;
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
    ESP_LOGI(TAG, "Ebook create完成：Browser + 双视图 Reader + 自动书签 + 音乐控制 + 共享GestureRouter圆环手势");
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

    if (!sdcard_is_mounted()) {
        EbookReader::release_directory(&g_directory);
        lv_obj_clean(g_browser_host);
        g_browser_status = ebook_make_label(
            g_browser_host, "TF卡未挂载\nReader不可用", 0xD08C8C, LV_TEXT_ALIGN_CENTER);
        if (g_browser_status != nullptr) {
            lv_obj_set_width(g_browser_status, 360);
            lv_obj_align(g_browser_status, LV_ALIGN_TOP_MID, 0, 100);
        }
    } else {
        ebook_scan_current_directory();
    }
    ebook_show_browser();
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    lv_obj_invalidate(g_root);
    ESP_LOGI(TAG, "Ebook进入Foreground：/BOOKS Browser已显示，Music可继续后台播放");
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
    g_press_tracking = false;
    if (g_gesture_timer != nullptr) {
        lv_timer_delete(g_gesture_timer);
        g_gesture_timer = nullptr;
    }
    app_launcher_overlay_destroy();
    EbookReader::release_directory(&g_directory);
    ebook_release_page_indexes();

    g_header_back = nullptr;
    g_header_title = nullptr;
    g_header_line = nullptr;
    g_browser_host = nullptr;
    g_browser_status = nullptr;
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
    g_reader_view_mode = ReaderViewMode::Fullscreen;
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
