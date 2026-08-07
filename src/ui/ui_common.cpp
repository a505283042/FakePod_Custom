#include "ui_common.h"

#include "esp_log.h"

static const char *TAG = "界面";
static bool g_scroll_guard_active = false;

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
