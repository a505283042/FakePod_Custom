#include "ui_common.h"

#include "esp_log.h"
#include "font/font_manager.h"

static const char *TAG = "界面";
static bool g_scroll_guard_active = false;

static constexpr uint32_t UI_COMMON_NOTICE_MS = 1800U;
static lv_obj_t *g_notice_card = nullptr;
static lv_obj_t *g_notice_title = nullptr;
static lv_obj_t *g_notice_detail = nullptr;
static lv_timer_t *g_notice_timer = nullptr;

static void ui_common_notice_timer_cb(lv_timer_t *timer)
{
    if (g_notice_card != nullptr && lv_obj_is_valid(g_notice_card)) {
        lv_obj_add_flag(g_notice_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (timer != nullptr) {
        lv_timer_pause(timer);
    }
}

static bool ui_common_notice_ensure_created()
{
    if (g_notice_card != nullptr && lv_obj_is_valid(g_notice_card)) {
        return true;
    }

    lv_obj_t *layer = lv_layer_top();
    if (layer == nullptr) return false;

    g_notice_card = lv_obj_create(layer);
    if (g_notice_card == nullptr) return false;
    ui_common_lock_object(g_notice_card);
    lv_obj_set_size(g_notice_card, 350, 104);
    lv_obj_align(g_notice_card, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_radius(g_notice_card, 16, 0);
    lv_obj_set_style_bg_color(g_notice_card, lv_color_hex(0x171C24), 0);
    lv_obj_set_style_bg_opa(g_notice_card, LV_OPA_90, 0);
    lv_obj_set_style_border_width(g_notice_card, 1, 0);
    lv_obj_set_style_border_color(g_notice_card, lv_color_hex(0x3B4655), 0);
    lv_obj_set_style_pad_all(g_notice_card, 0, 0);
    lv_obj_remove_flag(g_notice_card, LV_OBJ_FLAG_CLICKABLE);

    g_notice_title = lv_label_create(g_notice_card);
    g_notice_detail = lv_label_create(g_notice_card);
    if (g_notice_title == nullptr || g_notice_detail == nullptr) {
        lv_obj_delete(g_notice_card);
        g_notice_card = nullptr;
        g_notice_title = nullptr;
        g_notice_detail = nullptr;
        return false;
    }

    const lv_font_t *font = font_manager_get_ui_font();
    lv_obj_set_width(g_notice_title, 318);
    lv_obj_set_style_text_align(g_notice_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_notice_title, lv_color_hex(0xF2F3F5), 0);
    if (font != nullptr) lv_obj_set_style_text_font(g_notice_title, font, 0);
    lv_obj_align(g_notice_title, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_set_width(g_notice_detail, 318);
    lv_obj_set_style_text_align(g_notice_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_notice_detail, lv_color_hex(0x9CA9B8), 0);
    if (font != nullptr) lv_obj_set_style_text_font(g_notice_detail, font, 0);
    lv_obj_align(g_notice_detail, LV_ALIGN_BOTTOM_MID, 0, -18);

    lv_obj_add_flag(g_notice_card, LV_OBJ_FLAG_HIDDEN);
    if (g_notice_timer == nullptr) {
        g_notice_timer = lv_timer_create(ui_common_notice_timer_cb, UI_COMMON_NOTICE_MS, nullptr);
        if (g_notice_timer != nullptr) lv_timer_pause(g_notice_timer);
    }
    return true;
}

void ui_common_show_notice(const char *title, const char *detail)
{
    if (!ui_common_notice_ensure_created()) return;

    lv_label_set_text(g_notice_title, title != nullptr ? title : "");
    lv_label_set_text(g_notice_detail, detail != nullptr ? detail : "");
    lv_obj_remove_flag(g_notice_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_notice_card);
    lv_obj_invalidate(g_notice_card);

    if (g_notice_timer != nullptr) {
        lv_timer_reset(g_notice_timer);
        lv_timer_resume(g_notice_timer);
    }
}

// 如果 LVGL 仍产生滚动偏移，立即把对象拉回原点。
static void ui_common_scroll_guard_cb(lv_event_t *event)
{
    if (g_scroll_guard_active) {
        return;
    }

    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_target(event));
    if (obj == nullptr) {
        return;
    }

    const int32_t scroll_x = lv_obj_get_scroll_x(obj);
    const int32_t scroll_y = lv_obj_get_scroll_y(obj);
    if (scroll_x == 0 && scroll_y == 0) {
        return;
    }

    g_scroll_guard_active = true;
    lv_obj_scroll_to(obj, 0, 0, LV_ANIM_OFF);
    g_scroll_guard_active = false;
    ESP_LOGW(TAG, "已拦截异常界面滚动：X=%ld Y=%ld", static_cast<long>(scroll_x), static_cast<long>(scroll_y));
}

void ui_common_lock_object(lv_obj_t *obj)
{
    if (obj == nullptr) {
        return;
    }

    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
    lv_obj_set_scroll_snap_x(obj, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scroll_snap_y(obj, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(obj, ui_common_scroll_guard_cb, LV_EVENT_SCROLL, nullptr);
    lv_obj_add_event_cb(obj, ui_common_scroll_guard_cb, LV_EVENT_SCROLL_END, nullptr);
}
