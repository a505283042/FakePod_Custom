#include "app_launcher_overlay.h"

#include <algorithm>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gesture/gesture_router.h"
#include "ui/screens/player_home.h"
#include "ui_common.h"

namespace {

static const char *TAG = "APP圆环";
static constexpr int32_t kAnimProgressMax = 1000;
static constexpr uint32_t kAnimDurationMs = 300U;
static constexpr lv_opa_t kBackdropOpa = 142;
static constexpr int16_t kTapMaxMovePx = 12;
static constexpr uint8_t kInvalidFrame = 0xFFU;

enum class MotionState : uint8_t { Hidden = 0, Entering, Shown, Leaving };

static lv_obj_t *g_parent = nullptr;
static lv_obj_t *g_overlay = nullptr;
static lv_obj_t *g_backdrop = nullptr;
static lv_obj_t *g_panel = nullptr;
static lv_obj_t *g_ring_image = nullptr;
static lv_obj_t *g_center_image = nullptr;
static uint8_t *g_ring_buffer = nullptr;    // PSRAM: I4 palette + packed pixels ~=56.5KiB
static uint8_t *g_center_buffer = nullptr;  // PSRAM: 120x120 I2 palette + packed pixels ~=3.6KiB
static lv_image_dsc_t g_ring_dsc = {};
static lv_image_dsc_t g_center_dsc = {};
static AppId g_selected = AppId::Ebook;
static uint8_t g_frame_index = kInvalidFrame;
static int32_t g_anim_progress = 0;
static bool g_visible = false;
static MotionState g_motion = MotionState::Hidden;
static AppId g_pending_target = AppId::None;
static bool g_switch_scheduled = false;

static bool render_frame(uint8_t frame_index)
{
    if (g_ring_image == nullptr || g_center_image == nullptr ||
        g_ring_buffer == nullptr || g_center_buffer == nullptr) return false;

    const esp_err_t ring_ret = player_home_launcher_shared_render_i4(
        g_selected, frame_index,
        g_ring_buffer, PLAYER_HOME_LAUNCHER_I4_IMAGE_BYTES, &g_ring_dsc);
    if (ring_ret != ESP_OK) {
        ESP_LOGE(TAG, "Music共享I4帧失败：frame=%u ret=%s",
            static_cast<unsigned>(frame_index), esp_err_to_name(ring_ret));
        return false;
    }
    const int32_t progress = player_home_launcher_shared_frame_progress(frame_index);
    const esp_err_t center_ret = player_home_launcher_shared_render_center_i2(
        g_selected, progress,
        g_center_buffer, PLAYER_HOME_LAUNCHER_CENTER_IMAGE_BYTES, &g_center_dsc);
    if (center_ret != ESP_OK) {
        ESP_LOGE(TAG, "Music共享中心图标失败：frame=%u ret=%s",
            static_cast<unsigned>(frame_index), esp_err_to_name(center_ret));
        return false;
    }

    g_frame_index = frame_index;
    g_anim_progress = progress;
    lv_image_set_src(g_ring_image, &g_ring_dsc);
    lv_image_set_src(g_center_image, &g_center_dsc);
    lv_obj_invalidate(g_ring_image);
    lv_obj_invalidate(g_center_image);
    return true;
}

static void anim_exec(void *var, int32_t value)
{
    (void)var;
    const int32_t progress = std::max<int32_t>(0, std::min<int32_t>(value, kAnimProgressMax));
    const uint8_t frame = player_home_launcher_shared_frame_for_progress(progress);
    if (frame != g_frame_index) (void)render_frame(frame);
}

static void show_done(lv_anim_t *anim)
{
    (void)anim;
    if (g_visible && g_motion == MotionState::Entering) g_motion = MotionState::Shown;
}

static void hide_done(lv_anim_t *anim)
{
    (void)anim;
    g_visible = false;
    g_motion = MotionState::Hidden;
    g_anim_progress = 0;
    g_frame_index = kInvalidFrame;
    if (g_overlay != nullptr) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void start_animation(int32_t from, int32_t to, lv_anim_completed_cb_t completed)
{
    if (g_ring_image == nullptr) return;
    lv_anim_delete(g_ring_image, anim_exec);
    lv_anim_t anim = {};
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, g_ring_image);
    lv_anim_set_exec_cb(&anim, anim_exec);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, kAnimDurationMs);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, completed);
    lv_anim_start(&anim);
}

static void switch_app_async(void *user_data)
{
    (void)user_data;
    const AppId target = g_pending_target;
    g_pending_target = AppId::None;
    g_switch_scheduled = false;
    if (target == AppId::None || target == app_manager_foreground() ||
        !app_manager_is_registered(target)) return;
    const esp_err_t ret = app_manager_request_foreground(
        target, AppTransitionMode::PreserveBackground);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "共享圆环进入APP失败：name=%s ret=%s",
            app_manager_name(target), esp_err_to_name(ret));
    }
}

static void activate_index(uint8_t index, int32_t x, int32_t y)
{
    bool should_launch = false;
    const AppId target = player_home_launcher_shared_select_index(index, &should_launch);
    if (target == AppId::None) return;
    g_selected = target;
    (void)render_frame(g_frame_index == kInvalidFrame ? 0U : g_frame_index);
    ESP_LOGI(TAG, "Music共享圆环命中：index=%u name=%s touch=(%ld,%ld) launch=%s",
        static_cast<unsigned>(index), app_manager_name(target),
        static_cast<long>(x), static_cast<long>(y), should_launch ? "YES" : "NO");

    if (!should_launch) return;
    g_pending_target = target;
    if (!g_switch_scheduled) {
        g_switch_scheduled = true;
        lv_async_call(switch_app_async, nullptr);
    }
}

static void control_capture_cb(lv_event_t *event)
{
    if (event == nullptr) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        gesture_router_set_control_capture(true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        gesture_router_set_control_capture(false);
    }
}

static bool click_is_valid_tap(lv_event_t *event)
{
    return event != nullptr && lv_event_get_code(event) == LV_EVENT_CLICKED &&
        g_visible && g_motion == MotionState::Shown &&
        !gesture_router_should_suppress_click() &&
        gesture_router_press_was_tap(kTapMaxMovePx);
}

static void backdrop_click_cb(lv_event_t *event)
{
    if (!click_is_valid_tap(event)) return;
    app_launcher_overlay_hide();
}

static void panel_click_cb(lv_event_t *event)
{
    if (!click_is_valid_tap(event)) return;
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) return;

    lv_point_t point = {};
    lv_indev_get_point(indev, &point);
    lv_area_t panel_coords = {};
    lv_obj_get_coords(g_panel, &panel_coords);
    const int8_t hit = player_home_launcher_shared_hit_test(
        point.x, point.y, panel_coords.x1, panel_coords.y1);
    if (hit < 0) return;
    activate_index(static_cast<uint8_t>(hit), point.x, point.y);
}

static esp_err_t ensure_created(lv_obj_t *parent)
{
    if (parent == nullptr) return ESP_ERR_INVALID_ARG;
    if (g_overlay != nullptr && g_parent == parent) return ESP_OK;
    app_launcher_overlay_destroy();

    g_ring_buffer = static_cast<uint8_t *>(heap_caps_malloc(
        PLAYER_HOME_LAUNCHER_I4_IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_center_buffer = static_cast<uint8_t *>(heap_caps_malloc(
        PLAYER_HOME_LAUNCHER_CENTER_IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_ring_buffer == nullptr || g_center_buffer == nullptr) {
        app_launcher_overlay_destroy();
        return ESP_ERR_NO_MEM;
    }

    g_parent = parent;
    g_overlay = lv_obj_create(parent);
    if (g_overlay == nullptr) {
        app_launcher_overlay_destroy();
        return ESP_ERR_NO_MEM;
    }
    ui_common_lock_object(g_overlay);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g_overlay, 460, 460);
    lv_obj_set_pos(g_overlay, 0, 0);
    lv_obj_set_style_radius(g_overlay, 0, 0);
    lv_obj_set_style_border_width(g_overlay, 0, 0);
    lv_obj_set_style_shadow_width(g_overlay, 0, 0);
    lv_obj_set_style_pad_all(g_overlay, 0, 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_CLICKABLE);

    // 与 Music Launcher 相同的输入层级：全屏 backdrop 负责收起，
    // 340x340 panel 独占圆环点击；不再在单个全屏对象上手写 PRESSED/RELEASED。
    g_backdrop = lv_obj_create(g_overlay);
    if (g_backdrop == nullptr) {
        app_launcher_overlay_destroy();
        return ESP_ERR_NO_MEM;
    }
    ui_common_lock_object(g_backdrop);
    lv_obj_set_pos(g_backdrop, 0, 0);
    lv_obj_set_size(g_backdrop, 460, 460);
    lv_obj_set_style_radius(g_backdrop, 0, 0);
    lv_obj_set_style_border_width(g_backdrop, 0, 0);
    lv_obj_set_style_shadow_width(g_backdrop, 0, 0);
    lv_obj_set_style_pad_all(g_backdrop, 0, 0);
    lv_obj_set_style_bg_color(g_backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_backdrop, kBackdropOpa, 0);
    lv_obj_add_flag(g_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_backdrop, backdrop_click_cb, LV_EVENT_CLICKED, nullptr);

    g_panel = lv_obj_create(g_overlay);
    if (g_panel == nullptr) {
        app_launcher_overlay_destroy();
        return ESP_ERR_NO_MEM;
    }
    ui_common_lock_object(g_panel);
    lv_obj_set_pos(g_panel, PLAYER_HOME_LAUNCHER_PANEL_X, PLAYER_HOME_LAUNCHER_PANEL_Y);
    lv_obj_set_size(g_panel, PLAYER_HOME_LAUNCHER_PANEL_SIZE, PLAYER_HOME_LAUNCHER_PANEL_SIZE);
    lv_obj_set_style_radius(g_panel, 0, 0);
    lv_obj_set_style_bg_opa(g_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_panel, 0, 0);
    lv_obj_set_style_shadow_width(g_panel, 0, 0);
    lv_obj_set_style_pad_all(g_panel, 0, 0);
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_panel, control_capture_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(g_panel, panel_click_cb, LV_EVENT_CLICKED, nullptr);

    g_ring_image = lv_image_create(g_panel);
    if (g_ring_image == nullptr) {
        app_launcher_overlay_destroy();
        return ESP_ERR_NO_MEM;
    }
    ui_common_lock_object(g_ring_image);
    lv_obj_remove_flag(g_ring_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(g_ring_image, 0, 0);

    g_center_image = lv_image_create(g_panel);
    if (g_center_image == nullptr) {
        app_launcher_overlay_destroy();
        return ESP_ERR_NO_MEM;
    }
    ui_common_lock_object(g_center_image);
    lv_obj_remove_flag(g_center_image, LV_OBJ_FLAG_CLICKABLE);
    const int32_t center_pos =
        (PLAYER_HOME_LAUNCHER_PANEL_SIZE - PLAYER_HOME_LAUNCHER_CENTER_IMAGE_SIZE) / 2;
    lv_obj_set_pos(g_center_image, center_pos, center_pos);

    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    return ESP_OK;
}

} // namespace

esp_err_t app_launcher_overlay_show(lv_obj_t *parent, AppId selected)
{
    if (parent == nullptr || selected == AppId::None) return ESP_ERR_INVALID_ARG;
    const esp_err_t create_ret = ensure_created(parent);
    if (create_ret != ESP_OK) return create_ret;

    gesture_router_reset();

    // 共享 Launcher 的高亮项以真实前台 APP 为准。selected 仅保留为调用期 fallback，
    // 避免某个 APP 以后复制/修改调用代码时传入旧值，导致“在哪个 APP 打开却选中别的 APP”。
    const AppId foreground = app_manager_foreground();
    const bool foreground_valid =
        foreground != AppId::None && app_manager_is_registered(foreground);
    g_selected = foreground_valid ? foreground : selected;
    app_manager_set_launcher_target(g_selected);
    g_visible = true;
    g_motion = MotionState::Entering;
    g_frame_index = kInvalidFrame;
    g_anim_progress = 0;
    if (!render_frame(0U)) return ESP_FAIL;
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_overlay);
    lv_obj_invalidate(g_overlay);
    start_animation(0, kAnimProgressMax, show_done);
    ESP_LOGI(TAG,
        "共享Music圆环展开：source=%s selected=%s I4=%uB center=%uB PSRAM=%uB",
        app_manager_name(app_manager_foreground()), app_manager_name(g_selected),
        static_cast<unsigned>(PLAYER_HOME_LAUNCHER_I4_IMAGE_BYTES),
        static_cast<unsigned>(PLAYER_HOME_LAUNCHER_CENTER_IMAGE_BYTES),
        static_cast<unsigned>(PLAYER_HOME_LAUNCHER_I4_IMAGE_BYTES +
            PLAYER_HOME_LAUNCHER_CENTER_IMAGE_BYTES));
    return ESP_OK;
}

void app_launcher_overlay_hide()
{
    if (g_overlay == nullptr || g_ring_image == nullptr || !g_visible ||
        g_motion == MotionState::Hidden || g_motion == MotionState::Leaving) return;
    g_motion = MotionState::Leaving;
    start_animation(g_anim_progress, 0, hide_done);
}

void app_launcher_overlay_destroy()
{
    if (g_ring_image != nullptr) lv_anim_delete(g_ring_image, anim_exec);
    g_ring_image = nullptr;
    g_center_image = nullptr;
    g_panel = nullptr;
    g_backdrop = nullptr;
    if (g_overlay != nullptr) {
        lv_obj_delete(g_overlay);
        g_overlay = nullptr;
    }
    if (g_ring_buffer != nullptr) {
        heap_caps_free(g_ring_buffer);
        g_ring_buffer = nullptr;
    }
    if (g_center_buffer != nullptr) {
        heap_caps_free(g_center_buffer);
        g_center_buffer = nullptr;
    }
    g_ring_dsc = {};
    g_center_dsc = {};
    g_parent = nullptr;
    g_selected = AppId::Ebook;
    g_frame_index = kInvalidFrame;
    g_anim_progress = 0;
    g_visible = false;
    g_motion = MotionState::Hidden;
    g_pending_target = AppId::None;
    g_switch_scheduled = false;
}

bool app_launcher_overlay_is_visible()
{
    return g_visible;
}
