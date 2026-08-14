#include "ebook_app.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "ebook_reader_model.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "font/font_manager.h"
#include "gesture/gesture_router.h"
#include "lvgl.h"
#include "sdcard.h"
#include "ui_common.h"

static const char *TAG = "EbookAPP";

namespace {

enum class EbookPage : uint8_t
{
    Browser = 0,
    Reader,
};

static constexpr int16_t kBackSwipeThresholdPx = 72;
static constexpr int16_t kBackSwipeVerticalTolerancePx = 64;
static constexpr int32_t kHeaderHeight = 68;
static constexpr int32_t kContentMargin = 12;
static constexpr int32_t kRowHeight = 58;
static constexpr int16_t kPageSwipeThresholdPx = 72;
static constexpr int16_t kPageSwipeVerticalTolerancePx = 72;
static constexpr int32_t kReaderMarginX = 12;
static constexpr int32_t kReaderTextHeight = 314;
static constexpr int32_t kReaderLineSpace = 3;
static constexpr size_t kInitialHistoryCapacity = 64;

static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_header_back = nullptr;
static lv_obj_t *g_header_title = nullptr;
static lv_obj_t *g_browser_host = nullptr;
static lv_obj_t *g_browser_status = nullptr;
static lv_obj_t *g_reader_host = nullptr;
static lv_obj_t *g_reader_text = nullptr;
static lv_obj_t *g_reader_page_info = nullptr;
static lv_obj_t *g_reader_prev = nullptr;
static lv_obj_t *g_reader_next = nullptr;

static bool g_press_tracking = false;
static lv_point_t g_press_start = {};
static EbookPage g_page = EbookPage::Browser;
static char *g_current_dir = nullptr; // PSRAM
static char *g_scratch_path = nullptr; // PSRAM
static char *g_book_path = nullptr; // PSRAM
static EbookReader::DirectorySnapshot g_directory = {};
static uint64_t *g_page_history = nullptr; // PSRAM，仅保存已经走过的页起点
static size_t g_page_history_count = 0;
static size_t g_page_history_capacity = 0;
static uint64_t g_page_start = 0;
static uint64_t g_page_next = 0;
static uint64_t g_book_size = 0;
static bool g_page_at_end = false;

static const char *ebook_basename(const char *path)
{
    if (path == nullptr || path[0] == '\0') return "电子书";
    const char *slash = strrchr(path, '/');
    return (slash != nullptr && slash[1] != '\0') ? slash + 1 : path;
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

static void ebook_request_music()
{
    gesture_router_reset();
    const esp_err_t ret = app_manager_request_foreground(
        AppId::Music, AppTransitionMode::Exclusive);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "返回Music失败：%s", esp_err_to_name(ret));
    }
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

static void ebook_release_page_history()
{
    if (g_page_history != nullptr) heap_caps_free(g_page_history);
    g_page_history = nullptr;
    g_page_history_count = 0;
    g_page_history_capacity = 0;
}

static void ebook_reset_reader_session()
{
    ebook_release_page_history();
    g_page_start = 0;
    g_page_next = 0;
    g_book_size = 0;
    g_page_at_end = false;
    if (g_book_path != nullptr) g_book_path[0] = '\0';
}

static bool ebook_history_push(uint64_t offset)
{
    if (g_page_history_count >= g_page_history_capacity) {
        const size_t next_capacity = g_page_history_capacity == 0
            ? kInitialHistoryCapacity
            : g_page_history_capacity * 2U;
        uint64_t *next = static_cast<uint64_t *>(heap_caps_malloc(
            next_capacity * sizeof(uint64_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (next == nullptr) return false;
        if (g_page_history != nullptr && g_page_history_count > 0) {
            memcpy(next, g_page_history, g_page_history_count * sizeof(uint64_t));
            heap_caps_free(g_page_history);
        }
        g_page_history = next;
        g_page_history_capacity = next_capacity;
    }
    g_page_history[g_page_history_count++] = offset;
    return true;
}

static void ebook_show_browser()
{
    g_page = EbookPage::Browser;
    ebook_reset_reader_session();
    if (g_browser_host != nullptr) lv_obj_remove_flag(g_browser_host, LV_OBJ_FLAG_HIDDEN);
    if (g_reader_host != nullptr) lv_obj_add_flag(g_reader_host, LV_OBJ_FLAG_HIDDEN);
    if (g_reader_text != nullptr) lv_label_set_text(g_reader_text, "");
    ebook_update_header();
}

static void ebook_show_reader()
{
    g_page = EbookPage::Reader;
    if (g_browser_host != nullptr) lv_obj_add_flag(g_browser_host, LV_OBJ_FLAG_HIDDEN);
    if (g_reader_host != nullptr) lv_obj_remove_flag(g_reader_host, LV_OBJ_FLAG_HIDDEN);
    ebook_update_header();
}

static void ebook_update_reader_controls()
{
    if (g_reader_page_info != nullptr) {
        char info[96] = {};
        const uint64_t progress = g_book_size > 0
            ? ((g_page_next > g_book_size ? g_book_size : g_page_next) * 100U) / g_book_size
            : 0;
        snprintf(info, sizeof(info), "第 %u 页 · %llu%%",
            static_cast<unsigned>(g_page_history_count + 1U),
            static_cast<unsigned long long>(progress));
        lv_label_set_text(g_reader_page_info, info);
    }
    if (g_reader_prev != nullptr) {
        lv_obj_set_style_opa(g_reader_prev,
            g_page_history_count > 0 ? LV_OPA_COVER : LV_OPA_40, 0);
    }
    if (g_reader_next != nullptr) {
        lv_obj_set_style_opa(g_reader_next,
            !g_page_at_end ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

static uint16_t ebook_reader_glyph_width(uint32_t codepoint, uint32_t next_codepoint)
{
    const lv_font_t *font = font_manager_get_ui_font();
    if (font == nullptr || font->get_glyph_dsc == nullptr) {
        return codepoint < 0x80U ? 12U : 24U;
    }
    return lv_font_get_glyph_width(font, codepoint, next_codepoint);
}

static EbookReader::PageLayout ebook_reader_page_layout()
{
    EbookReader::PageLayout layout = {};
    layout.text_width_px = static_cast<uint16_t>(460 - kReaderMarginX * 2);
    layout.glyph_width = ebook_reader_glyph_width;

    const lv_font_t *font = font_manager_get_ui_font();
    const int32_t line_height = font != nullptr ? font->line_height : 32;
    const int32_t stride = line_height + kReaderLineSpace;
    int32_t lines = stride > 0
        ? (kReaderTextHeight + kReaderLineSpace) / stride
        : 1;
    if (lines < 1) lines = 1;
    if (lines > 32) lines = 32;
    layout.max_lines = static_cast<uint8_t>(lines);
    return layout;
}

static esp_err_t ebook_load_page(uint64_t offset)
{
    if (g_book_path == nullptr || g_book_path[0] == '\0' || g_reader_text == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    EbookReader::TextPage page = {};
    const EbookReader::PageLayout layout = ebook_reader_page_layout();
    const esp_err_t ret = EbookReader::load_text_page(g_book_path, offset, layout, &page);
    if (ret != ESP_OK) return ret;

    lv_label_set_text(g_reader_text, page.text != nullptr ? page.text : "");
    g_page_start = page.start_offset;
    g_page_next = page.next_offset;
    g_book_size = page.file_size;
    g_page_at_end = page.at_end;
    EbookReader::release_text_page(&page);
    ebook_update_reader_controls();
    return ESP_OK;
}

static void ebook_reader_next_page()
{
    if (g_page != EbookPage::Reader || g_page_at_end || g_page_next <= g_page_start) return;
    const uint64_t next_offset = g_page_next;
    if (!ebook_history_push(g_page_start)) {
        ESP_LOGE(TAG, "Reader页历史申请PSRAM失败");
        return;
    }
    const esp_err_t ret = ebook_load_page(next_offset);
    if (ret != ESP_OK) {
        if (g_page_history_count > 0) --g_page_history_count;
        ESP_LOGW(TAG, "TXT下一页失败：offset=%llu ret=%s",
            static_cast<unsigned long long>(next_offset), esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "TXT翻页：page=%u offset=%llu/%llu",
        static_cast<unsigned>(g_page_history_count + 1U),
        static_cast<unsigned long long>(g_page_start),
        static_cast<unsigned long long>(g_book_size));
}

static void ebook_reader_previous_page()
{
    if (g_page != EbookPage::Reader) return;
    if (g_page_history_count == 0) {
        ebook_show_browser();
        return;
    }
    const uint64_t previous_offset = g_page_history[g_page_history_count - 1U];
    --g_page_history_count;
    const esp_err_t ret = ebook_load_page(previous_offset);
    if (ret != ESP_OK) {
        ++g_page_history_count;
        ESP_LOGW(TAG, "TXT上一页失败：offset=%llu ret=%s",
            static_cast<unsigned long long>(previous_offset), esp_err_to_name(ret));
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
    const esp_err_t ret = ebook_load_page(0);
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
    const EbookReader::PageLayout layout = ebook_reader_page_layout();
    ESP_LOGI(TAG, "TXT小说重排打开：%s bytes=%llu width=%u lines=%u first_next=%llu",
        entry.name,
        static_cast<unsigned long long>(g_book_size),
        static_cast<unsigned>(layout.text_width_px),
        static_cast<unsigned>(layout.max_lines),
        static_cast<unsigned long long>(g_page_next));

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
    ebook_request_music();
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
    if (abs(dy) > kPageSwipeVerticalTolerancePx || abs(dx) <= abs(dy)) return;
    if (dx <= -kPageSwipeThresholdPx) ebook_reader_next_page();
    else if (dx >= kPageSwipeThresholdPx) ebook_reader_previous_page();
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

    lv_obj_t *header_line = lv_obj_create(g_root);
    if (header_line != nullptr) {
        ui_common_lock_object(header_line);
        lv_obj_set_size(header_line, 390, 1);
        lv_obj_align(header_line, LV_ALIGN_TOP_MID, 0, kHeaderHeight - 1);
        lv_obj_set_style_border_width(header_line, 0, 0);
        lv_obj_set_style_bg_color(header_line, lv_color_hex(0x1C2430), 0);
        lv_obj_set_style_bg_opa(header_line, LV_OPA_COVER, 0);
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

    g_reader_host = lv_obj_create(g_root);
    if (g_reader_host != nullptr) {
        lv_obj_set_pos(g_reader_host, kReaderMarginX, kHeaderHeight + 8);
        lv_obj_set_size(g_reader_host, 460 - kReaderMarginX * 2, 460 - (kHeaderHeight + 8) - 14);
        lv_obj_set_style_radius(g_reader_host, 0, 0);
        lv_obj_set_style_border_width(g_reader_host, 0, 0);
        lv_obj_set_style_bg_opa(g_reader_host, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(g_reader_host, 0, 0);
        lv_obj_remove_flag(g_reader_host, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(g_reader_host, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(g_reader_host, ebook_reader_pointer_cb, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(g_reader_host, ebook_reader_pointer_cb, LV_EVENT_RELEASED, nullptr);
        lv_obj_add_event_cb(g_reader_host, ebook_reader_pointer_cb, LV_EVENT_PRESS_LOST, nullptr);

        g_reader_text = ebook_make_label(g_reader_host, "", 0xE8EBEF, LV_TEXT_ALIGN_LEFT);
        if (g_reader_text != nullptr) {
            lv_obj_set_pos(g_reader_text, 0, 4);
            lv_obj_set_size(g_reader_text, 460 - kReaderMarginX * 2, kReaderTextHeight);
            lv_label_set_long_mode(g_reader_text, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(g_reader_text, kReaderLineSpace, 0);
        }

        g_reader_prev = lv_button_create(g_reader_host);
        if (g_reader_prev != nullptr) {
            ui_common_lock_object(g_reader_prev);
            lv_obj_set_size(g_reader_prev, 66, 42);
            lv_obj_align(g_reader_prev, LV_ALIGN_BOTTOM_LEFT, 6, -2);
            lv_obj_set_style_radius(g_reader_prev, 18, 0);
            lv_obj_set_style_bg_color(g_reader_prev, lv_color_hex(0x151B24), 0);
            lv_obj_set_style_border_width(g_reader_prev, 0, 0);
            lv_obj_add_event_cb(g_reader_prev, ebook_reader_prev_button_cb, LV_EVENT_CLICKED, nullptr);
            lv_obj_t *text = ebook_make_label(g_reader_prev, "<", 0xE7EBF1, LV_TEXT_ALIGN_CENTER);
            if (text != nullptr) lv_obj_center(text);
        }

        g_reader_next = lv_button_create(g_reader_host);
        if (g_reader_next != nullptr) {
            ui_common_lock_object(g_reader_next);
            lv_obj_set_size(g_reader_next, 66, 42);
            lv_obj_align(g_reader_next, LV_ALIGN_BOTTOM_RIGHT, -6, -2);
            lv_obj_set_style_radius(g_reader_next, 18, 0);
            lv_obj_set_style_bg_color(g_reader_next, lv_color_hex(0x151B24), 0);
            lv_obj_set_style_border_width(g_reader_next, 0, 0);
            lv_obj_add_event_cb(g_reader_next, ebook_reader_next_button_cb, LV_EVENT_CLICKED, nullptr);
            lv_obj_t *text = ebook_make_label(g_reader_next, ">", 0xE7EBF1, LV_TEXT_ALIGN_CENTER);
            if (text != nullptr) lv_obj_center(text);
        }

        g_reader_page_info = ebook_make_label(g_reader_host, "第 1 页", 0x657385, LV_TEXT_ALIGN_CENTER);
        if (g_reader_page_info != nullptr) {
            lv_obj_set_width(g_reader_page_info, 240);
            lv_obj_align(g_reader_page_info, LV_ALIGN_BOTTOM_MID, 0, -12);
        }
    }

    if (g_browser_host == nullptr || g_reader_host == nullptr ||
        g_header_back == nullptr || g_header_title == nullptr ||
        g_reader_text == nullptr || g_reader_page_info == nullptr ||
        g_reader_prev == nullptr || g_reader_next == nullptr) {
        lv_obj_delete(g_root);
        g_root = nullptr;
        g_header_back = nullptr;
        g_header_title = nullptr;
        g_browser_host = nullptr;
        g_reader_host = nullptr;
        g_reader_text = nullptr;
        g_reader_page_info = nullptr;
        g_reader_prev = nullptr;
        g_reader_next = nullptr;
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
    ESP_LOGI(TAG, "Ebook create完成：Browser + 小说智能重排分页 Reader UI已建立（Hidden）");
    return ESP_OK;
}

static esp_err_t ebook_enter()
{
    if (g_root == nullptr || g_current_dir == nullptr) return ESP_ERR_INVALID_STATE;
    g_press_tracking = false;
    gesture_router_reset();

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
    g_press_tracking = false;
    gesture_router_reset();
    if (g_root != nullptr) lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Ebook离开Foreground：准备destroy Reader私有资源");
    return ESP_OK;
}

static void ebook_destroy()
{
    g_press_tracking = false;
    EbookReader::release_directory(&g_directory);
    ebook_release_page_history();

    g_header_back = nullptr;
    g_header_title = nullptr;
    g_browser_host = nullptr;
    g_browser_status = nullptr;
    g_reader_host = nullptr;
    g_reader_text = nullptr;
    g_reader_page_info = nullptr;
    g_reader_prev = nullptr;
    g_reader_next = nullptr;
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
        ESP_LOGI(TAG, "Ebook APP已注册：/BOOKS 浏览 + UTF-8 TXT 小说智能重排 Reader");
    }
    return ret;
}
