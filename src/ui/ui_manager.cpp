#include "ui_manager.h"

#include <stdint.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "board_pins.h"
#include "cst820.h"
#include "display.h"

static const char *TAG = "界面";
static lv_display_t *g_display = nullptr;
static lv_indev_t *g_touch = nullptr;
static lv_obj_t *g_touch_label = nullptr;
static lv_obj_t *g_touch_marker = nullptr;
static lv_obj_t *g_button_label = nullptr;
static lv_obj_t *g_screen = nullptr;
static bool g_ready = false;
static bool g_scroll_guard_active = false;
static bool g_touch_active = false;
static uint16_t g_touch_raw_x = 0;
static uint16_t g_touch_raw_y = 0;
static uint16_t g_touch_screen_x = 0;
static uint16_t g_touch_screen_y = 0;

// CO5300 对局部刷新窗口有偶数对齐要求：
// 起始 X/Y 必须为偶数，刷新宽度和高度也必须为偶数。
// LVGL 的文字、按钮、触摸点会产生任意大小的脏矩形，
// 如果不做对齐，可能出现花屏、错位和残影。
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
}

// 当前 Stage 5 测试页只验证点击和坐标，不允许任何对象参与滚动。
static void ui_disable_object_scroll(lv_obj_t *obj)
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
}

// 如果 LVGL 仍产生滚动事件，立即把对象拉回原点。
static void ui_scroll_guard_event_cb(lv_event_t *event)
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

// 将原始坐标限制到当前屏幕范围，后续再根据实测决定是否交换/镜像。
static uint16_t ui_clamp_coord(uint16_t value, uint16_t max_value)
{
    return value > max_value ? max_value : value;
}

// CST820 -> LVGL 指针输入。
static void ui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    CST820Point point = {};
    esp_err_t ret = cst820_read_point(&point);

    if (ret == ESP_OK && point.pressed) {
        g_touch_active = true;
        g_touch_raw_x = point.x;
        g_touch_raw_y = point.y;
        g_touch_screen_x = ui_clamp_coord(point.x, FAKEPOD_LCD_WIDTH - 1);
        g_touch_screen_y = ui_clamp_coord(point.y, FAKEPOD_LCD_HEIGHT - 1);
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = g_touch_screen_x;
        data->point.y = g_touch_screen_y;
        return;
    }

    g_touch_active = false;
    data->state = LV_INDEV_STATE_RELEASED;
}

static void ui_test_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    ESP_LOGI(TAG, "LVGL 按钮点击成功，触摸链路正常");
    if (g_button_label != nullptr) {
        lv_label_set_text(g_button_label, "TOUCH OK");
    }
}

static void ui_touch_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_touch_label != nullptr) {
        if (g_touch_active) {
            lv_label_set_text_fmt(
                g_touch_label,
                "Raw touch: %u, %u",
                static_cast<unsigned>(g_touch_raw_x),
                static_cast<unsigned>(g_touch_raw_y)
            );
        } else {
            lv_label_set_text(g_touch_label, "Raw touch: --- , ---");
        }
    }

    if (g_touch_marker == nullptr) {
        return;
    }

    if (g_touch_active) {
        lv_obj_clear_flag(g_touch_marker, LV_OBJ_FLAG_HIDDEN);
        // 触摸点始终限制在屏幕内部，避免子对象越界扩大可滚动区域。
        int32_t marker_x = static_cast<int32_t>(g_touch_screen_x) - 7;
        int32_t marker_y = static_cast<int32_t>(g_touch_screen_y) - 7;
        const int32_t marker_max_x = FAKEPOD_LCD_WIDTH - 14;
        const int32_t marker_max_y = FAKEPOD_LCD_HEIGHT - 14;

        if (marker_x < 0) marker_x = 0;
        if (marker_y < 0) marker_y = 0;
        if (marker_x > marker_max_x) marker_x = marker_max_x;
        if (marker_y > marker_max_y) marker_y = marker_max_y;

        lv_obj_set_pos(g_touch_marker, marker_x, marker_y);
    } else {
        lv_obj_add_flag(g_touch_marker, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_create_stage5_screen()
{
    g_screen = lv_screen_active();
    ui_disable_object_scroll(g_screen);
    lv_obj_add_event_cb(g_screen, ui_scroll_guard_event_cb, LV_EVENT_SCROLL, nullptr);
    lv_obj_add_event_cb(g_screen, ui_scroll_guard_event_cb, LV_EVENT_SCROLL_END, nullptr);

    lv_obj_set_style_bg_color(g_screen, lv_color_hex(0x10131A), 0);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(g_screen);
    ui_disable_object_scroll(title);
    lv_label_set_text(title, "FakePod Custom");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t *subtitle = lv_label_create(g_screen);
    ui_disable_object_scroll(subtitle);
    lv_label_set_text(subtitle, "LVGL 9 + CST820");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x9EA7B3), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 105);

    g_touch_label = lv_label_create(g_screen);
    ui_disable_object_scroll(g_touch_label);
    lv_label_set_text(g_touch_label, "Raw touch: --- , ---");
    lv_obj_set_style_text_color(g_touch_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(g_touch_label, LV_ALIGN_CENTER, 0, -25);

    lv_obj_t *button = lv_button_create(g_screen);
    ui_disable_object_scroll(button);
    lv_obj_set_size(button, 220, 72);
    lv_obj_align(button, LV_ALIGN_CENTER, 0, 85);
    lv_obj_add_event_cb(button, ui_test_button_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(button, ui_scroll_guard_event_cb, LV_EVENT_SCROLL, nullptr);
    lv_obj_add_event_cb(button, ui_scroll_guard_event_cb, LV_EVENT_SCROLL_END, nullptr);

    g_button_label = lv_label_create(button);
    ui_disable_object_scroll(g_button_label);
    lv_label_set_text(g_button_label, "TOUCH TEST");
    lv_obj_center(g_button_label);

    g_touch_marker = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_touch_marker);
    ui_disable_object_scroll(g_touch_marker);
    lv_obj_set_size(g_touch_marker, 14, 14);
    lv_obj_set_style_radius(g_touch_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_touch_marker, lv_color_hex(0xFF3B30), 0);
    lv_obj_set_style_bg_opa(g_touch_marker, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_touch_marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_touch_marker, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(ui_touch_timer_cb, 100, nullptr);
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
    lvgl_cfg.task_priority = 4;
    lvgl_cfg.task_stack = 6144;
    lvgl_cfg.task_affinity = -1;
    lvgl_cfg.task_max_sleep_ms = 100;
    lvgl_cfg.timer_period_ms = 5;

    esp_err_t ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL Port 初始化失败：%s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "正在注册 CO5300 显示设备");
    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = display_get_panel_io();
    disp_cfg.panel_handle = display_get_panel();
    disp_cfg.buffer_size = FAKEPOD_LCD_WIDTH * 20;
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

    g_display = lvgl_port_add_disp(&disp_cfg);
    if (g_display == nullptr) {
        ESP_LOGE(TAG, "注册 LVGL 显示设备失败");
        return ESP_FAIL;
    }

    // CO5300 的 CASET/RASET 对局部刷新矩形有偶数对齐要求。
    lv_display_add_event_cb(
        g_display,
        ui_display_align_area_cb,
        LV_EVENT_INVALIDATE_AREA,
        nullptr
    );
    ESP_LOGI(TAG, "已启用 CO5300 局部刷新偶数对齐");

    ESP_LOGI(TAG, "正在注册 CST820 触摸输入");
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "获取 LVGL 锁失败");
        return ESP_FAIL;
    }

    g_touch = lv_indev_create();
    if (g_touch == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "创建 LVGL 触摸输入失败");
        return ESP_ERR_NO_MEM;
    }

    lv_indev_set_type(g_touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_touch, ui_touch_read_cb);
    lv_indev_set_display(g_touch, g_display);

    // 将滚动启动阈值提高到 LVGL 9.2 允许的最大值，
    // 并在输入设备层直接拦截真正的滚动事件。
    lv_indev_set_scroll_limit(g_touch, 255);
    lv_indev_add_event_cb(g_touch, ui_touch_block_scroll_cb, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_indev_add_event_cb(g_touch, ui_touch_block_scroll_cb, LV_EVENT_SCROLL, nullptr);

    ui_create_stage5_screen();
    ESP_LOGI(TAG, "Stage 5 测试页已启用输入层硬性禁滚动");
    lvgl_port_unlock();

    g_ready = true;
    ESP_LOGI(TAG, "LVGL 用户界面初始化成功");
    return ESP_OK;
}

bool ui_manager_is_ready()
{
    return g_ready;
}
