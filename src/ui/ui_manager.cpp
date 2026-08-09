#include "ui_manager.h"

#include <stdint.h>

#include "esp_log.h"
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
// DOWN/UP 必达；MOVE 只在坐标 sequence 真正变化时分发给业务层，避免同一快照反复喂给页面。
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

    lv_display_add_event_cb(g_display, ui_display_align_area_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
    ESP_LOGI(TAG, "已启用 CO5300 局部刷新偶数对齐");

    ESP_LOGI(TAG, "正在注册 CST820 触摸输入");
    gesture_router_reset();
    const esp_err_t touch_fast_ret = ui_touch_input_start();
    if (touch_fast_ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch Fast Path 启动失败，将降级为 LVGL 同步读取 CST820：%s",
            esp_err_to_name(touch_fast_ret));
    }
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
    lvgl_port_unlock();

    g_ready = true;
    ESP_LOGI(TAG, "Stage 6 正式 UI 基础框架初始化成功");
    return ESP_OK;
}

bool ui_manager_is_ready()
{
    return g_ready;
}
