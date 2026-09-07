#include "settings_app.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "app_launcher_overlay.h"
#include "app_build_info.h"
#include "app_manager.h"
#include "audio_service.h"
#include "device_settings.h"
#include "font/font_manager.h"
#include "font/usb_service_font.h"
#include "gesture/gesture_router.h"
#include "persistent_state.h"
#include "player_control.h"
#include "player_state.h"
#include "sdcard.h"
#include "storage_io.h"
#include "usb_storage_service.h"
#include "artwork_loader.h"
#include "lyrics/lyrics_service.h"
#include "media_library.h"
#include "screen_lock_simple.h"
#include "settings_menu_icons.h"
#include "ui_common.h"

static const char *TAG = "设置APP";

namespace {

static constexpr uint32_t kBgRgb = 0x0A0D12;
static constexpr uint32_t kHeaderRgb = 0xF2F4F7;
static constexpr uint32_t kMutedRgb = 0x8A94A5;
static constexpr uint32_t kRowRgb = 0x141A23;
static constexpr uint32_t kRowBorderRgb = 0x202938;
static constexpr uint32_t kAccentRgb = 0x79B9FF;
static constexpr uint32_t kValueRgb = 0xBFD1E8;
static constexpr int16_t kHeaderHeight = 62;
static constexpr int16_t kMainGridTileWidth = 206;
static constexpr int16_t kMainGridTileHeight = 174;
static constexpr int16_t kMainGridGapX = 12;
static constexpr int16_t kMainGridGapY = 12;
static constexpr int16_t kMainGridWidth =
    kMainGridTileWidth * 2 + kMainGridGapX;
static constexpr int16_t kMainGridHeight =
    kMainGridTileHeight * 2 + kMainGridGapY;
static constexpr int16_t kContentX = 18;
static constexpr int16_t kContentWidth = 424;
static constexpr int16_t kContentTop = 68;
static constexpr int16_t kContentHeight = 360;
static constexpr int16_t kDetailRowHeight = 64;
static constexpr int16_t kDetailRowGap = 8;
static constexpr uint32_t kRefreshPeriodMs = 500U;
static constexpr int16_t kTapMovePx = 14;
static constexpr uint8_t kBrightnessMin = 5U;
static constexpr uint8_t kBrightnessMax = 100U;
static constexpr int16_t kBrightnessFullScalePx = 280;
static constexpr int32_t kDetailInertiaStartPxPerSec = 180;
static constexpr int32_t kDetailInertiaStopPxPerSec = 45;

enum class SettingsPage : uint8_t {
    Main = 0,
    Connection,
    Applications,
    System,
    About,
};

enum class SettingsCategory : uint8_t {
    Connection = 0,
    Applications,
    System,
    About,
    Count,
};

struct CategoryRow {
    SettingsCategory category;
    const char *title;
    SettingsMenuIcon icon;
};

static constexpr CategoryRow kCategories[] = {
    {SettingsCategory::Connection, "连接", SettingsMenuIcon::Connection},
    {SettingsCategory::Applications, "应用", SettingsMenuIcon::Applications},
    {SettingsCategory::System, "系统", SettingsMenuIcon::System},
    {SettingsCategory::About, "关于", SettingsMenuIcon::About},
};
static_assert(sizeof(kCategories) / sizeof(kCategories[0]) ==
    static_cast<size_t>(SettingsCategory::Count));

static bool g_usb_transition_pending = false;
static bool g_usb_return_safe_ui = false;
static lv_obj_t *g_usb_overlay = nullptr;
static lv_obj_t *g_usb_overlay_status = nullptr;
static lv_obj_t *g_usb_overlay_action = nullptr;

static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_header_back = nullptr;
static lv_obj_t *g_header_title = nullptr;
static lv_obj_t *g_header_line = nullptr;
static lv_obj_t *g_content = nullptr;
static lv_obj_t *g_detail_host = nullptr;
static lv_timer_t *g_timer = nullptr;
static lv_timer_t *g_refresh_timer = nullptr;
static SettingsPage g_page = SettingsPage::Main;
static lv_obj_t *g_detail_values[16] = {};
static SettingsPage g_pending_page = SettingsPage::Main;
static bool g_audio_mode_switching = false;
static lv_obj_t *g_brightness_card = nullptr;
static lv_obj_t *g_brightness_value = nullptr;
static bool g_brightness_adjust_armed = false;
static bool g_brightness_dragging = false;
static uint32_t g_brightness_drag_sequence = 0U;
static uint8_t g_brightness_drag_start = 60U;
static uint8_t g_brightness_saved_level = 60U;
static uint8_t g_brightness_pending_level = 60U;
static bool g_brightness_dirty = false;
static DeviceMusicListScope g_music_scope_saved = DeviceMusicListScope::All;
static DeviceMusicListScope g_music_scope_pending = DeviceMusicListScope::All;
static bool g_music_scope_dirty = false;
static int32_t g_detail_saved_scroll_y[5] = {};
static int32_t g_detail_scroll_y = 0;
static int32_t g_detail_scroll_max_y = 0;
static int32_t g_detail_drag_start_scroll_y = 0;
static int32_t g_detail_drag_last_scroll_y = 0;
static int32_t g_detail_scroll_velocity_px_s = 0;
static uint32_t g_detail_drag_sequence = 0U;
static uint32_t g_detail_drag_last_tick_ms = 0U;
static uint32_t g_detail_inertia_last_tick_ms = 0U;
static bool g_detail_inertia_active = false;

static void music_list_commit_on_exit();
static void settings_update_vertical_adjust_channel();

static void set_visible(lv_obj_t *obj, bool visible)
{
    if (obj == nullptr) return;
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_label(
    lv_obj_t *parent,
    const char *text,
    uint32_t rgb,
    lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == nullptr) return nullptr;
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_font(label, font_manager_get_ui_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static bool click_is_valid(lv_event_t *event)
{
    return event != nullptr && lv_event_get_code(event) == LV_EVENT_CLICKED &&
        !gesture_router_should_suppress_click() &&
        gesture_router_press_was_tap(kTapMovePx);
}

static const char *page_title(SettingsPage page)
{
    switch (page) {
        case SettingsPage::Main: return "设置";
        case SettingsPage::Connection: return "连接";
        case SettingsPage::Applications: return "应用";
        case SettingsPage::System: return "系统";
        case SettingsPage::About: return "关于";
        default: return "设置";
    }
}

static SettingsPage page_for_category(SettingsCategory category)
{
    switch (category) {
        case SettingsCategory::Connection: return SettingsPage::Connection;
        case SettingsCategory::Applications: return SettingsPage::Applications;
        case SettingsCategory::System: return SettingsPage::System;
        case SettingsCategory::About: return SettingsPage::About;
        default: return SettingsPage::Main;
    }
}

static SettingsPage parent_page(SettingsPage page)
{
    switch (page) {
        case SettingsPage::Connection:
        case SettingsPage::Applications:
        case SettingsPage::System:
        case SettingsPage::About:
            return SettingsPage::Main;
        case SettingsPage::Main:
        default:
            return SettingsPage::Main;
    }
}

static void clear_detail_value_refs()
{
    for (lv_obj_t *&label : g_detail_values) label = nullptr;
    g_detail_host = nullptr;
    g_brightness_card = nullptr;
    g_brightness_value = nullptr;
}

static size_t settings_page_index(SettingsPage page)
{
    const size_t index = static_cast<size_t>(page);
    return index < 5U ? index : 0U;
}

static lv_obj_t *detail_parent()
{
    return g_detail_host != nullptr ? g_detail_host : g_content;
}

static int32_t detail_clamp_scroll_y(int32_t y)
{
    if (y < 0) return 0;
    if (y > g_detail_scroll_max_y) return g_detail_scroll_max_y;
    return y;
}

static void detail_apply_scroll_y(int32_t y)
{
    if (g_detail_host == nullptr) return;
    const int32_t clamped = detail_clamp_scroll_y(y);
    g_detail_scroll_y = clamped;
    g_detail_saved_scroll_y[settings_page_index(g_page)] = clamped;
    lv_obj_set_y(g_detail_host, -clamped);
    lv_obj_invalidate(g_content);
}

static uint16_t detail_row_count_for_page(SettingsPage page)
{
    switch (page) {
        case SettingsPage::Connection: return 4U;
        case SettingsPage::Applications: return 5U;
        case SettingsPage::System: return 6U;
        case SettingsPage::About: return 5U;
        case SettingsPage::Main:
        default:
            return 0U;
    }
}

static void detail_prepare(SettingsPage page)
{
    g_detail_scroll_velocity_px_s = 0;
    g_detail_drag_sequence = 0U;
    g_detail_drag_last_tick_ms = 0U;
    g_detail_inertia_last_tick_ms = 0U;
    g_detail_inertia_active = false;
    g_detail_scroll_max_y = 0;
    g_detail_scroll_y = 0;

    const uint16_t row_count = detail_row_count_for_page(page);
    if (row_count == 0U || g_content == nullptr) return;

    const int32_t content_h = static_cast<int32_t>(row_count) *
        static_cast<int32_t>(kDetailRowHeight + kDetailRowGap) - kDetailRowGap;
    g_detail_scroll_max_y = content_h > kContentHeight ? content_h - kContentHeight : 0;
    g_detail_scroll_y = detail_clamp_scroll_y(g_detail_saved_scroll_y[settings_page_index(page)]);
    g_detail_saved_scroll_y[settings_page_index(page)] = g_detail_scroll_y;

    g_detail_host = lv_obj_create(g_content);
    if (g_detail_host == nullptr) {
        g_detail_scroll_max_y = 0;
        g_detail_scroll_y = 0;
        return;
    }
    ui_common_lock_object(g_detail_host);
    lv_obj_set_size(g_detail_host, kContentWidth, content_h > kContentHeight ? content_h : kContentHeight);
    lv_obj_set_pos(g_detail_host, 0, -g_detail_scroll_y);
    lv_obj_set_style_radius(g_detail_host, 0, 0);
    lv_obj_set_style_border_width(g_detail_host, 0, 0);
    lv_obj_set_style_pad_all(g_detail_host, 0, 0);
    lv_obj_set_style_bg_opa(g_detail_host, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(g_detail_host, LV_OBJ_FLAG_SCROLLABLE);
}

static void settings_update_vertical_adjust_channel()
{
    const bool enable = g_brightness_adjust_armed ||
        (g_page != SettingsPage::Main && g_detail_scroll_max_y > 0);
    gesture_router_set_vertical_adjust_enabled(enable);
    gesture_router_set_vertical_adjust_edges_reserved(enable);
    if (!enable) {
        g_detail_inertia_active = false;
        g_detail_scroll_velocity_px_s = 0;
    }
}

static bool detail_scroll_gesture_update()
{
    if (g_page == SettingsPage::Main || g_brightness_adjust_armed ||
        g_detail_host == nullptr || g_detail_scroll_max_y <= 0) {
        return false;
    }

    UiVerticalAdjustSnapshot drag = {};
    if (!gesture_router_get_vertical_adjust(&drag)) {
        return false;
    }

    const uint32_t now = lv_tick_get();
    if (g_detail_drag_sequence != drag.sequence) {
        g_detail_drag_sequence = drag.sequence;
        g_detail_drag_start_scroll_y = g_detail_scroll_y;
        g_detail_drag_last_scroll_y = g_detail_scroll_y;
        g_detail_drag_last_tick_ms = now;
        g_detail_scroll_velocity_px_s = 0;
        g_detail_inertia_active = false;
    }

    detail_apply_scroll_y(g_detail_drag_start_scroll_y - static_cast<int32_t>(drag.delta_y));
    const uint32_t elapsed = now - g_detail_drag_last_tick_ms;
    if (elapsed > 0U) {
        const int32_t delta = g_detail_scroll_y - g_detail_drag_last_scroll_y;
        const int32_t instant = static_cast<int32_t>(
            (static_cast<int64_t>(delta) * 1000LL) / static_cast<int64_t>(elapsed));
        g_detail_scroll_velocity_px_s =
            (g_detail_scroll_velocity_px_s * 2 + instant * 3) / 5;
        g_detail_drag_last_scroll_y = g_detail_scroll_y;
        g_detail_drag_last_tick_ms = now;
    }

    if (!drag.released) return true;

    gesture_router_ack_vertical_adjust_release(drag.sequence);
    g_detail_inertia_active =
        (g_detail_scroll_velocity_px_s >= kDetailInertiaStartPxPerSec ||
         g_detail_scroll_velocity_px_s <= -kDetailInertiaStartPxPerSec);
    g_detail_inertia_last_tick_ms = now;
    return true;
}

static void detail_scroll_inertia_update()
{
    if (!g_detail_inertia_active || g_detail_host == nullptr || g_detail_scroll_max_y <= 0) return;

    const uint32_t now = lv_tick_get();
    uint32_t elapsed = now - g_detail_inertia_last_tick_ms;
    if (elapsed == 0U) return;
    if (elapsed > 50U) elapsed = 50U;
    g_detail_inertia_last_tick_ms = now;

    int32_t step = static_cast<int32_t>(
        (static_cast<int64_t>(g_detail_scroll_velocity_px_s) * static_cast<int64_t>(elapsed)) / 1000LL);
    if (step == 0 && g_detail_scroll_velocity_px_s != 0) {
        step = g_detail_scroll_velocity_px_s > 0 ? 1 : -1;
    }
    const int32_t before = g_detail_scroll_y;
    detail_apply_scroll_y(before + step);
    if (g_detail_scroll_y == before || g_detail_scroll_y == 0 || g_detail_scroll_y == g_detail_scroll_max_y) {
        g_detail_inertia_active = false;
        g_detail_scroll_velocity_px_s = 0;
        return;
    }

    // 与歌曲列表相同的手感方向：松手后保留惯性，再快速衰减停住。
    g_detail_scroll_velocity_px_s = (g_detail_scroll_velocity_px_s * 92) / 100;
    if (g_detail_scroll_velocity_px_s < kDetailInertiaStopPxPerSec &&
        g_detail_scroll_velocity_px_s > -kDetailInertiaStopPxPerSec) {
        g_detail_inertia_active = false;
        g_detail_scroll_velocity_px_s = 0;
    }
}

static uint8_t brightness_percent(uint8_t level)
{
    return level;
}

static void brightness_label_set(uint8_t level)
{
    if (g_brightness_value == nullptr) return;
    char text[24] = {};
    snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(brightness_percent(level)));
    lv_label_set_text(g_brightness_value, text);
}

static void brightness_set_armed(bool armed)
{
    const bool target = armed && g_page == SettingsPage::System;
    g_brightness_adjust_armed = target;
    if (!target) {
        g_brightness_dragging = false;
    }

    settings_update_vertical_adjust_channel();

    if (g_brightness_card != nullptr) {
        lv_obj_set_style_border_width(g_brightness_card, target ? 2 : 1, 0);
        lv_obj_set_style_border_color(
            g_brightness_card,
            lv_color_hex(target ? kAccentRgb : kRowBorderRgb),
            0);
        lv_obj_set_style_bg_color(
            g_brightness_card,
            lv_color_hex(target ? 0x17263A : kRowRgb),
            0);
    }
}

static void brightness_disarm()
{
    brightness_set_armed(false);
    g_brightness_drag_sequence = 0U;
}

static void brightness_apply_preview(uint8_t level)
{
    if (level < kBrightnessMin) level = kBrightnessMin;
    if (level > kBrightnessMax) level = kBrightnessMax;

    const esp_err_t ret = screen_lock_simple_set_normal_brightness(level);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "亮度实时预览失败 level=%u：%s",
            static_cast<unsigned>(level), esp_err_to_name(ret));
        return;
    }

    g_brightness_pending_level = level;
    g_brightness_dirty = g_brightness_pending_level != g_brightness_saved_level;
    brightness_label_set(level);
}

static uint8_t brightness_preview_from_delta(uint8_t start_level, int16_t delta_y)
{
    const int32_t span = static_cast<int32_t>(kBrightnessMax - kBrightnessMin);
    const int32_t change =
        (-static_cast<int32_t>(delta_y) * span) / kBrightnessFullScalePx;
    int32_t level = static_cast<int32_t>(start_level) + change;
    if (level < kBrightnessMin) level = kBrightnessMin;
    if (level > kBrightnessMax) level = kBrightnessMax;
    return static_cast<uint8_t>(level);
}

static bool brightness_gesture_update()
{
    if (!g_brightness_adjust_armed || g_page != SettingsPage::System) {
        return false;
    }

    UiVerticalAdjustSnapshot drag = {};
    if (!gesture_router_get_vertical_adjust(&drag)) {
        return false;
    }

    if (!g_brightness_dragging || g_brightness_drag_sequence != drag.sequence) {
        g_brightness_dragging = true;
        g_brightness_drag_sequence = drag.sequence;
        g_brightness_drag_start = g_brightness_pending_level;
    }

    const uint8_t preview = brightness_preview_from_delta(g_brightness_drag_start, drag.delta_y);
    if (preview != g_brightness_pending_level) {
        brightness_apply_preview(preview);
    }

    if (!drag.released) {
        return true;
    }

    g_brightness_dragging = false;
    gesture_router_ack_vertical_adjust_release(drag.sequence);
    ESP_LOGI(TAG, "亮度预览完成：start=%u delta_y=%dpx -> %u；退出设置时保存",
        static_cast<unsigned>(g_brightness_drag_start),
        static_cast<int>(drag.delta_y),
        static_cast<unsigned>(g_brightness_pending_level));
    return true;
}

static void brightness_commit_on_exit()
{
    brightness_disarm();
    if (!g_brightness_dirty) return;

    const uint8_t target = g_brightness_pending_level;
    const esp_err_t ret = device_settings_set_brightness_level(target);
    if (ret == ESP_OK) {
        g_brightness_saved_level = target;
        g_brightness_dirty = false;
        ESP_LOGI(TAG, "退出设置，屏幕亮度已保存：level=%u (%u%%)",
            static_cast<unsigned>(target),
            static_cast<unsigned>(brightness_percent(target)));
        return;
    }

    // NVS提交失败时恢复进入Settings前的持久化亮度，避免硬件状态与下次开机配置不一致。
    (void)screen_lock_simple_set_normal_brightness(g_brightness_saved_level);
    g_brightness_pending_level = g_brightness_saved_level;
    g_brightness_dirty = false;
    ESP_LOGE(TAG, "退出设置保存亮度失败，已恢复 level=%u：%s",
        static_cast<unsigned>(g_brightness_saved_level), esp_err_to_name(ret));
}

static lv_obj_t *create_row(
    lv_obj_t *parent,
    int16_t y,
    int16_t height,
    const char *title,
    const char *value,
    SettingsDetailIcon icon_id,
    bool clickable,
    lv_event_cb_t callback,
    void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    if (row == nullptr) return nullptr;
    ui_common_lock_object(row);
    lv_obj_set_size(row, kContentWidth, height);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(kRowBorderRgb), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(kRowRgb), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (clickable) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        if (callback != nullptr) lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, user_data);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    const lv_image_dsc_t *icon_dsc = settings_detail_icon_dsc(icon_id);
    if (icon_dsc != nullptr) {
        lv_obj_t *icon = lv_image_create(row);
        if (icon != nullptr) {
            lv_image_set_src(icon, icon_dsc);
            lv_obj_set_style_image_recolor(icon, lv_color_hex(kAccentRgb), 0);
            lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, 16, 0);
        }
    }

    lv_obj_t *title_label = make_label(row, title, kHeaderRgb);
    if (title_label != nullptr) {
        lv_obj_set_width(title_label, icon_dsc != nullptr ? 190 : 220);
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
        lv_obj_align(title_label, LV_ALIGN_LEFT_MID, icon_dsc != nullptr ? 52 : 16, 0);
    }

    lv_obj_t *value_label = make_label(row, value, kValueRgb, LV_TEXT_ALIGN_RIGHT);
    if (value_label != nullptr) {
        lv_obj_set_width(value_label, 145);
        lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
        lv_obj_align(value_label, LV_ALIGN_RIGHT_MID, -16, 0);
    }
    return value_label;
}

static void format_mb(char *buffer, size_t bytes, size_t value_bytes)
{
    if (buffer == nullptr || bytes == 0U) return;
    if (value_bytes >= 1024U * 1024U) {
        const size_t mb = value_bytes / (1024U * 1024U);
        const size_t tenths = ((value_bytes % (1024U * 1024U)) * 10U) / (1024U * 1024U);
        snprintf(buffer, bytes, "%u.%u MB",
            static_cast<unsigned>(mb), static_cast<unsigned>(tenths));
    } else {
        snprintf(buffer, bytes, "%u KB", static_cast<unsigned>(value_bytes / 1024U));
    }
}


static void show_page(SettingsPage page);

static void show_page_async(void *user_data)
{
    (void)user_data;
    show_page(g_pending_page);
}

static void category_click_cb(lv_event_t *event)
{
    if (!click_is_valid(event)) return;
    const uintptr_t raw = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (raw >= static_cast<uintptr_t>(SettingsCategory::Count)) return;
    g_pending_page = page_for_category(static_cast<SettingsCategory>(raw));
    lv_async_call(show_page_async, nullptr);
}

static void create_main_grid_tile(
    lv_obj_t *parent,
    int16_t x,
    int16_t y,
    const char *title,
    SettingsCategory category,
    SettingsMenuIcon icon_id)
{
    lv_obj_t *tile = lv_obj_create(parent);
    if (tile == nullptr) return;
    ui_common_lock_object(tile);
    lv_obj_set_size(tile, kMainGridTileWidth, kMainGridTileHeight);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_style_radius(tile, 16, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(kRowBorderRgb), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(kRowRgb), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        tile,
        category_click_cb,
        LV_EVENT_CLICKED,
        reinterpret_cast<void *>(static_cast<uintptr_t>(category)));

    // 一级菜单图标使用Flash内置A8程序数据，不依赖TF卡，也不需要PNG/JPG解码。
    // A8只保存Alpha；实际颜色由image_recolor统一跟随Settings主题。
    const lv_image_dsc_t *icon_dsc = settings_menu_icon_dsc(icon_id);
    if (icon_dsc != nullptr) {
        lv_obj_t *icon = lv_image_create(tile);
        if (icon != nullptr) {
            lv_image_set_src(icon, icon_dsc);
            lv_obj_set_style_image_recolor(icon, lv_color_hex(kAccentRgb), 0);
            lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 34);
        }
    }

    // 图标在上、菜单名在下；主页仍不显示状态文字。
    lv_obj_t *title_label = make_label(tile, title, kHeaderRgb, LV_TEXT_ALIGN_CENTER);
    if (title_label != nullptr) {
        lv_obj_set_width(title_label, kMainGridTileWidth - 16);
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
        lv_obj_align(title_label, LV_ALIGN_BOTTOM_MID, 0, -28);
    }
}

static void create_main_page()
{

    const int16_t start_x = static_cast<int16_t>((kContentWidth - kMainGridWidth) / 2);
    const int16_t start_y = static_cast<int16_t>((360 - kMainGridHeight) / 2);

    for (size_t i = 0; i < static_cast<size_t>(SettingsCategory::Count); ++i) {
        const CategoryRow &def = kCategories[i];

        // 一级设置收敛为2×2四宫格：连接 / 应用 / 系统 / 关于。
        const int16_t slot = static_cast<int16_t>(i);
        const int16_t col = static_cast<int16_t>(slot % 2);
        const int16_t row = static_cast<int16_t>(slot / 2);
        const int16_t x = static_cast<int16_t>(
            start_x + col * (kMainGridTileWidth + kMainGridGapX));
        const int16_t y = static_cast<int16_t>(
            start_y + row * (kMainGridTileHeight + kMainGridGapY));

        create_main_grid_tile(g_content, x, y, def.title, def.category, def.icon);
    }

}

static int16_t detail_row_y(int16_t index)
{
    return static_cast<int16_t>(index * (kDetailRowHeight + kDetailRowGap));
}

static void add_detail_row(int16_t index, const char *title, const char *value, SettingsDetailIcon icon_id, bool active = true)
{
    lv_obj_t *value_label = create_row(
        detail_parent(),
        detail_row_y(index),
        kDetailRowHeight,
        title,
        value,
        icon_id,
        false,
        nullptr,
        nullptr);
    if (index >= 0 && static_cast<size_t>(index) < sizeof(g_detail_values) / sizeof(g_detail_values[0])) {
        g_detail_values[index] = value_label;
    }
    if (!active && value_label != nullptr) {
        lv_obj_set_style_text_color(value_label, lv_color_hex(kMutedRgb), 0);
    }
}

static void add_clickable_detail_row(
    int16_t index,
    const char *title,
    const char *value,
    SettingsDetailIcon icon_id,
    lv_event_cb_t callback)
{
    lv_obj_t *value_label = create_row(
        detail_parent(),
        detail_row_y(index),
        kDetailRowHeight,
        title,
        value,
        icon_id,
        true,
        callback,
        nullptr);
    if (index >= 0 && static_cast<size_t>(index) < sizeof(g_detail_values) / sizeof(g_detail_values[0])) {
        g_detail_values[index] = value_label;
    }
    if (value_label != nullptr) {
        lv_obj_set_style_text_color(value_label, lv_color_hex(kAccentRgb), 0);
    }
}

static AudioOutputMode audio_mode_from_setting(DeviceAudioOutputMode mode)
{
    switch (mode) {
        case DeviceAudioOutputMode::HighImpedanceHeadphones:
            return AudioOutputMode::HighImpedanceHeadphones;
        case DeviceAudioOutputMode::LineOut:
            return AudioOutputMode::LineOut;
        case DeviceAudioOutputMode::NormalHeadphones:
        default:
            return AudioOutputMode::NormalHeadphones;
    }
}

static void sound_mode_click_cb(lv_event_t *event)
{
    if (!click_is_valid(event) || g_audio_mode_switching) return;
    const uintptr_t raw = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (raw > static_cast<uintptr_t>(DeviceAudioOutputMode::HighImpedanceHeadphones)) return;

    const DeviceAudioOutputMode requested = static_cast<DeviceAudioOutputMode>(raw);
    DeviceSettingsSnapshot before = {};
    (void)device_settings_get_snapshot(&before);
    if (before.audio_output_mode == requested) return;

    g_audio_mode_switching = true;
    bool audio_ok = audio_service_set_output_mode(audio_mode_from_setting(requested), true);
    esp_err_t persist_ret = ESP_FAIL;
    if (audio_ok) {
        persist_ret = device_settings_set_audio_output_mode(requested);
        if (persist_ret != ESP_OK) {
            // NVS失败时把硬件也恢复旧档，保证UI/NVS/实际输出三者一致。
            (void)audio_service_set_output_mode(audio_mode_from_setting(before.audio_output_mode), true);
        }
    }
    g_audio_mode_switching = false;

    if (!audio_ok) {
        ESP_LOGE(TAG, "CS43131输出档切换失败：目标=%s",
            device_settings_audio_output_mode_name(requested));
    } else if (persist_ret != ESP_OK) {
        ESP_LOGE(TAG, "CS43131输出档保存失败：%s", esp_err_to_name(persist_ret));
    }

    g_pending_page = SettingsPage::System;
    lv_async_call(show_page_async, nullptr);
}

static DeviceAudioOutputMode next_audio_output_mode(DeviceAudioOutputMode current)
{
    switch (current) {
        case DeviceAudioOutputMode::NormalHeadphones:
            return DeviceAudioOutputMode::HighImpedanceHeadphones;
        case DeviceAudioOutputMode::HighImpedanceHeadphones:
            return DeviceAudioOutputMode::LineOut;
        case DeviceAudioOutputMode::LineOut:
        default:
            return DeviceAudioOutputMode::NormalHeadphones;
    }
}

static void create_audio_output_control(const DeviceSettingsSnapshot &settings)
{
    const DeviceAudioOutputMode next = next_audio_output_mode(settings.audio_output_mode);
    lv_obj_t *value_label = create_row(
        detail_parent(),
        detail_row_y(0),
        kDetailRowHeight,
        "输出模式",
        device_settings_audio_output_mode_name(settings.audio_output_mode),
        SettingsDetailIcon::AudioOutput,
        true,
        sound_mode_click_cb,
        reinterpret_cast<void *>(static_cast<uintptr_t>(next)));
    g_detail_values[0] = value_label;
    if (value_label != nullptr) {
        lv_obj_set_style_text_color(value_label, lv_color_hex(kAccentRgb), 0);
    }
}

static const char *screen_off_name(uint16_t seconds)
{
    switch (seconds) {
        case 0U: return "永不";
        case 10U: return "10秒";
        case 30U: return "30秒";
        case 60U: return "1分钟";
        case 120U: return "2分钟";
        case 180U: return "3分钟";
        default: return "自定义";
    }
}

static uint16_t next_screen_off_seconds(uint16_t current)
{
    switch (current) {
        case 0U: return 10U;
        case 10U: return 30U;
        case 30U: return 60U;
        case 60U: return 120U;
        case 120U: return 180U;
        case 180U:
        default:
            return 0U;
    }
}

static void auto_screen_off_click_cb(lv_event_t *event)
{
    if (!click_is_valid(event) || g_page != SettingsPage::System) return;
    DeviceSettingsSnapshot settings = {};
    if (!device_settings_get_snapshot(&settings)) return;

    const uint16_t next = next_screen_off_seconds(settings.auto_screen_off_seconds);
    const esp_err_t ret = device_settings_set_auto_screen_off_seconds(next);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存自动熄屏失败：%s", esp_err_to_name(ret));
        return;
    }
    screen_lock_simple_configure_auto_off(next, settings.aod_enabled);
    g_pending_page = SettingsPage::System;
    lv_async_call(show_page_async, nullptr);
}

static void aod_enabled_click_cb(lv_event_t *event)
{
    if (!click_is_valid(event) || g_page != SettingsPage::System) return;
    DeviceSettingsSnapshot settings = {};
    if (!device_settings_get_snapshot(&settings)) return;

    const bool next = !settings.aod_enabled;
    const esp_err_t ret = device_settings_set_aod_enabled(next);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存息屏显示失败：%s", esp_err_to_name(ret));
        return;
    }
    screen_lock_simple_configure_auto_off(settings.auto_screen_off_seconds, next);
    g_pending_page = SettingsPage::System;
    lv_async_call(show_page_async, nullptr);
}

static void aux_key_mode_click_cb(lv_event_t *event)
{
    if (!click_is_valid(event) || g_page != SettingsPage::System) return;
    DeviceSettingsSnapshot settings = {};
    if (!device_settings_get_snapshot(&settings)) return;

    const DeviceAuxKeyMode next = settings.aux_key_mode == DeviceAuxKeyMode::Track
        ? DeviceAuxKeyMode::Volume
        : DeviceAuxKeyMode::Track;
    const esp_err_t ret = device_settings_set_aux_key_mode(next);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存辅助键模式失败：%s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "辅助键模式切换：%s", device_settings_aux_key_mode_name(next));
    g_pending_page = SettingsPage::System;
    lv_async_call(show_page_async, nullptr);
}

static void motion_controls_click_cb(lv_event_t *event)
{
    if (!click_is_valid(event) || g_page != SettingsPage::System) return;
    DeviceSettingsSnapshot settings = {};
    if (!device_settings_get_snapshot(&settings)) return;

    const bool next = !settings.motion_controls_enabled;
    const esp_err_t ret = device_settings_set_motion_controls_enabled(next);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存手势控歌开关失败：%s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "手势控歌：%s（前翻/后翻/双击播放暂停统一门控）", next ? "开" : "关");
    g_pending_page = SettingsPage::System;
    lv_async_call(show_page_async, nullptr);
}

static void brightness_card_click_cb(lv_event_t *event)
{
    if (!click_is_valid(event) || g_page != SettingsPage::System) return;
    brightness_set_armed(!g_brightness_adjust_armed);
}

static void create_brightness_control(const DeviceSettingsSnapshot &settings, int16_t index)
{
    (void)settings;
    g_brightness_card = lv_obj_create(detail_parent());
    if (g_brightness_card == nullptr) return;
    ui_common_lock_object(g_brightness_card);
    lv_obj_set_size(g_brightness_card, kContentWidth, kDetailRowHeight);
    lv_obj_set_pos(g_brightness_card, 0, detail_row_y(index));
    lv_obj_set_style_radius(g_brightness_card, 14, 0);
    lv_obj_set_style_border_width(g_brightness_card, 1, 0);
    lv_obj_set_style_border_color(g_brightness_card, lv_color_hex(kRowBorderRgb), 0);
    lv_obj_set_style_bg_color(g_brightness_card, lv_color_hex(kRowRgb), 0);
    lv_obj_set_style_bg_opa(g_brightness_card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_brightness_card, 0, 0);
    lv_obj_remove_flag(g_brightness_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_brightness_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_brightness_card, brightness_card_click_cb, LV_EVENT_CLICKED, nullptr);

    const lv_image_dsc_t *brightness_icon_dsc =
        settings_detail_icon_dsc(SettingsDetailIcon::Brightness);
    if (brightness_icon_dsc != nullptr) {
        lv_obj_t *icon = lv_image_create(g_brightness_card);
        if (icon != nullptr) {
            lv_image_set_src(icon, brightness_icon_dsc);
            lv_obj_set_style_image_recolor(icon, lv_color_hex(kAccentRgb), 0);
            lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, 16, 0);
        }
    }

    lv_obj_t *title = make_label(g_brightness_card, "屏幕亮度", kHeaderRgb);
    if (title != nullptr) {
        lv_obj_set_width(title, 190);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 52, 0);
    }

    g_brightness_value = make_label(g_brightness_card, "", kAccentRgb, LV_TEXT_ALIGN_RIGHT);
    if (g_brightness_value != nullptr) {
        lv_obj_set_width(g_brightness_value, 145);
        lv_obj_align(g_brightness_value, LV_ALIGN_RIGHT_MID, -16, 0);
        if (index >= 0 && static_cast<size_t>(index) < sizeof(g_detail_values) / sizeof(g_detail_values[0])) {
            g_detail_values[index] = g_brightness_value;
        }
        brightness_label_set(g_brightness_pending_level);
    }

    // 点选整行后复用播放器音量的全屏纵向手势；详情页不再额外放提示行或小滑条。
    brightness_set_armed(g_brightness_adjust_armed);
}

static void usb_tf_storage_return_click_cb(lv_event_t *event);

static bool show_usb_runtime_overlay()
{
    lv_obj_t *screen = lv_screen_active();
    if (screen == nullptr) return false;

    g_usb_return_safe_ui = false;
    g_usb_overlay = lv_obj_create(screen);
    if (g_usb_overlay == nullptr) return false;

    lv_obj_remove_style_all(g_usb_overlay);
    lv_obj_set_size(g_usb_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_center(g_usb_overlay);
    lv_obj_set_style_bg_color(g_usb_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_usb_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_usb_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_usb_overlay, LV_OBJ_FLAG_CLICKABLE);

    const lv_font_t *service_font = usb_service_font_get();

    lv_obj_t *title = lv_label_create(g_usb_overlay);
    if (title != nullptr) {
        lv_label_set_text(title, "USB 磁盘模式");
        lv_obj_set_style_text_font(title, service_font, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -54);
    }

    g_usb_overlay_status = lv_label_create(g_usb_overlay);
    if (g_usb_overlay_status != nullptr) {
        lv_label_set_text(g_usb_overlay_status, "存储卡开启");
        lv_obj_set_style_text_font(g_usb_overlay_status, service_font, 0);
        lv_obj_set_style_text_color(g_usb_overlay_status, lv_color_hex(0xBFD1E8), 0);
        lv_obj_set_style_text_align(g_usb_overlay_status, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(g_usb_overlay_status, 8, 0);
        lv_obj_set_width(g_usb_overlay_status, 410);
        lv_obj_align(g_usb_overlay_status, LV_ALIGN_CENTER, 0, 22);
    }

    g_usb_overlay_action = lv_button_create(g_usb_overlay);
    if (g_usb_overlay_action != nullptr) {
        lv_obj_set_size(g_usb_overlay_action, 138, 52);
        lv_obj_align(g_usb_overlay_action, LV_ALIGN_CENTER, 0, 124);
        lv_obj_set_style_radius(g_usb_overlay_action, 16, 0);
        lv_obj_set_style_border_width(g_usb_overlay_action, 0, 0);
        lv_obj_set_style_bg_color(g_usb_overlay_action, lv_color_hex(0x326CA8), 0);
        lv_obj_set_style_bg_opa(g_usb_overlay_action, LV_OPA_COVER, 0);
        lv_obj_add_flag(g_usb_overlay_action, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(g_usb_overlay_action, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(
            g_usb_overlay_action,
            usb_tf_storage_return_click_cb,
            LV_EVENT_CLICKED,
            nullptr);

        lv_obj_t *action_label = lv_label_create(g_usb_overlay_action);
        if (action_label != nullptr) {
            lv_label_set_text(action_label, "恢复");
            lv_obj_set_style_text_font(action_label, service_font, 0);
            lv_obj_set_style_text_color(action_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_center(action_label);
        }
    }

    lv_obj_move_foreground(g_usb_overlay);
    lv_obj_invalidate(g_usb_overlay);
    return true;
}

static void usb_runtime_overlay_set_status(const char *text, uint32_t rgb)
{
    if (text == nullptr || !lvgl_port_lock(1000)) return;
    if (g_usb_overlay_status != nullptr && lv_obj_is_valid(g_usb_overlay_status)) {
        lv_label_set_text(g_usb_overlay_status, text);
        lv_obj_set_style_text_color(g_usb_overlay_status, lv_color_hex(rgb), 0);
        lv_obj_invalidate(g_usb_overlay_status);
    }
    lvgl_port_unlock();
}

static void usb_library_scan_event(MediaLibraryScanEvent event, void *)
{
    if (event == MediaLibraryScanEvent::ChangesDetected) {
        usb_runtime_overlay_set_status("正在更新音乐库...", 0xC7D5E8);
    }
}

static void usb_runtime_overlay_show_library_summary(const MediaLibraryChangeSummary &changes)
{
    char status[160] = {};
    size_t used = static_cast<size_t>(snprintf(status, sizeof(status), "音乐库已更新"));
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
    append_count("新增", changes.added_count);
    append_count("删除", changes.removed_count);
    append_count("更新", changes.updated_count);
    usb_runtime_overlay_set_status(status, 0xA9D6B4);
}

static void usb_runtime_overlay_destroy()
{
    if (!lvgl_port_lock(1000)) return;
    if (g_usb_overlay != nullptr && lv_obj_is_valid(g_usb_overlay)) {
        lv_obj_delete(g_usb_overlay);
    }
    g_usb_overlay = nullptr;
    g_usb_overlay_status = nullptr;
    g_usb_overlay_action = nullptr;
    g_usb_return_safe_ui = false;
    lvgl_port_unlock();
}

static void usb_runtime_overlay_set_service_ready()
{
    if (!lvgl_port_lock(1000)) return;

    if (g_usb_overlay_status != nullptr && lv_obj_is_valid(g_usb_overlay_status)) {
        lv_label_set_text(
            g_usb_overlay_status,
            "可在电脑上访问存储卡\n"
            "电脑安全弹出后\n"
            "即恢复");
        lv_obj_set_style_text_color(g_usb_overlay_status, lv_color_hex(0xC7D5E8), 0);
        lv_obj_invalidate(g_usb_overlay_status);
    }
    if (g_usb_overlay_action != nullptr && lv_obj_is_valid(g_usb_overlay_action)) {
        set_visible(g_usb_overlay_action, true);
        lv_obj_invalidate(g_usb_overlay_action);
    }
    g_usb_return_safe_ui = false;
    lvgl_port_unlock();
}

static void usb_runtime_overlay_refresh_return_action()
{
    if (!usb_storage_service_is_active() ||
        g_usb_overlay_action == nullptr || !lv_obj_is_valid(g_usb_overlay_action)) {
        return;
    }

    // 手动“恢复”始终保留作兜底；正常 V3.5 路径会在 owner=APP 后自动启动归还。
    set_visible(g_usb_overlay_action, true);

    const bool safe_to_return = usb_storage_service_host_safe_to_return();
    if (safe_to_return && !g_usb_return_safe_ui) {
        if (g_usb_overlay_status != nullptr && lv_obj_is_valid(g_usb_overlay_status)) {
            lv_label_set_text(g_usb_overlay_status, "安全弹出\n可恢复");
            lv_obj_set_style_text_color(g_usb_overlay_status, lv_color_hex(0xA9D6B4), 0);
            lv_obj_invalidate(g_usb_overlay_status);
        }
        g_usb_return_safe_ui = true;
        ESP_LOGI(TAG, "USB MSC收到设备级安全释放信号：恢复按钮保持可用");
    } else if (!safe_to_return && g_usb_return_safe_ui) {
        if (g_usb_overlay_status != nullptr && lv_obj_is_valid(g_usb_overlay_status)) {
            lv_label_set_text(
                g_usb_overlay_status,
                "可在电脑上访问存储卡\n"
                "电脑安全弹出后\n"
                "即恢复");
            lv_obj_set_style_text_color(g_usb_overlay_status, lv_color_hex(0xC7D5E8), 0);
            lv_obj_invalidate(g_usb_overlay_status);
        }
        g_usb_return_safe_ui = false;
    }
}

static void usb_tf_runtime_enter_task(void *)
{
    bool artwork_quiet = false;
    bool lyrics_quiet = false;
    bool storage_exclusive = false;

    // 软件热切换不会经过 Settings leave，因此显式提交本会话设置与播放器持久化状态。
    music_list_commit_on_exit();
    brightness_commit_on_exit();
    const esp_err_t persistent_ret = persistent_state_flush();
    if (persistent_ret != ESP_OK) {
        ESP_LOGW(TAG, "进入TF卡USB模式前保存播放器状态失败：%s；继续热切换",
            esp_err_to_name(persistent_ret));
    }

    // system_loop 已被 runtime transition gate 截停，不会在 Stop 之后自动续播/换曲。
    if (!audio_service_stop(true)) {
        ESP_LOGW(TAG, "进入TF卡USB模式前停止音频失败；取消热切换");
        goto fail;
    }

    artwork_quiet = artwork_loader_prepare_storage_handoff(pdMS_TO_TICKS(3000));
    lyrics_quiet = lyrics_service_prepare_storage_handoff(pdMS_TO_TICKS(3000));
    if (!artwork_quiet || !lyrics_quiet) {
        ESP_LOGE(TAG, "后台TF文件任务未能在时限内静默，取消USB接管");
        goto fail;
    }

    // 这里拿到全局 SD mutex 后，所有旧临界区都已经退出；blocked 发布后普通任务无法再穿透。
    storage_exclusive = storage_io_begin_usb_handoff(pdMS_TO_TICKS(3000));
    if (!storage_exclusive) {
        ESP_LOGE(TAG, "获取USB独占TF总线超时");
        goto fail;
    }

    {
        const esp_err_t unmount_ret = sdcard_unmount_for_usb();
        if (unmount_ret != ESP_OK) {
            ESP_LOGE(TAG, "热切换卸载TF失败：%s", esp_err_to_name(unmount_ret));
            goto fail;
        }
    }

    {
        const esp_err_t service_ret = usb_storage_service_start();
        if (service_ret != ESP_OK) {
            ESP_LOGE(TAG, "运行时启动USB MSC失败：%s；尝试恢复普通TF挂载",
                esp_err_to_name(service_ret));
            const esp_err_t remount_ret = sdcard_init();
            if (remount_ret != ESP_OK) {
                ESP_LOGE(TAG, "USB启动失败后TF重新挂载也失败：%s", esp_err_to_name(remount_ret));
            }
            goto fail;
        }
    }

    // TinyUSB raw owner 成功建立：释放 mutex，但继续封锁应用侧 /sdcard；
    // 直到 Host 安全弹出并由 V3 归还任务重新挂载普通 VFS。
    storage_io_finish_usb_handoff(true);
    storage_exclusive = false;
    usb_storage_service_finish_runtime_transition();
    usb_runtime_overlay_set_service_ready();
    ESP_LOGI(TAG, "USB MSC V3.5热切换完成：TF owner=TinyUSB MSC；Windows安全弹出后将按owner=APP自动归还");
    vTaskDelete(nullptr);
    return;

fail:
    if (storage_exclusive) {
        // 若 VFS 已经卸载而服务未成功，尽力恢复原挂载；已挂载时 sdcard_init() 会直接返回 OK。
        if (!sdcard_is_mounted()) {
            const esp_err_t remount_ret = sdcard_init();
            if (remount_ret != ESP_OK) {
                ESP_LOGE(TAG, "热切换回滚重新挂载TF失败：%s", esp_err_to_name(remount_ret));
            }
        }
        storage_io_finish_usb_handoff(false);
    }
    artwork_loader_resume_storage_after_handoff();
    lyrics_service_resume_storage_after_handoff();
    usb_storage_service_cancel_runtime_transition();
    g_usb_transition_pending = false;
    usb_runtime_overlay_destroy();
    ESP_LOGW(TAG, "USB MSC热切换失败：已撤销遮罩/运行闸门并恢复正常Music运行");
    vTaskDelete(nullptr);
}

static void usb_tf_storage_enter_click_cb(lv_event_t *event)
{
    if (!click_is_valid(event) || g_page != SettingsPage::Connection || g_usb_transition_pending) return;
    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "TF卡未挂载，不能运行时切换USB MSC");
        return;
    }
    if (!usb_storage_service_begin_runtime_transition()) return;

    g_usb_transition_pending = true;
    const bool prompt_visible = show_usb_runtime_overlay();
    const BaseType_t task_created = xTaskCreate(
        usb_tf_runtime_enter_task,
        "usb_msc_hot",
        4096,
        nullptr,
        2,
        nullptr);

    if (task_created != pdPASS) {
        usb_storage_service_cancel_runtime_transition();
        g_usb_transition_pending = false;
        usb_runtime_overlay_destroy();
        ESP_LOGE(TAG, "USB MSC热切换任务创建失败；已恢复正常Music运行");
        return;
    }

    ESP_LOGI(TAG, "请求USB MSC V3热切换：提示=%s，ESP32不重启",
        prompt_visible ? "已显示" : "显示失败");
}

static char *usb_tf_copy_selected_track_path()
{
    if (!player_state_is_ready()) return nullptr;
    const char *path = player_state_get_path();
    if (path == nullptr || path[0] == '\0') return nullptr;

    const size_t bytes = strlen(path) + 1U;
    char *copy = static_cast<char *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        copy = static_cast<char *>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    }
    if (copy != nullptr) {
        memcpy(copy, path, bytes);
    } else {
        ESP_LOGW(TAG, "USB归还前无法复制当前歌曲路径；热刷新后将按旧全局索引回退");
    }
    return copy;
}

static void usb_tf_runtime_return_task(void *)
{
    const size_t preferred_track_index = player_state_is_ready() ? player_state_get_index() : 0U;
    char *preferred_track_path = usb_tf_copy_selected_track_path();
    usb_runtime_overlay_set_status("恢复存储卡", 0xC7D5E8);

    // 正常 V3.5 路径由 MSC owner=APP 自动进入；手动按钮只作为兜底。
    // 先让 deferred writes 落盘并删除 LUN，再把 USB descriptor 收缩为 CDC-only，
    // 最后把同一张卡交回 FakePod VFS；两个 TF owner 在任何时刻都不会并存。
    const esp_err_t stop_ret = usb_storage_service_stop();
    if (stop_ret != ESP_OK) {
        usb_storage_service_cancel_runtime_return();
        usb_runtime_overlay_set_status(
            "请先安全弹出\n"
            "安全弹出后恢复",
            0xD9B86C);
        ESP_LOGE(TAG, "USB MSC V3停止服务失败：%s；保持MSC封锁，可再次恢复",
            esp_err_to_name(stop_ret));
        heap_caps_free(preferred_track_path);
        vTaskDelete(nullptr);
        return;
    }

    if (!storage_io_begin_usb_return(pdMS_TO_TICKS(3000))) {
        usb_storage_service_finish_runtime_return(false);
        usb_runtime_overlay_set_status("请重启设备", 0xE28A8A);
        ESP_LOGE(TAG, "USB MSC V3已释放USB但无法取得TF归还闸门；为避免双owner继续封锁，请重启设备");
        heap_caps_free(preferred_track_path);
        vTaskDelete(nullptr);
        return;
    }

    const esp_err_t mount_ret = sdcard_init();
    if (mount_ret != ESP_OK) {
        storage_io_finish_usb_return(false);
        usb_storage_service_finish_runtime_return(false);
        usb_runtime_overlay_set_status("请重启设备", 0xE28A8A);
        ESP_LOGE(TAG, "USB MSC V3归还后重新挂载TF失败：%s；继续封锁普通TF访问",
            esp_err_to_name(mount_ret));
        heap_caps_free(preferred_track_path);
        vTaskDelete(nullptr);
        return;
    }

    // 仍持有 USB return owner + recursive SD mutex，且 system_loop / Artwork / Lyrics 都未放开。
    // 在这个唯一安全窗口完成 Catalog 事务热替换，避免任何旧 generation 裸指针并发使用。
    MusicCatalogV2 retired_catalog = {};
    MediaLibraryChangeSummary library_changes = {};
    const uint32_t old_generation = media_catalog_v2_generation();
    const size_t old_count = media_library_get_count();
    const esp_err_t reload_ret = media_library_hot_reload_quiesced(
        &retired_catalog,
        &library_changes,
        usb_library_scan_event,
        nullptr
    );
    if (reload_ret == ESP_OK) {
        const bool player_rebound = player_state_rebind_after_catalog_reload(preferred_track_path, preferred_track_index);
        // Player generation 已重绑，后台资产仍停用；现在才允许释放旧 Catalog 内存。
        media_catalog_v2_release(&retired_catalog);
        ESP_LOGI(TAG,
            "USB归还曲库热刷新：generation=%lu->%lu tracks=%u->%u player=%s 新增=%lu 删除=%lu 更新=%lu",
            static_cast<unsigned long>(old_generation),
            static_cast<unsigned long>(media_catalog_v2_generation()),
            static_cast<unsigned>(old_count),
            static_cast<unsigned>(media_library_get_count()),
            player_rebound ? "已重绑" : "重绑失败",
            static_cast<unsigned long>(library_changes.added_count),
            static_cast<unsigned long>(library_changes.removed_count),
            static_cast<unsigned long>(library_changes.updated_count));
        if (library_changes.changed) {
            usb_runtime_overlay_show_library_summary(library_changes);
            vTaskDelay(pdMS_TO_TICKS(800));
        }
    } else {
        // 事务失败时 media_library 保留原 Catalog；继续恢复设备，只是目录仍为 USB 前快照。
        ESP_LOGW(TAG, "USB归还曲库热刷新失败：%s；继续使用旧Catalog", esp_err_to_name(reload_ret));
    }
    heap_caps_free(preferred_track_path);
    preferred_track_path = nullptr;

    storage_io_finish_usb_return(true);
    artwork_loader_resume_storage_after_handoff();
    lyrics_service_resume_storage_after_handoff();

    if (player_state_is_ready() && media_library_get_count() > 0U) {
        (void)lyrics_service_request_track(static_cast<uint32_t>(player_state_get_index()));
    }

    // 所有普通TF owner、后台资产服务均恢复后，最后再放开 system_loop。
    usb_storage_service_finish_runtime_return(true);
    g_usb_transition_pending = false;
    usb_runtime_overlay_destroy();
    ESP_LOGI(TAG, "USB MSC热归还完成：/sdcard 已恢复，USB=CDC-only，Catalog generation=%lu tracks=%u",
        static_cast<unsigned long>(media_catalog_v2_generation()),
        static_cast<unsigned>(media_library_get_count()));
    vTaskDelete(nullptr);
}

static bool usb_tf_storage_start_return(bool user_confirmed_eject, bool automatic)
{
    if (!g_usb_transition_pending || !usb_storage_service_is_active()) return false;
    if (!usb_storage_service_begin_runtime_return(user_confirmed_eject)) return false;

    if (g_usb_overlay_status != nullptr && lv_obj_is_valid(g_usb_overlay_status)) {
        lv_label_set_text(g_usb_overlay_status, automatic ? "安全弹出\n恢复存储卡" : "恢复存储卡");
        lv_obj_set_style_text_color(g_usb_overlay_status, lv_color_hex(0xA9D6B4), 0);
        lv_obj_invalidate(g_usb_overlay_status);
    }
    if (g_usb_overlay_action != nullptr && lv_obj_is_valid(g_usb_overlay_action)) {
        set_visible(g_usb_overlay_action, false);
    }

    const BaseType_t task_created = xTaskCreate(
        usb_tf_runtime_return_task,
        "usb_msc_back",
        6144,
        nullptr,
        2,
        nullptr);

    if (task_created != pdPASS) {
        usb_storage_service_cancel_runtime_return();
        if (g_usb_overlay_action != nullptr && lv_obj_is_valid(g_usb_overlay_action)) {
            set_visible(g_usb_overlay_action, true);
        }
        ESP_LOGE(TAG, "USB MSC V3.5归还任务创建失败；保留手动恢复入口");
        return false;
    }

    ESP_LOGI(TAG, "%s", automatic
        ? "USB MSC检测到owner=APP：自动开始热归还"
        : "USB MSC用户确认安全弹出：开始手动热归还");
    return true;
}

static void usb_tf_storage_return_click_cb(lv_event_t *event)
{
    if (!click_is_valid(event)) return;

    // 手动按钮继续作为兜底：正常 V3.5 路径由 Windows 安全弹出后的 owner=APP 自动归还。
    if (!usb_tf_storage_start_return(true, false)) {
        ESP_LOGW(TAG, "USB MSC正在切换中，暂不能热归还");
    }
}

static void create_connection_page(const DeviceSettingsSnapshot &settings)
{
    // USB模式仅显示当前启动配置；真正的高风险TF owner切换只允许从下一行显式触发，
    // 避免用户查看USB状态时误触发一次性MSC handoff。
    add_detail_row(
        0,
        "USB模式",
        device_settings_usb_mode_name(settings.usb_mode),
        SettingsDetailIcon::Usb);
    add_clickable_detail_row(
        1,
        "TF卡文件管理",
        "进入",
        SettingsDetailIcon::TfFiles,
        usb_tf_storage_enter_click_cb);
    add_detail_row(2, "BLE模式", "待接入", SettingsDetailIcon::Bluetooth, false);
    add_detail_row(3, "飞行模式", "待接入", SettingsDetailIcon::Airplane, false);
}

static PlayerFolderScope player_folder_scope_from_setting(DeviceMusicListScope scope)
{
    switch (scope) {
        case DeviceMusicListScope::All: return PlayerFolderScope::All;
        case DeviceMusicListScope::Level1: return PlayerFolderScope::Level1;
        case DeviceMusicListScope::Level2: return PlayerFolderScope::Level2;
    }
    return PlayerFolderScope::All;
}

static bool music_current_folder_path(
    PlayerFolderScope scope, char *buffer, size_t buffer_size)
{
    if ((scope != PlayerFolderScope::Level1 && scope != PlayerFolderScope::Level2) ||
        buffer == nullptr || buffer_size == 0U) {
        return false;
    }
    buffer[0] = '\0';
    const char *track_path = player_state_get_path();
    static constexpr const char *kMusicPrefix = "/sdcard/MUSIC/";
    const size_t root_length = strlen(kMusicPrefix);
    if (track_path == nullptr || strncasecmp(track_path, kMusicPrefix, root_length) != 0) {
        return false;
    }

    const char *relative = track_path + root_length;
    const char *first = strchr(relative, '/');
    if (first == nullptr) return false;
    const char *end = first;
    if (scope == PlayerFolderScope::Level2) {
        const char *second = strchr(first + 1U, '/');
        if (second == nullptr) return false;
        end = second;
    }

    const size_t length = static_cast<size_t>(end - track_path) + 1U;
    if (length + 1U > buffer_size) return false;
    memcpy(buffer, track_path, length);
    buffer[length] = '\0';
    return player_playlist_folder_selection_available(scope, buffer, nullptr);
}

static bool music_first_folder_path(
    PlayerFolderScope scope, char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U) return false;
    buffer[0] = '\0';
    if (scope == PlayerFolderScope::Level1) {
        return player_playlist_copy_folder_option_at(
            PlayerFolderScope::Level1, nullptr, 0U, buffer, buffer_size, nullptr);
    }
    if (scope != PlayerFolderScope::Level2) return false;

    const size_t parent_count = player_playlist_get_folder_option_count(PlayerFolderScope::Level1, nullptr);
    for (size_t parent = 0U; parent < parent_count; ++parent) {
        char parent_path[PLAYER_FOLDER_PATH_MAX] = {};
        if (!player_playlist_copy_folder_option_at(
                PlayerFolderScope::Level1, nullptr, parent,
                parent_path, sizeof(parent_path), nullptr)) {
            continue;
        }
        if (player_playlist_copy_folder_option_at(
                PlayerFolderScope::Level2, parent_path, 0U,
                buffer, buffer_size, nullptr)) {
            return true;
        }
    }
    return false;
}

static bool music_resolve_folder_path(
    PlayerFolderScope scope, const char *saved_path, char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U) return false;
    buffer[0] = '\0';
    if (saved_path != nullptr && saved_path[0] != '\0' &&
        player_playlist_folder_selection_available(scope, saved_path, nullptr) &&
        strlen(saved_path) + 1U <= buffer_size) {
        snprintf(buffer, buffer_size, "%s", saved_path);
        return true;
    }
    if (music_current_folder_path(scope, buffer, buffer_size)) {
        return true;
    }
    return music_first_folder_path(scope, buffer, buffer_size);
}

static bool music_list_restore_runtime(const DeviceMusicListSelection &selection)
{
    if (selection.level1_path[0] != '\0') {
        (void)player_control_set_folder_selection(PlayerFolderScope::Level1, selection.level1_path);
    }
    if (selection.level2_path[0] != '\0') {
        (void)player_control_set_folder_selection(PlayerFolderScope::Level2, selection.level2_path);
    }
    return player_control_set_folder_scope(player_folder_scope_from_setting(selection.scope));
}

static bool music_list_apply_scope(DeviceMusicListScope scope)
{
    DeviceMusicListSelection previous = {};
    if (!device_settings_get_music_list_selection(&previous)) return false;

    char level1_path[PLAYER_FOLDER_PATH_MAX] = {};
    char level2_path[PLAYER_FOLDER_PATH_MAX] = {};
    snprintf(level1_path, sizeof(level1_path), "%s", previous.level1_path);
    snprintf(level2_path, sizeof(level2_path), "%s", previous.level2_path);

    const PlayerFolderScope player_scope = player_folder_scope_from_setting(scope);
    if (scope == DeviceMusicListScope::Level1) {
        if (!music_resolve_folder_path(
                PlayerFolderScope::Level1, previous.level1_path,
                level1_path, sizeof(level1_path)) ||
            !player_control_set_folder_selection(PlayerFolderScope::Level1, level1_path)) {
            ESP_LOGW(TAG, "没有可用的一级音乐文件夹");
            return false;
        }
    } else if (scope == DeviceMusicListScope::Level2) {
        if (!music_resolve_folder_path(
                PlayerFolderScope::Level2, previous.level2_path,
                level2_path, sizeof(level2_path)) ||
            !player_control_set_folder_selection(PlayerFolderScope::Level2, level2_path)) {
            ESP_LOGW(TAG, "没有含直属音乐的二级文件夹");
            return false;
        }
    }

    if (!player_control_set_folder_scope(player_scope)) {
        (void)music_list_restore_runtime(previous);
        return false;
    }

    const esp_err_t ret = device_settings_set_music_list_selection(scope, level1_path, level2_path);
    if (ret != ESP_OK) {
        (void)music_list_restore_runtime(previous);
        ESP_LOGW(TAG, "保存播放列表范围失败：%s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "播放列表范围已切换：%s；文件夹在主页下拉列表左滑切换",
        device_settings_music_list_scope_name(scope));
    return true;
}

static DeviceMusicListScope next_music_list_scope(DeviceMusicListScope scope)
{
    switch (scope) {
        case DeviceMusicListScope::All: return DeviceMusicListScope::Level1;
        case DeviceMusicListScope::Level1: return DeviceMusicListScope::Level2;
        case DeviceMusicListScope::Level2:
        default:
            return DeviceMusicListScope::All;
    }
}

static void music_list_scope_cycle_click_cb(lv_event_t *event)
{
    if (!click_is_valid(event) || g_page != SettingsPage::Applications) return;
    g_music_scope_pending = next_music_list_scope(g_music_scope_pending);
    g_music_scope_dirty = g_music_scope_pending != g_music_scope_saved;

    if (g_detail_values[1] != nullptr) {
        lv_label_set_text(
            g_detail_values[1],
            device_settings_music_list_scope_name(g_music_scope_pending));
        lv_obj_invalidate(g_detail_values[1]);
    }
    ESP_LOGI(TAG, "播放列表范围待保存：%s（退出设置时生效）",
        device_settings_music_list_scope_name(g_music_scope_pending));
}

static void music_list_commit_on_exit()
{
    if (!g_music_scope_dirty) return;

    const DeviceMusicListScope requested = g_music_scope_pending;
    if (!music_list_apply_scope(requested)) {
        ESP_LOGW(TAG, "退出设置时应用播放列表范围失败：%s；保持=%s",
            device_settings_music_list_scope_name(requested),
            device_settings_music_list_scope_name(g_music_scope_saved));
        g_music_scope_pending = g_music_scope_saved;
        g_music_scope_dirty = false;
        return;
    }

    g_music_scope_saved = requested;
    g_music_scope_pending = requested;
    g_music_scope_dirty = false;
    ESP_LOGI(TAG, "退出设置保存播放列表范围：%s",
        device_settings_music_list_scope_name(requested));
}

static void create_applications_page(const DeviceSettingsSnapshot &settings)
{
    (void)settings;
    add_detail_row(0, "自启动APP", "待接入", SettingsDetailIcon::Startup, false);
    add_clickable_detail_row(
        1,
        "播放列表范围",
        device_settings_music_list_scope_name(g_music_scope_pending),
        SettingsDetailIcon::Music,
        music_list_scope_cycle_click_cb);
    add_detail_row(2, "视频播放器", "待接入", SettingsDetailIcon::Video, false);
    add_detail_row(3, "文本阅读", "待接入", SettingsDetailIcon::Reader, false);
    add_detail_row(4, "电子音流", "待接入", SettingsDetailIcon::Synth, false);
}

static void create_system_page(const DeviceSettingsSnapshot &settings)
{
    create_audio_output_control(settings);
    create_brightness_control(settings, 1);
    add_clickable_detail_row(
        2,
        "自动熄屏",
        screen_off_name(settings.auto_screen_off_seconds),
        SettingsDetailIcon::ScreenOff,
        auto_screen_off_click_cb);
    add_clickable_detail_row(
        3,
        "息屏显示",
        settings.aod_enabled ? "开" : "关",
        SettingsDetailIcon::Aod,
        aod_enabled_click_cb);
    add_clickable_detail_row(
        4,
        "辅助键模式",
        device_settings_aux_key_mode_name(settings.aux_key_mode),
        SettingsDetailIcon::AuxKey,
        aux_key_mode_click_cb);
    add_clickable_detail_row(
        5,
        "手势控歌",
        settings.motion_controls_enabled ? "开" : "关",
        SettingsDetailIcon::Music,
        motion_controls_click_cb);
}

static void create_about_page()
{
    char value[64] = {};
    add_detail_row(0, "固件版本", FAKEPOD_FIRMWARE_VERSION, SettingsDetailIcon::Firmware);
    add_detail_row(1, "ESP-IDF", esp_get_idf_version(), SettingsDetailIcon::Framework);
    format_mb(value, sizeof(value), heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    add_detail_row(2, "内部RAM剩余", value, SettingsDetailIcon::InternalRam);
    format_mb(value, sizeof(value), heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    add_detail_row(3, "PSRAM剩余", value, SettingsDetailIcon::Psram);
    if (sdcard_is_mounted()) {
        snprintf(value, sizeof(value), "%u MB", static_cast<unsigned>(sdcard_get_capacity_mb()));
    } else {
        snprintf(value, sizeof(value), "--");
    }
    add_detail_row(4, "TF卡容量", value, SettingsDetailIcon::SdCard);
}

static void show_page(SettingsPage page)
{
    if (g_content == nullptr || g_header_title == nullptr) return;
    app_launcher_overlay_hide();
    brightness_disarm();
    lv_obj_clean(g_content);
    clear_detail_value_refs();
    g_page = page;
    lv_label_set_text(g_header_title, page_title(page));

    DeviceSettingsSnapshot settings = {};
    (void)device_settings_get_snapshot(&settings);

    // LVGL 原生滚动仍保持关闭；详情页复用歌曲列表的“原始触摸驱动 + 手动位移 + 惯性”逻辑。
    // 这样不会触发全局 scroll guard，也能在设置项超过 5 条后自然纵向浏览。
    lv_obj_remove_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_scroll_dir(g_content, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(g_content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_scroll_to(g_content, 0, 0, LV_ANIM_OFF);
    detail_prepare(page);

    switch (page) {
        case SettingsPage::Main: create_main_page(); break;
        case SettingsPage::Connection: create_connection_page(settings); break;
        case SettingsPage::Applications: create_applications_page(settings); break;
        case SettingsPage::System: create_system_page(settings); break;
        case SettingsPage::About: create_about_page(); break;
    }
    settings_update_vertical_adjust_channel();
    lv_obj_invalidate(g_content);
}

static void return_music_async(void *user_data)
{
    (void)user_data;
    if (app_manager_foreground() != AppId::Settings) return;
    const esp_err_t ret = app_manager_request_foreground(
        AppId::Music, AppTransitionMode::PreserveBackground);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "返回Music失败：%s", esp_err_to_name(ret));
    }
}

static void back_click_cb(lv_event_t *event)
{
    if (!click_is_valid(event)) return;
    if (g_page != SettingsPage::Main) {
        show_page(parent_page(g_page));
        return;
    }
    lv_async_call(return_music_async, nullptr);
}

static void gesture_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_root == nullptr || app_manager_foreground() != AppId::Settings) return;

    // 与播放器Overlay音量一致：选中亮度项后，任意位置的纵向拖动拥有最高优先级。
    if (brightness_gesture_update()) {
        return;
    }
    if (detail_scroll_gesture_update()) {
        return;
    }
    detail_scroll_inertia_update();

    UiGestureAction action = UiGestureAction::None;
    if (!gesture_router_take_action(&action)) return;

    if (action == UiGestureAction::PullUpFromBottom) {
        const esp_err_t ret = app_launcher_overlay_show(g_root, AppId::Settings);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "设置页打开共享Launcher失败：%s", esp_err_to_name(ret));
        }
        return;
    }
    if (action == UiGestureAction::SwipeRight) {
        if (g_page != SettingsPage::Main) {
            show_page(parent_page(g_page));
        } else {
            lv_async_call(return_music_async, nullptr);
        }
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_root == nullptr || app_manager_foreground() != AppId::Settings ||
        app_launcher_overlay_is_visible()) return;

    if (g_usb_overlay != nullptr && lv_obj_is_valid(g_usb_overlay)) {
        usb_runtime_overlay_refresh_return_action();

        // V3.5：只在 MSC storage 明确切回 owner=APP 后自动归还。
        // 不使用 USB DETACHED，避免首次 TinyUSB 重枚举时的短暂断开误触发。
        if (usb_storage_service_auto_return_requested()) {
            (void)usb_tf_storage_start_return(false, true);
            return;
        }
    }

    if (g_page == SettingsPage::Main) return;

    char value[64] = {};
    if (g_page == SettingsPage::About) {
        if (g_detail_values[2] != nullptr) {
            format_mb(value, sizeof(value),
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            lv_label_set_text(g_detail_values[2], value);
        }
        if (g_detail_values[3] != nullptr) {
            format_mb(value, sizeof(value),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            lv_label_set_text(g_detail_values[3], value);
        }
        if (g_detail_values[4] != nullptr) {
            if (sdcard_is_mounted()) {
                snprintf(value, sizeof(value), "%u MB", static_cast<unsigned>(sdcard_get_capacity_mb()));
            } else {
                snprintf(value, sizeof(value), "--");
            }
            lv_label_set_text(g_detail_values[4], value);
        }
    }
}

static esp_err_t settings_create()
{
    if (g_root != nullptr) return ESP_OK;
    lv_obj_t *screen = lv_screen_active();
    if (screen == nullptr) return ESP_ERR_INVALID_STATE;

    lv_display_t *display = lv_display_get_default();
    const bool restore_invalidation =
        display != nullptr && lv_display_is_invalidation_enabled(display);
    if (restore_invalidation) lv_display_enable_invalidation(display, false);

    g_root = lv_obj_create(screen);
    if (g_root == nullptr) {
        if (restore_invalidation) lv_display_enable_invalidation(display, true);
        return ESP_ERR_NO_MEM;
    }
    ui_common_lock_object(g_root);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_root, 460, 460);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(kBgRgb), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);

    g_header_back = lv_button_create(g_root);
    if (g_header_back != nullptr) {
        ui_common_lock_object(g_header_back);
        lv_obj_set_size(g_header_back, 58, 42);
        lv_obj_align(g_header_back, LV_ALIGN_TOP_LEFT, 14, 10);
        lv_obj_set_style_radius(g_header_back, 21, 0);
        lv_obj_set_style_border_width(g_header_back, 0, 0);
        lv_obj_set_style_bg_color(g_header_back, lv_color_hex(0x151B24), 0);
        lv_obj_set_style_bg_opa(g_header_back, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(g_header_back, back_click_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *back = make_label(g_header_back, "<", kHeaderRgb, LV_TEXT_ALIGN_CENTER);
        if (back != nullptr) lv_obj_center(back);
    }

    g_header_title = make_label(g_root, "设置", kHeaderRgb, LV_TEXT_ALIGN_CENTER);
    if (g_header_title != nullptr) {
        lv_obj_set_width(g_header_title, 280);
        lv_label_set_long_mode(g_header_title, LV_LABEL_LONG_DOT);
        lv_obj_align(g_header_title, LV_ALIGN_TOP_MID, 0, 18);
    }

    g_header_line = lv_obj_create(g_root);
    if (g_header_line != nullptr) {
        ui_common_lock_object(g_header_line);
        lv_obj_set_size(g_header_line, 400, 1);
        lv_obj_align(g_header_line, LV_ALIGN_TOP_MID, 0, kHeaderHeight - 1);
        lv_obj_set_style_border_width(g_header_line, 0, 0);
        lv_obj_set_style_bg_color(g_header_line, lv_color_hex(0x202938), 0);
        lv_obj_set_style_bg_opa(g_header_line, LV_OPA_COVER, 0);
    }

    g_content = lv_obj_create(g_root);
    if (g_content != nullptr) {
        ui_common_lock_object(g_content);
        lv_obj_set_size(g_content, kContentWidth, kContentHeight);
        lv_obj_set_pos(g_content, kContentX, kContentTop);
        lv_obj_set_style_radius(g_content, 0, 0);
        lv_obj_set_style_border_width(g_content, 0, 0);
        lv_obj_set_style_pad_all(g_content, 0, 0);
        lv_obj_set_style_bg_opa(g_content, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);
    }


    g_timer = lv_timer_create(gesture_timer_cb, 20U, nullptr);
    g_refresh_timer = lv_timer_create(refresh_timer_cb, kRefreshPeriodMs, nullptr);

    if (g_header_back == nullptr || g_header_title == nullptr || g_header_line == nullptr ||
        g_content == nullptr || g_timer == nullptr || g_refresh_timer == nullptr) {
        if (g_refresh_timer != nullptr) {
            lv_timer_delete(g_refresh_timer);
            g_refresh_timer = nullptr;
        }
        if (g_timer != nullptr) {
            lv_timer_delete(g_timer);
            g_timer = nullptr;
        }
        if (g_root != nullptr) {
            lv_obj_delete(g_root);
            g_root = nullptr;
        }
        g_header_back = nullptr;
        g_header_title = nullptr;
        g_header_line = nullptr;
        g_content = nullptr;
        if (restore_invalidation) lv_display_enable_invalidation(display, true);
        return ESP_ERR_NO_MEM;
    }

    lv_timer_pause(g_timer);
    lv_timer_pause(g_refresh_timer);
    show_page(SettingsPage::Main);
    if (restore_invalidation) lv_display_enable_invalidation(display, true);
    ESP_LOGI(TAG, "Settings create完成：连接/应用/系统/关于 2x2 四宫格 + 可滚动详情列表");
    return ESP_OK;
}

static esp_err_t settings_enter()
{
    if (g_root == nullptr || g_timer == nullptr) return ESP_ERR_INVALID_STATE;
    gesture_router_reset();
    DeviceSettingsSnapshot settings = {};
    if (device_settings_get_snapshot(&settings)) {
        g_brightness_saved_level = settings.brightness_level;
        g_brightness_pending_level = settings.brightness_level;
        g_music_scope_saved = settings.music_list_scope;
        g_music_scope_pending = settings.music_list_scope;
    } else {
        g_brightness_saved_level = screen_lock_simple_get_normal_brightness();
        g_brightness_pending_level = g_brightness_saved_level;
    }
    g_brightness_dirty = false;
    g_music_scope_dirty = false;
    show_page(SettingsPage::Main);
    lv_timer_reset(g_timer);
    lv_timer_resume(g_timer);
    if (g_refresh_timer != nullptr) {
        lv_timer_reset(g_refresh_timer);
        lv_timer_resume(g_refresh_timer);
    }
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    lv_obj_invalidate(g_root);
    ESP_LOGI(TAG, "设置进入Foreground：Music可继续后台播放");
    return ESP_OK;
}

static esp_err_t settings_leave(AppRunState next_state)
{
    if (next_state != AppRunState::Stopped) return ESP_ERR_NOT_SUPPORTED;
    // 会话内只预览/滚选；真正离开Settings时才提交一次NVS并切换播放器范围。
    music_list_commit_on_exit();
    brightness_commit_on_exit();
    if (g_timer != nullptr) lv_timer_pause(g_timer);
    if (g_refresh_timer != nullptr) lv_timer_pause(g_refresh_timer);
    gesture_router_set_vertical_adjust_enabled(false);
    gesture_router_set_vertical_adjust_edges_reserved(false);
    gesture_router_reset();
    app_launcher_overlay_hide();
    set_visible(g_root, false);
    ESP_LOGI(TAG, "设置离开Foreground：准备释放私有UI");
    return ESP_OK;
}

static void settings_destroy()
{
    // 防御性兜底：若生命周期异常直接destroy，也只在dirty时提交一次。
    music_list_commit_on_exit();
    brightness_commit_on_exit();
    gesture_router_set_control_capture(false);
    gesture_router_set_vertical_adjust_enabled(false);
    gesture_router_set_vertical_adjust_edges_reserved(false);
    app_launcher_overlay_destroy();
    if (g_refresh_timer != nullptr) {
        lv_timer_delete(g_refresh_timer);
        g_refresh_timer = nullptr;
    }
    if (g_timer != nullptr) {
        lv_timer_delete(g_timer);
        g_timer = nullptr;
    }
    if (g_root != nullptr) {
        lv_obj_delete(g_root);
        g_root = nullptr;
    }
    g_header_back = nullptr;
    g_header_title = nullptr;
    g_header_line = nullptr;
    g_content = nullptr;
    clear_detail_value_refs();
    g_detail_scroll_y = 0;
    g_detail_scroll_max_y = 0;
    g_detail_inertia_active = false;
    g_detail_scroll_velocity_px_s = 0;
    g_pending_page = SettingsPage::Main;
    g_page = SettingsPage::Main;
    ESP_LOGI(TAG, "Settings destroy完成");
}

} // namespace

esp_err_t settings_app_register()
{
    AppDescriptor descriptor = {};
    descriptor.id = AppId::Settings;
    descriptor.name = "设置";
    descriptor.supports_background = false;
    descriptor.lifecycle.create = settings_create;
    descriptor.lifecycle.enter = settings_enter;
    descriptor.lifecycle.leave = settings_leave;
    descriptor.lifecycle.destroy = settings_destroy;

    const esp_err_t ret = app_manager_register(descriptor);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "设置APP已注册：2x2 四宫格主页 + 设备设置NVS模型");
    }
    return ret;
}
