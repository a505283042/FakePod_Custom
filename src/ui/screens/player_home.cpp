#include "player_home.h"

#include <stdio.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "audio_service.h"
#include "artwork/now_playing_artwork.h"
#include "board_pins.h"
#include "font/font_manager.h"
#include "gesture/gesture_router.h"
#include "input/touch_input.h"
#include "media_library.h"
#include "media/library/media_catalog_v2.h"
#include "player_control.h"
#include "player_state.h"
#include "library_view.h"
#include "lyrics/lyrics_view.h"
#include "spectrum/spectrum_view.h"
#include "ui_common.h"

static const char *TAG = "首页";

// R.20：播放器默认显示 normal RGB565；后台同时预处理 dimmed RGB565。
// Overlay 打开时优先直接切 dimmed Surface，只有压缩图回退路径才使用半透明黑层。
// AudioTask / Player Transport 仍沿用既有实现。
static constexpr int32_t kProgressScale = 10000;
static constexpr uint32_t kOverlayTimeoutMs = 5000U;
// P1.5.3.2R.7：Overlay 默认只处理点击；纵向音量手势不再随 Overlay 自动启用。
// 用户必须先点击 Overlay 底部的音量图标，才进入纵向音量调节；再次点击或 Overlay 隐藏时退出。
static constexpr lv_opa_t kOverlayDimOpacity = 150U;
// P1.5R.1.2：用户正在触摸或刚松手时，周期性状态/封面刷新主动让路。
static constexpr uint32_t kInteractionYieldMs = 120U;
// 约 220px 的纵向位移覆盖 0~100%。上滑增加，下滑降低；12px 起手阈值由 GestureRouter 负责。
static constexpr int32_t kOverlayVolumeFullScalePx = 220;
// P1.4.3.1：只有中央安全区的轻点可以打开 Overlay。
// 顶部留给曲库下拉，底部留给 Launcher，左右边缘留给页面横滑。
static constexpr int16_t kOverlayTapSafeLeftPx = 36;
static constexpr int16_t kOverlayTapSafeTopPx = 60;
static constexpr int16_t kOverlayTapSafeRightPx = 423;
static constexpr int16_t kOverlayTapSafeBottomPx = 404;
// R.11：Overlay 只能由真正轻点进入。超过 12px 的任意方向移动都视为滑动/拖动，
// 即使没有达到 72px 页面手势触发阈值，也绝不补成 Overlay 点击。
static constexpr int16_t kOverlayTapMaxMovePx = 12;

// P1.5.3.2R.18：Launcher 改为“单对象径向菜单”。
// 外圈扇区、外圈图标、中心圆和中心图标全部在同一个 340x340 对象里绘制；
// 点击也不再依赖 7 个矩形按钮，而是直接根据触摸点的半径/角度映射到对应扇区。
// 这样显示几何与触摸几何只保留一套极坐标，彻底消除“看到A却点中B”的结构性误差。
static constexpr uint32_t kLauncherAccentRgb = 0xFF4FA3;
static constexpr uint32_t kLauncherSelectedSectorRgb = 0x28E6F2;
static constexpr uint32_t kLauncherBackdropRgb = 0x000000;
static constexpr lv_opa_t kLauncherBackdropOpa = 142;
static constexpr uint8_t kLauncherItemCount = 7U;

// 圆环只占屏幕中央 340x340；Backdrop 固定全屏，不参与位移动画。
// 上滑时只移动这个较小对象，可显著减少每帧失效/重绘面积。
static constexpr int16_t kLauncherPanelSize = 340;
static constexpr int16_t kLauncherPanelX = (460 - kLauncherPanelSize) / 2;
static constexpr int16_t kLauncherPanelShownY = (460 - kLauncherPanelSize) / 2;
static constexpr int16_t kLauncherPanelHiddenY = 460;
static constexpr int16_t kLauncherCenterX = kLauncherPanelSize / 2;
static constexpr int16_t kLauncherCenterY = kLauncherPanelSize / 2;
static constexpr uint16_t kLauncherOuterRadius = 150U;
static constexpr int16_t kLauncherRingWidth = 70;
static constexpr int16_t kLauncherInnerRadius =
    static_cast<int16_t>(kLauncherOuterRadius) - kLauncherRingWidth;
static constexpr int16_t kLauncherIconRadius =
    static_cast<int16_t>(kLauncherOuterRadius) - kLauncherRingWidth / 2;
static constexpr int16_t kLauncherSectorHalfSpanDeg = 23;
static constexpr int16_t kLauncherCenterDiameter = 116;
static constexpr int16_t kLauncherTouchInnerRadius = kLauncherInnerRadius - 8;
static constexpr int16_t kLauncherTouchOuterRadius =
    static_cast<int16_t>(kLauncherOuterRadius) + 8;
static constexpr uint32_t kLauncherAnimDurationMs = 300U;

// lv_draw_arc 的角度约定：0°在下、90°在右、180°在上、270°在左。
enum class LauncherIconKind : uint8_t {
    Music = 0,
    Nsf,
    MicSpectrum,
    Mjpg,
    Picture,
    Ebook,
    Settings,
};

enum class LauncherMotionState : uint8_t {
    Hidden = 0,
    Entering,
    Shown,
    Leaving,
};

struct LauncherMenuItemDef {
    LauncherIconKind icon;
    const char *name;
    int16_t center_angle;
    uint32_t idle_rgb;
    int8_t optical_x;
    int8_t optical_y;
};

// 7 个扇区按约 51.4° 等距分布；扇区宽 46°，相邻之间保留暗缝。
// optical_x/y 只修正不同图形的“视觉重心”，不会改变扇区几何或触摸判定。
static constexpr LauncherMenuItemDef kLauncherItems[kLauncherItemCount] = {
    {LauncherIconKind::Music,       "音乐",     180, 0x3C527F, -5,  0},
    {LauncherIconKind::Nsf,         "NSF播放",  129, 0x334A78,  0,  0},
    {LauncherIconKind::MicSpectrum, "拾音频谱",  77, 0x2B426F, -2,  0},
    {LauncherIconKind::Mjpg,        "MJPG播放",  26, 0x263D69,  0,  0},
    {LauncherIconKind::Picture,     "图片播放", 334, 0x233861,  0,  0},
    {LauncherIconKind::Ebook,       "电子书",   283, 0x2A406C,  0,  0},
    {LauncherIconKind::Settings,    "设置",     231, 0x354B77,  0,  0},
};

// P1.5.2R.4.2：播放页页面级手势统一用白名单过滤。
// TouchInput / GestureRouter 仍识别所有动作；页面层只执行当前上下文允许的动作。
// 这样“禁止歌词下拉曲库”和“频谱只允许右滑返回”共享同一条规则。
enum class PlaybackGestureScope : uint8_t
{
    Home = 0,
    HomeOverlay,
    HomeLauncher,
    Lyrics,
    LyricsOverlay,
    Spectrum,
};

static bool player_home_page_allows_gesture(
    PlaybackGestureScope scope,
    UiGestureAction action)
{
    switch (scope) {
        case PlaybackGestureScope::Home:
            return action == UiGestureAction::SwipeLeft ||
                action == UiGestureAction::SwipeRight ||
                action == UiGestureAction::PullDownFromTop ||
                action == UiGestureAction::PullUpFromBottom;
        case PlaybackGestureScope::Lyrics:
            return action == UiGestureAction::SwipeLeft;
        case PlaybackGestureScope::Spectrum:
            return action == UiGestureAction::SwipeRight;
        case PlaybackGestureScope::HomeOverlay:
        case PlaybackGestureScope::HomeLauncher:
        case PlaybackGestureScope::LyricsOverlay:
        default:
            return false;
    }
}

static lv_obj_t *g_overlay = nullptr;
static lv_obj_t *g_overlay_backdrop = nullptr;
static lv_timer_t *g_overlay_timer = nullptr;
static bool g_overlay_visible = false;
static bool g_overlay_fast_dim = false;
static bool g_overlay_dim_path_valid = false;

static lv_obj_t *g_launcher = nullptr;
static lv_obj_t *g_launcher_backdrop = nullptr;
static lv_obj_t *g_launcher_panel = nullptr;
static bool g_launcher_visible = false;
static LauncherMotionState g_launcher_motion = LauncherMotionState::Hidden;
static uint8_t g_launcher_selected_index = 0U;
static uint32_t g_launcher_anim_started_ms = 0U;

static lv_timer_t *g_audio_timer = nullptr;
static lv_timer_t *g_artwork_timer = nullptr;
static bool g_background_timers_running = true;

static lv_obj_t *g_title = nullptr;
static lv_obj_t *g_artist = nullptr;
static lv_obj_t *g_track_info = nullptr;
static lv_obj_t *g_prev_button = nullptr;
static lv_obj_t *g_play_button = nullptr;
static lv_obj_t *g_next_button = nullptr;
static lv_obj_t *g_play_symbol = nullptr;
static bool g_play_icon_pause = false;
static lv_obj_t *g_progress = nullptr;
static lv_obj_t *g_current_time = nullptr;
static lv_obj_t *g_total_time = nullptr;
static lv_obj_t *g_loop_button = nullptr;
static lv_obj_t *g_volume_label = nullptr;
static lv_obj_t *g_volume_slider = nullptr;
static lv_obj_t *g_volume_mode_button = nullptr;

// Stage 11.2：进度条使用 0~10000 的归一化范围，避免把超长音频毫秒数直接塞进 LVGL int32_t range。
// 拖动期间只做 UI 本地预览；松手时才向 Player 提交一次 Seek。
static bool g_progress_dragging = false;
static bool g_progress_seek_pending = false;
static uint64_t g_progress_total_ms = 0;
static uint64_t g_progress_preview_ms = 0;
static uint32_t g_progress_track_index = UINT32_MAX;
static uint32_t g_progress_playback_revision = 0;
static uint32_t g_progress_seek_base_revision = 0;
static uint32_t g_progress_seek_started_tick = 0;

static bool g_volume_dragging = false;
static bool g_volume_adjust_armed = false;
static uint8_t g_volume_drag_start_percent = 0U;
static uint8_t g_volume_preview_percent = 0U;
static uint32_t g_volume_drag_sequence = 0U;
static uint32_t g_last_audio_state_revision = UINT32_MAX;
// P1.5.3.2R.19：记录主页最后已经绑定的 Player Track。切歌后不再等待 100ms Artwork timer，
// 而是在当前 LVGL 线程立即把预热好的 RGB565 Surface 绑定到主页；20ms watcher 同时覆盖自然 EOF。
static uint32_t g_last_artwork_bound_track = UINT32_MAX;

// Overlay / Launcher 内部 helper 先行声明，供后续多个回调交叉调用。
static void player_home_cancel_progress_interaction();
static void player_home_progress_sync(const AudioStateSnapshot &snapshot);
static void player_home_overlay_hide();

static void player_home_control_capture_cb(lv_event_t *event)
{
    if (event == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        gesture_router_set_control_capture(true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        gesture_router_set_control_capture(false);
    }
}

static bool player_home_click_suppressed()
{
    return gesture_router_should_suppress_click();
}

static lv_obj_t *player_home_create_label(
    lv_obj_t *parent,
    const char *text,
    lv_color_t color,
    const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    ui_common_lock_object(label);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font != nullptr ? font : font_manager_get_ui_font(), 0);
    return label;
}

static lv_obj_t *player_home_create_round_button(lv_obj_t *parent, int32_t size, const char *symbol)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, player_home_control_capture_cb, LV_EVENT_ALL, nullptr);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(button, 36, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);

    lv_obj_t *label = player_home_create_label(
        button, symbol, lv_color_hex(0xFFFFFF), lv_font_default());
    lv_obj_center(label);
    return button;
}

// P1.5.3.2R.10：上一曲/播放/下一曲继续使用 R.9 的大触摸按钮，
// 但不再依赖默认 Symbol 字体的小尺寸 glyph；按钮内部改为更大的轻量线框图标。
// 播放按钮仍保留隐藏 label 作为既有状态载体，自绘图标由 g_play_icon_pause 同步刷新。
static void player_home_transport_icon_draw_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }

    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    if (layer == nullptr || obj == nullptr) {
        return;
    }

    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);
    const int32_t cx = (coords.x1 + coords.x2) / 2;
    const int32_t cy = (coords.y1 + coords.y2) / 2;
    const bool is_play_button = obj == g_play_button;

    lv_draw_line_dsc_t line = {};
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(is_play_button ? 0x111111 : 0xFFFFFF);
    line.width = is_play_button ? 5 : 4;
    line.opa = LV_OPA_COVER;
    line.round_start = 1U;
    line.round_end = 1U;

    auto draw = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        line.p1.x = x0; line.p1.y = y0;
        line.p2.x = x1; line.p2.y = y1;
        lv_draw_line(layer, &line);
    };

    if (obj == g_prev_button) {
        // 34x30 左右的大号 Previous：竖线 + 双折返箭头。
        draw(cx - 17, cy - 15, cx - 17, cy + 15);
        draw(cx + 12, cy - 14, cx - 3, cy);
        draw(cx - 3, cy, cx + 12, cy + 14);
        draw(cx - 1, cy - 14, cx - 16, cy);
        draw(cx - 16, cy, cx - 1, cy + 14);
        return;
    }

    if (obj == g_next_button) {
        // Previous 的镜像，保持三颗按钮内部图标视觉重量一致。
        draw(cx + 17, cy - 15, cx + 17, cy + 15);
        draw(cx - 12, cy - 14, cx + 3, cy);
        draw(cx + 3, cy, cx - 12, cy + 14);
        draw(cx + 1, cy - 14, cx + 16, cy);
        draw(cx + 16, cy, cx + 1, cy + 14);
        return;
    }

    if (obj == g_play_button) {
        if (g_play_icon_pause) {
            // 约 24x34 的大号 Pause。
            draw(cx - 9, cy - 17, cx - 9, cy + 17);
            draw(cx + 9, cy - 17, cx + 9, cy + 17);
        } else {
            // 约 31x36 的大号 Play 三角轮廓。
            draw(cx - 11, cy - 18, cx - 11, cy + 18);
            draw(cx - 11, cy - 18, cx + 16, cy);
            draw(cx + 16, cy, cx - 11, cy + 18);
        }
    }
}

static void player_home_mode_icon_draw_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }

    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    if (layer == nullptr || obj == nullptr) {
        return;
    }

    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);
    const int32_t cx = (coords.x1 + coords.x2) / 2;
    const int32_t cy = (coords.y1 + coords.y2) / 2;

    lv_draw_line_dsc_t line = {};
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(0xF5F7FA);
    line.width = 2;
    line.opa = LV_OPA_COVER;
    line.round_start = 1U;
    line.round_end = 1U;

    auto draw = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        line.p1.x = x0; line.p1.y = y0;
        line.p2.x = x1; line.p2.y = y1;
        lv_draw_line(layer, &line);
    };
    auto arrow_right = [&](int32_t x, int32_t y) {
        draw(x - 6, y - 5, x, y);
        draw(x - 6, y + 5, x, y);
    };
    auto arrow_left = [&](int32_t x, int32_t y) {
        draw(x + 6, y - 5, x, y);
        draw(x + 6, y + 5, x, y);
    };

    switch (player_control_get_loop_mode()) {
        case PlayerLoopMode::Sequential:
            draw(cx - 15, cy, cx + 13, cy);
            arrow_right(cx + 13, cy);
            break;

        case PlayerLoopMode::ListRepeat:
        case PlayerLoopMode::SingleRepeat:
            draw(cx - 13, cy - 9, cx + 11, cy - 9);
            arrow_right(cx + 11, cy - 9);
            draw(cx + 14, cy - 5, cx + 14, cy + 6);
            draw(cx + 13, cy + 9, cx - 11, cy + 9);
            arrow_left(cx - 11, cy + 9);
            draw(cx - 14, cy + 5, cx - 14, cy - 6);
            if (player_control_get_loop_mode() == PlayerLoopMode::SingleRepeat) {
                // 不依赖额外 Unicode glyph，用两根短线在中心画一个小“1”。
                draw(cx, cy - 3, cx, cy + 4);
                draw(cx - 2, cy - 1, cx, cy - 3);
            }
            break;

        case PlayerLoopMode::Shuffle:
            // 两条交叉路径直接自绘 Shuffle 图标，避免当前字体缺少随机播放 glyph。
            draw(cx - 15, cy - 9, cx - 7, cy - 9);
            draw(cx - 7, cy - 9, cx + 6, cy + 9);
            draw(cx + 6, cy + 9, cx + 14, cy + 9);
            arrow_right(cx + 14, cy + 9);

            draw(cx - 15, cy + 9, cx - 7, cy + 9);
            draw(cx - 7, cy + 9, cx + 6, cy - 9);
            draw(cx + 6, cy - 9, cx + 14, cy - 9);
            arrow_right(cx + 14, cy - 9);
            break;
    }
}

static lv_obj_t *player_home_create_mode_button(lv_obj_t *parent)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, player_home_control_capture_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(button, player_home_mode_icon_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_set_size(button, 60, 60);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(button, 32, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    return button;
}

static void player_home_volume_icon_draw_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }

    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    if (layer == nullptr || obj == nullptr) {
        return;
    }

    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);
    const int32_t cx = (coords.x1 + coords.x2) / 2;
    const int32_t cy = (coords.y1 + coords.y2) / 2;

    lv_draw_line_dsc_t line = {};
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(0xF5F7FA);
    line.width = 3;
    line.opa = LV_OPA_COVER;
    line.round_start = 1U;
    line.round_end = 1U;

    auto draw = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        line.p1.x = x0; line.p1.y = y0;
        line.p2.x = x1; line.p2.y = y1;
        lv_draw_line(layer, &line);
    };

    // 直接自绘较大的扬声器图标，避免默认 LV_SYMBOL_AUDIO 在 60px 按钮里显得过小。
    draw(cx - 15, cy - 6, cx - 8, cy - 6);
    draw(cx - 15, cy + 6, cx - 8, cy + 6);
    draw(cx - 15, cy - 6, cx - 15, cy + 6);
    draw(cx - 8, cy - 6, cx + 1, cy - 14);
    draw(cx - 8, cy + 6, cx + 1, cy + 14);
    draw(cx + 1, cy - 14, cx + 1, cy + 14);
    draw(cx + 7, cy - 7, cx + 11, cy - 3);
    draw(cx + 11, cy - 3, cx + 11, cy + 3);
    draw(cx + 11, cy + 3, cx + 7, cy + 7);
    draw(cx + 13, cy - 12, cx + 18, cy - 6);
    draw(cx + 18, cy - 6, cx + 18, cy + 6);
    draw(cx + 18, cy + 6, cx + 13, cy + 12);
}

static lv_obj_t *player_home_create_volume_mode_button(lv_obj_t *parent)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, player_home_control_capture_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(button, player_home_volume_icon_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_set_size(button, 60, 60);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(button, 36, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    return button;
}

static lv_obj_t *player_home_create_pill_button(
    lv_obj_t *parent,
    int32_t width,
    int32_t height,
    const char *text,
    lv_obj_t **out_label)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, player_home_control_capture_cb, LV_EVENT_ALL, nullptr);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(button, 28, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);

    lv_obj_t *label = player_home_create_label(
        button, text, lv_color_hex(0xF5F7FA), font_manager_get_ui_font());
    lv_obj_center(label);
    if (out_label != nullptr) {
        *out_label = label;
    }
    return button;
}

static void player_home_launcher_draw_line(
    lv_layer_t *layer,
    lv_draw_line_dsc_t *line,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1)
{
    line->p1.x = x0;
    line->p1.y = y0;
    line->p2.x = x1;
    line->p2.y = y1;
    lv_draw_line(layer, line);
}

static void player_home_launcher_draw_dot(
    lv_layer_t *layer,
    int32_t cx,
    int32_t cy,
    int32_t diameter,
    lv_color_t color)
{
    lv_draw_rect_dsc_t dot = {};
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = color;
    dot.bg_opa = LV_OPA_COVER;
    dot.radius = LV_RADIUS_CIRCLE;
    dot.border_width = 0;
    const int32_t half = diameter / 2;
    lv_area_t area = {cx - half, cy - half, cx + half, cy + half};
    lv_draw_rect(layer, &dot, &area);
}

static void player_home_launcher_draw_icon(
    lv_layer_t *layer,
    LauncherIconKind icon,
    int32_t cx,
    int32_t cy,
    lv_color_t color,
    int32_t scale_percent)
{
    if (layer == nullptr) {
        return;
    }

    auto s = [scale_percent](int32_t value) -> int32_t {
        const int32_t scaled = (value * scale_percent + (value >= 0 ? 50 : -50)) / 100;
        if (value != 0 && scaled == 0) {
            return value > 0 ? 1 : -1;
        }
        return scaled;
    };

    lv_draw_line_dsc_t line = {};
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = scale_percent >= 120 ? 4 : 3;
    line.opa = LV_OPA_COVER;
    line.round_start = 1U;
    line.round_end = 1U;

    auto draw = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        player_home_launcher_draw_line(
            layer, &line,
            cx + s(x0), cy + s(y0),
            cx + s(x1), cy + s(y1));
    };
    auto dot = [&](int32_t x, int32_t y, int32_t d) {
        player_home_launcher_draw_dot(
            layer, cx + s(x), cy + s(y), s(d) < 3 ? 3 : s(d), color);
    };

    switch (icon) {
        case LauncherIconKind::Music:
            // 双音符：中心圆里的默认选中图标会略放大。
            draw(4, -17, 4, 9);
            draw(4, -17, 16, -20);
            draw(16, -20, 16, 4);
            dot(-2, 10, 10);
            dot(10, 5, 10);
            break;

        case LauncherIconKind::Nsf:
            // 芯片/游戏音源图标：方形主体 + 四周引脚 + 中心脉冲。
            draw(-13, -13, 13, -13);
            draw(13, -13, 13, 13);
            draw(13, 13, -13, 13);
            draw(-13, 13, -13, -13);
            draw(-8, -18, -8, -13);
            draw(0, -18, 0, -13);
            draw(8, -18, 8, -13);
            draw(-8, 13, -8, 18);
            draw(0, 13, 0, 18);
            draw(8, 13, 8, 18);
            draw(-18, -8, -13, -8);
            draw(-18, 0, -13, 0);
            draw(-18, 8, -13, 8);
            draw(13, -8, 18, -8);
            draw(13, 0, 18, 0);
            draw(13, 8, 18, 8);
            draw(-7, 4, -2, -4);
            draw(-2, -4, 3, 4);
            draw(3, 4, 8, -4);
            break;

        case LauncherIconKind::MicSpectrum:
            // 麦克风 + 右侧三根频谱条。
            draw(-11, -14, -11, 6);
            draw(-11, -14, -4, -18);
            draw(-4, -18, 3, -14);
            draw(3, -14, 3, 6);
            draw(3, 6, -4, 10);
            draw(-4, 10, -11, 6);
            draw(-15, 4, -15, 7);
            draw(-15, 7, -9, 13);
            draw(-9, 13, -4, 14);
            draw(-4, 14, 2, 12);
            draw(-4, 14, -4, 19);
            draw(-10, 19, 2, 19);
            draw(8, 10, 8, 17);
            draw(13, 4, 13, 17);
            draw(18, -3, 18, 17);
            break;

        case LauncherIconKind::Mjpg:
            // 视频：矩形画框 + 播放三角。
            draw(-18, -13, 11, -13);
            draw(11, -13, 11, 13);
            draw(11, 13, -18, 13);
            draw(-18, 13, -18, -13);
            draw(11, -7, 18, -12);
            draw(18, -12, 18, 12);
            draw(18, 12, 11, 7);
            draw(-6, -7, -6, 7);
            draw(-6, -7, 5, 0);
            draw(5, 0, -6, 7);
            break;

        case LauncherIconKind::Picture:
            // 图片：相框 + 山峰 + 太阳。
            draw(-18, -15, 18, -15);
            draw(18, -15, 18, 15);
            draw(18, 15, -18, 15);
            draw(-18, 15, -18, -15);
            draw(-14, 10, -5, 0);
            draw(-5, 0, 1, 6);
            draw(1, 6, 8, -4);
            draw(8, -4, 15, 10);
            dot(9, -9, 6);
            break;

        case LauncherIconKind::Ebook:
            // 打开的书本：左右页 + 中缝。
            draw(0, -14, 0, 15);
            draw(-1, -12, -7, -15);
            draw(-7, -15, -18, -12);
            draw(-18, -12, -18, 12);
            draw(-18, 12, -7, 10);
            draw(-7, 10, -1, 13);
            draw(1, -12, 7, -15);
            draw(7, -15, 18, -12);
            draw(18, -12, 18, 12);
            draw(18, 12, 7, 10);
            draw(7, 10, 1, 13);
            break;

        case LauncherIconKind::Settings:
            // 轻量齿轮：中心圆 + 8 个短辐条。
            dot(0, 0, 10);
            draw(0, -18, 0, -11);
            draw(0, 11, 0, 18);
            draw(-18, 0, -11, 0);
            draw(11, 0, 18, 0);
            draw(-13, -13, -8, -8);
            draw(8, 8, 13, 13);
            draw(13, -13, 8, -8);
            draw(-8, 8, -13, 13);
            break;
    }
}

static int16_t player_home_launcher_normalize_angle(int16_t angle)
{
    while (angle < 0) {
        angle = static_cast<int16_t>(angle + 360);
    }
    while (angle >= 360) {
        angle = static_cast<int16_t>(angle - 360);
    }
    return angle;
}

static int16_t player_home_launcher_angle_distance(int16_t lhs, int16_t rhs)
{
    int16_t distance = static_cast<int16_t>(
        player_home_launcher_normalize_angle(lhs) - player_home_launcher_normalize_angle(rhs));
    if (distance < 0) {
        distance = static_cast<int16_t>(-distance);
    }
    if (distance > 180) {
        distance = static_cast<int16_t>(360 - distance);
    }
    return distance;
}

static void player_home_launcher_item_center(
    uint8_t index,
    int32_t *out_x,
    int32_t *out_y)
{
    if (out_x == nullptr || out_y == nullptr || index >= kLauncherItemCount) {
        return;
    }

    // 和扇区绘制使用完全相同的中心角。lv_draw_arc：0°在下、90°在右，
    // 因此 x 用 sin，y 用 cos；图标位于环带厚度中心半径上。
    constexpr float kDegToRad = 0.01745329251994329577f;
    const LauncherMenuItemDef &item = kLauncherItems[index];
    const float radians = static_cast<float>(item.center_angle) * kDegToRad;
    *out_x = kLauncherCenterX +
        lroundf(sinf(radians) * static_cast<float>(kLauncherIconRadius)) + item.optical_x;
    *out_y = kLauncherCenterY +
        lroundf(cosf(radians) * static_cast<float>(kLauncherIconRadius)) + item.optical_y;
}

static void player_home_launcher_panel_draw_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }

    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    if (layer == nullptr || obj == nullptr) {
        return;
    }

    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);
    const int32_t cx = coords.x1 + kLauncherCenterX;
    const int32_t cy = coords.y1 + kLauncherCenterY;

    // 1) 七个扇区。
    for (uint8_t i = 0; i < kLauncherItemCount; ++i) {
        const LauncherMenuItemDef &item = kLauncherItems[i];
        lv_draw_arc_dsc_t arc = {};
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_hex(
            i == g_launcher_selected_index ? kLauncherSelectedSectorRgb : item.idle_rgb);
        arc.width = kLauncherRingWidth;
        arc.start_angle = player_home_launcher_normalize_angle(
            static_cast<int16_t>(item.center_angle - kLauncherSectorHalfSpanDeg));
        arc.end_angle = player_home_launcher_normalize_angle(
            static_cast<int16_t>(item.center_angle + kLauncherSectorHalfSpanDeg));
        arc.center.x = cx;
        arc.center.y = cy;
        arc.radius = kLauncherOuterRadius;
        arc.opa = LV_OPA_COVER;
        arc.rounded = 0U;
        lv_draw_arc(layer, &arc);
    }

    // 2) 七个图标。位置直接由对应扇区同一个 center_angle 计算。
    for (uint8_t i = 0; i < kLauncherItemCount; ++i) {
        int32_t icon_x = kLauncherCenterX;
        int32_t icon_y = kLauncherCenterY;
        player_home_launcher_item_center(i, &icon_x, &icon_y);
        player_home_launcher_draw_icon(
            layer,
            kLauncherItems[i].icon,
            coords.x1 + icon_x,
            coords.y1 + icon_y,
            lv_color_hex(0xF8FAFF),
            100);
    }

    // 3) 中心圆与当前项粉红色图标也在同一对象内绘制。
    lv_draw_rect_dsc_t center = {};
    lv_draw_rect_dsc_init(&center);
    center.bg_color = lv_color_hex(0x05070B);
    center.bg_opa = 245;
    center.radius = LV_RADIUS_CIRCLE;
    center.border_width = 2;
    center.border_color = lv_color_hex(0x161B27);
    center.border_opa = LV_OPA_COVER;
    const int32_t half = kLauncherCenterDiameter / 2;
    lv_area_t center_area = {cx - half, cy - half, cx + half, cy + half};
    lv_draw_rect(layer, &center, &center_area);

    player_home_launcher_draw_icon(
        layer,
        kLauncherItems[g_launcher_selected_index].icon,
        cx + kLauncherItems[g_launcher_selected_index].optical_x,
        cy + kLauncherItems[g_launcher_selected_index].optical_y,
        lv_color_hex(kLauncherAccentRgb),
        132);
}

static void player_home_launcher_apply_selection()
{
    if (g_launcher_panel != nullptr) {
        lv_obj_invalidate(g_launcher_panel);
    }
}

static int8_t player_home_launcher_hit_test(int32_t screen_x, int32_t screen_y)
{
    if (g_launcher_panel == nullptr || g_launcher_motion != LauncherMotionState::Shown) {
        return -1;
    }

    lv_area_t coords = {};
    lv_obj_get_coords(g_launcher_panel, &coords);
    const int32_t dx = screen_x - (coords.x1 + kLauncherCenterX);
    const int32_t dy = screen_y - (coords.y1 + kLauncherCenterY);
    const int32_t radius_sq = dx * dx + dy * dy;
    const int32_t min_radius_sq = kLauncherTouchInnerRadius * kLauncherTouchInnerRadius;
    const int32_t max_radius_sq = kLauncherTouchOuterRadius * kLauncherTouchOuterRadius;
    if (radius_sq < min_radius_sq || radius_sq > max_radius_sq) {
        return -1;
    }

    // atan2(x, y) 刻意交换参数，使角度系与 lv_draw_arc 完全一致：
    // 0°=下、90°=右、180°=上、270°=左。
    constexpr float kRadToDeg = 57.295779513082320876f;
    int16_t angle = static_cast<int16_t>(lroundf(atan2f(
        static_cast<float>(dx), static_cast<float>(dy)) * kRadToDeg));
    angle = player_home_launcher_normalize_angle(angle);

    uint8_t best_index = 0U;
    int16_t best_distance = 361;
    for (uint8_t i = 0; i < kLauncherItemCount; ++i) {
        const int16_t distance = player_home_launcher_angle_distance(
            angle, kLauncherItems[i].center_angle);
        if (distance < best_distance) {
            best_distance = distance;
            best_index = i;
        }
    }
    return static_cast<int8_t>(best_index);
}

static void player_home_launcher_panel_anim_exec(void *var, int32_t value)
{
    lv_obj_t *panel = static_cast<lv_obj_t *>(var);
    if (panel != nullptr) {
        lv_obj_set_y(panel, value);
    }
}

static void player_home_launcher_enter_done(lv_anim_t *anim)
{
    (void)anim;
    if (g_launcher_motion != LauncherMotionState::Entering) {
        return;
    }
    g_launcher_motion = LauncherMotionState::Shown;
    now_playing_artwork_set_direct_present_allowed(false);
    const uint32_t elapsed = static_cast<uint32_t>(lv_tick_get()) - g_launcher_anim_started_ms;
    ESP_LOGI(TAG, "R.18 Launcher滑入完成：%ums，径向点击已解锁", static_cast<unsigned>(elapsed));
}

static void player_home_launcher_leave_done(lv_anim_t *anim)
{
    (void)anim;
    if (g_launcher_motion != LauncherMotionState::Leaving) {
        return;
    }
    g_launcher_motion = LauncherMotionState::Hidden;
    g_launcher_visible = false;
    if (g_launcher != nullptr) {
        lv_obj_add_flag(g_launcher, LV_OBJ_FLAG_HIDDEN);
    }
    now_playing_artwork_set_direct_present_allowed(true);
    const uint32_t elapsed = static_cast<uint32_t>(lv_tick_get()) - g_launcher_anim_started_ms;
    ESP_LOGI(TAG, "R.18 Launcher滑出完成：%ums", static_cast<unsigned>(elapsed));
}

static void player_home_launcher_start_animation(
    int32_t target_y,
    LauncherMotionState motion,
    lv_anim_completed_cb_t completed_cb)
{
    if (g_launcher_panel == nullptr) {
        return;
    }

    lv_anim_delete(g_launcher_panel, player_home_launcher_panel_anim_exec);
    const int32_t start_y = lv_obj_get_y(g_launcher_panel);
    g_launcher_motion = motion;
    g_launcher_anim_started_ms = static_cast<uint32_t>(lv_tick_get());

    lv_anim_t animation = {};
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, g_launcher_panel);
    lv_anim_set_exec_cb(&animation, player_home_launcher_panel_anim_exec);
    lv_anim_set_values(&animation, start_y, target_y);
    lv_anim_set_duration(&animation, kLauncherAnimDurationMs);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&animation, completed_cb);
    lv_anim_start(&animation);
}

static void player_home_launcher_show()
{
    if (g_launcher == nullptr || g_launcher_panel == nullptr) {
        return;
    }

    player_home_overlay_hide();
    now_playing_artwork_set_direct_present_allowed(false);
    if (!g_launcher_visible) {
        g_launcher_visible = true;
        g_launcher_motion = LauncherMotionState::Entering;
        lv_obj_set_y(g_launcher_panel, kLauncherPanelHiddenY);
        lv_obj_remove_flag(g_launcher, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_launcher);
    }

    // 关闭动画尚未结束时再次上滑，也从当前真实位置平滑反向进入。
    player_home_launcher_start_animation(
        kLauncherPanelShownY,
        LauncherMotionState::Entering,
        player_home_launcher_enter_done);
    player_home_launcher_apply_selection();
}

static void player_home_launcher_hide()
{
    if (g_launcher == nullptr || g_launcher_panel == nullptr || !g_launcher_visible ||
        g_launcher_motion == LauncherMotionState::Hidden ||
        g_launcher_motion == LauncherMotionState::Leaving) {
        return;
    }

    // 离场动画期间同样禁止径向点击；Backdrop 保持固定，只让 340x340 菜单对象下滑。
    player_home_launcher_start_animation(
        kLauncherPanelHiddenY,
        LauncherMotionState::Leaving,
        player_home_launcher_leave_done);
}

static void player_home_launcher_backdrop_tap_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        g_launcher_motion != LauncherMotionState::Shown ||
        player_home_click_suppressed() || !gesture_router_press_was_tap(kOverlayTapMaxMovePx)) {
        return;
    }
    player_home_launcher_hide();
}

static void player_home_launcher_panel_click_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        g_launcher_motion != LauncherMotionState::Shown ||
        player_home_click_suppressed() || !gesture_router_press_was_tap(kOverlayTapMaxMovePx)) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t point = {};
    lv_indev_get_point(indev, &point);
    const int8_t hit = player_home_launcher_hit_test(point.x, point.y);
    if (hit < 0) {
        return;
    }

    const uint8_t index = static_cast<uint8_t>(hit);
    g_launcher_selected_index = index;
    player_home_launcher_apply_selection();
    ESP_LOGI(TAG, "R.18 Launcher径向命中：index=%u name=%s touch=(%ld,%ld)",
        static_cast<unsigned>(index),
        kLauncherItems[index].name,
        static_cast<long>(point.x),
        static_cast<long>(point.y));
}

static void player_home_set_volume_adjust_armed(bool armed)
{
    g_volume_adjust_armed = armed && g_overlay_visible;
    gesture_router_set_vertical_adjust_enabled(g_volume_adjust_armed);
    if (!g_volume_adjust_armed) {
        g_volume_dragging = false;
    }

    // 音量图标同时作为“当前允许纵向调音量”的状态提示。
    if (g_volume_mode_button != nullptr) {
        lv_obj_set_style_bg_opa(
            g_volume_mode_button,
            g_volume_adjust_armed ? 150 : 36,
            0);
    }
}

static void player_home_overlay_arm_timeout()
{
    if (g_overlay_timer == nullptr || !g_overlay_visible) {
        return;
    }
    lv_timer_set_period(g_overlay_timer, kOverlayTimeoutMs);
    lv_timer_reset(g_overlay_timer);
    lv_timer_resume(g_overlay_timer);
}

static void player_home_overlay_apply_dim_path()
{
    if (g_overlay_backdrop == nullptr) {
        return;
    }

    // R.20：CoverSurface 命中时直接切预暗 RGB565，backdrop 保持全透明；
    // 只有压缩 JPEG/PNG 等兼容回退路径不具备 dimmed Surface 时，才启用旧 alpha 黑层。
    const bool fast_dim = now_playing_artwork_set_dimmed(g_overlay_visible);
    if (!g_overlay_dim_path_valid || g_overlay_fast_dim != fast_dim) {
        g_overlay_fast_dim = fast_dim;
        g_overlay_dim_path_valid = true;
        const lv_opa_t backdrop_opa =
            fast_dim ? static_cast<lv_opa_t>(LV_OPA_TRANSP) : kOverlayDimOpacity;
        lv_obj_set_style_bg_opa(
            g_overlay_backdrop,
            backdrop_opa,
            0);
    }
}

static void player_home_overlay_show()
{
    if (g_overlay == nullptr) {
        return;
    }

    player_home_launcher_hide();

    if (!g_overlay_visible) {
        g_overlay_visible = true;
        g_overlay_dim_path_valid = false;
        player_home_set_volume_adjust_armed(false);
        player_home_overlay_apply_dim_path();
        lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    player_home_overlay_arm_timeout();
}

static void player_home_overlay_hide()
{
    if (g_overlay == nullptr || !g_overlay_visible) {
        return;
    }
    player_home_set_volume_adjust_armed(false);
    g_overlay_visible = false;
    if (g_overlay_timer != nullptr) {
        lv_timer_pause(g_overlay_timer);
    }
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    // Overlay 关闭后立即切回 normal RGB565。
    now_playing_artwork_set_dimmed(false);
    g_overlay_fast_dim = false;
    g_overlay_dim_path_valid = false;
}

static void player_home_overlay_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_progress_dragging || g_progress_seek_pending || g_volume_dragging) {
        player_home_overlay_arm_timeout();
        return;
    }
    player_home_overlay_hide();
}

static uint8_t player_home_volume_preview_from_delta(uint8_t start_percent, int16_t delta_y)
{
    const int32_t change =
        (-static_cast<int32_t>(delta_y) * 100) / kOverlayVolumeFullScalePx;
    int32_t value = static_cast<int32_t>(start_percent) + change;
    if (value < 0) {
        value = 0;
    } else if (value > 100) {
        value = 100;
    }
    return static_cast<uint8_t>(value);
}

static void player_home_volume_apply_preview(uint8_t value)
{
    g_volume_preview_percent = value;
    if (g_volume_slider != nullptr) {
        lv_slider_set_value(g_volume_slider, value, LV_ANIM_OFF);
    }
    if (g_volume_label != nullptr) {
        lv_label_set_text_fmt(g_volume_label, "%u%%", static_cast<unsigned>(value));
    }
}

static bool player_home_volume_gesture_update()
{
    if (!g_overlay_visible || !g_volume_adjust_armed) {
        return false;
    }

    UiVerticalAdjustSnapshot drag = {};
    if (!gesture_router_get_vertical_adjust(&drag)) {
        return false;
    }

    if (!g_volume_dragging || g_volume_drag_sequence != drag.sequence) {
        AudioStateSnapshot snapshot = {};
        if (!audio_service_get_snapshot(&snapshot)) {
            if (drag.released) {
                gesture_router_ack_vertical_adjust_release(drag.sequence);
            }
            return true;
        }

        g_volume_dragging = true;
        g_volume_drag_sequence = drag.sequence;
        g_volume_drag_start_percent = snapshot.volume_percent;
        g_volume_preview_percent = snapshot.volume_percent;

        // 如果触摸起点刚好落在进度条上，纵向手势一成立就取消 Seek 交互。
        // RELEASED 随后也会看到 GestureRouter 的 engaged 状态，因此不会提交误 Seek。
        if (g_progress_dragging) {
            player_home_cancel_progress_interaction();
            player_home_progress_sync(snapshot);
        }
    }

    const uint8_t preview = player_home_volume_preview_from_delta(
        g_volume_drag_start_percent, drag.delta_y);
    if (preview != g_volume_preview_percent) {
        player_home_volume_apply_preview(preview);
    }

    if (!drag.released) {
        return true;
    }

    g_volume_dragging = false;
    gesture_router_ack_vertical_adjust_release(drag.sequence);
    ESP_LOGI(TAG,
        "Overlay 纵向音量提交：start=%u%% delta_y=%dpx -> %u%%",
        static_cast<unsigned>(g_volume_drag_start_percent),
        static_cast<int>(drag.delta_y),
        static_cast<unsigned>(g_volume_preview_percent));

    if (g_volume_preview_percent != g_volume_drag_start_percent &&
        !player_control_set_volume(g_volume_preview_percent)) {
        ESP_LOGW(TAG, "Overlay 纵向音量请求未能入队");
        player_home_refresh();
    }

    player_home_overlay_arm_timeout();
    return true;
}

static void player_home_repaint_controls_after_direct_present()
{
    if (!now_playing_artwork_take_direct_present_event()) {
        return;
    }

    // DirectPresent 已经把整张 dimmed/normal Surface 直接写入 GRAM，因此会暂时覆盖
    // Home Overlay 的控件像素。LVGL image source 已在禁止 invalidation 的窗口内同步更新，
    // 这里只把真正有视觉内容的控件小区域重新标脏，避免再次无效化 460x460 封面。
    if (!g_overlay_visible) {
        return;
    }

    lv_obj_t *objects[] = {
        g_title,
        g_artist,
        g_track_info,
        g_prev_button,
        g_play_button,
        g_next_button,
        g_progress,
        g_current_time,
        g_total_time,
        g_loop_button,
        g_volume_slider,
        g_volume_mode_button,
    };
    for (lv_obj_t *obj : objects) {
        if (obj != nullptr) {
            lv_obj_invalidate(obj);
        }
    }
    if (g_volume_label != nullptr) {
        lv_obj_t *volume_status = lv_obj_get_parent(g_volume_label);
        if (volume_status != nullptr) {
            lv_obj_invalidate(volume_status);
        } else {
            lv_obj_invalidate(g_volume_label);
        }
    }
}

static void player_home_artwork_fast_rebind(const char *reason)
{
    if (!player_state_is_ready() || media_library_get_count() == 0U ||
        library_view_is_visible() || lyrics_view_is_visible() || spectrum_view_is_visible()) {
        return;
    }

    const uint32_t track_index = static_cast<uint32_t>(player_state_get_index());

    const int64_t started_us = esp_timer_get_time();
    now_playing_artwork_refresh_context();
    now_playing_artwork_update();
    player_home_repaint_controls_after_direct_present();
    g_last_artwork_bound_track = track_index;

    ESP_LOGI(TAG,
        "R.19 封面立即重绑：reason=%s track=%lu cost=%lldus",
        reason != nullptr ? reason : "track-change",
        static_cast<unsigned long>(track_index),
        static_cast<long long>(esp_timer_get_time() - started_us));
}

static void player_home_artwork_watch_track_change()
{
    if (!player_state_is_ready() || media_library_get_count() == 0U ||
        library_view_is_visible() || lyrics_view_is_visible() || spectrum_view_is_visible()) {
        return;
    }
    const uint32_t track_index = static_cast<uint32_t>(player_state_get_index());
    if (track_index != g_last_artwork_bound_track) {
        player_home_artwork_fast_rebind("20ms-context-watch");
    }
}

static void player_home_update_background_timer_qos()
{
    // 主页被歌词/频谱/曲库完整覆盖时，不让主页自己的 100ms Audio/Artwork timer
    // 继续在 LVGL P3 后台醒来。Gesture timer 保持运行，负责统一页面导航与恢复。
    const bool should_run =
        !library_view_is_visible() &&
        !lyrics_view_is_visible() &&
        !spectrum_view_is_visible();
    if (should_run == g_background_timers_running) {
        return;
    }

    g_background_timers_running = should_run;

    // P1.5R.1.2.2：主页被完整覆盖时必须释放 Artwork UI lease。
    // CoverSurface cache 只有两槽，隐藏主页继续 pin 旧曲会迫使下一曲预热淘汰“当前曲”。
    // 恢复主页时 set_active(true) 会按当前 Player context 重新绑定 cache。
    now_playing_artwork_set_active(should_run);

    lv_timer_t *timers[] = {g_audio_timer, g_artwork_timer};
    for (lv_timer_t *background_timer : timers) {
        if (background_timer == nullptr) {
            continue;
        }
        if (should_run) {
            lv_timer_reset(background_timer);
            lv_timer_resume(background_timer);
        } else {
            lv_timer_pause(background_timer);
        }
    }
    ESP_LOGI(TAG, "P1.5R.1.2 主页后台timer：%s", should_run ? "恢复" : "暂停");
}

static void player_home_gesture_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    player_home_update_background_timer_qos();
    if (library_view_is_visible()) {
        return;
    }

    // P1.4.3：歌词页与首页继续共用唯一 GestureRouter 消费点。
    // 歌词 Overlay 显示时，纵向手势优先调音量；其余页面级手势全部锁定。
    if (lyrics_view_is_visible()) {
        if (lyrics_view_process_overlay_interaction()) {
            return;
        }
        UiGestureAction lyrics_action = UiGestureAction::None;
        if (!gesture_router_take_action(&lyrics_action)) {
            return;
        }
        const PlaybackGestureScope lyrics_scope = lyrics_view_overlay_is_visible()
            ? PlaybackGestureScope::LyricsOverlay
            : PlaybackGestureScope::Lyrics;
        if (!player_home_page_allows_gesture(lyrics_scope, lyrics_action)) {
            // 页面不允许的动作只在业务层丢弃；GestureRouter 仍完整消费本轮触摸。
            return;
        }

        ESP_LOGI(TAG, "P1.5.2R.4.2 歌词页允许手势：%s", gesture_router_action_name(lyrics_action));
        if (lyrics_action == UiGestureAction::SwipeLeft) {
            lyrics_view_close();
            player_home_refresh();
        }
        return;
    }

    // P1.5.2R.4.2：频谱页是纯观赏页。触摸照常采集/识别，但页面级业务只允许右滑返回主页。
    // Tap 暂时保持空操作，后续可直接复用为“切换频谱显示样式”。
    if (spectrum_view_is_visible()) {
        UiGestureAction spectrum_action = UiGestureAction::None;
        if (!gesture_router_take_action(&spectrum_action)) {
            return;
        }
        if (!player_home_page_allows_gesture(PlaybackGestureScope::Spectrum, spectrum_action)) {
            return;
        }

        ESP_LOGI(TAG, "P1.5.2R.4.2 频谱页允许手势：%s", gesture_router_action_name(spectrum_action));
        spectrum_view_close();
        player_home_refresh();
        return;
    }

    // R.19：主页可见时每 20ms 只比较一次轻量 Track index。自然 EOF、随机播放或其他入口
    // 改变 Playlist Context 后，立即复用与“歌词/频谱返回主页”相同的封面绑定快路径。
    player_home_artwork_watch_track_change();

    // Launcher 菜单显示时，页面级导航先全部让位；外部轻点由 backdrop 关闭，
    // 若继续滑动任意方向，也直接先收起 Launcher，不在这一版里叠加更多行为。
    if (g_launcher_visible) {
        UiGestureAction launcher_action = UiGestureAction::None;
        if (gesture_router_take_action(&launcher_action)) {
            player_home_launcher_hide();
        }
        return;
    }

    // Overlay 纵向调音量是最高优先级的播放器手势。拖动期间只更新 UI 预览，
    // RELEASE 后才向 AudioTask 提交一次 volume 命令。
    if (player_home_volume_gesture_update()) {
        return;
    }

    UiGestureAction action = UiGestureAction::None;
    if (!gesture_router_take_action(&action)) {
        return;
    }

    const PlaybackGestureScope home_scope = g_overlay_visible
        ? PlaybackGestureScope::HomeOverlay
        : PlaybackGestureScope::Home;
    if (!player_home_page_allows_gesture(home_scope, action)) {
        // Overlay 是控制层：页面级导航统一锁定；纵向音量已在上方优先消费。
        if (g_overlay_visible) {
            player_home_overlay_arm_timeout();
        }
        return;
    }

    ESP_LOGI(TAG, "P1.5.2R.4.2 主页允许手势：%s", gesture_router_action_name(action));
    switch (action) {
        case UiGestureAction::PullDownFromTop:
            player_home_overlay_hide();
            library_view_open();
            break;
        case UiGestureAction::SwipeLeft:
            player_home_overlay_hide();
            spectrum_view_open();
            break;
        case UiGestureAction::SwipeRight:
            player_home_overlay_hide();
            lyrics_view_open();
            break;
        case UiGestureAction::PullUpFromBottom:
            player_home_overlay_hide();
            player_home_launcher_show();
            break;
        default:
            break;
    }
}

static void player_home_screen_tap_cb(lv_event_t *event)
{
    if (g_launcher_visible) {
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || player_home_click_suppressed() ||
        !gesture_router_press_was_tap(kOverlayTapMaxMovePx)) {
        return;
    }
    if (!gesture_router_press_started_in_rect(
            kOverlayTapSafeLeftPx,
            kOverlayTapSafeTopPx,
            kOverlayTapSafeRightPx,
            kOverlayTapSafeBottomPx)) {
        return;
    }
    player_home_overlay_show();
}

static void player_home_overlay_backdrop_tap_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !g_overlay_visible ||
        player_home_click_suppressed() || !gesture_router_press_was_tap(kOverlayTapMaxMovePx)) {
        return;
    }
    // 暗色背景是独立的最底层命中面。按钮和 Slider 均位于它上方，因此空白处
    // 才会进入这里，控制控件的触摸不会再被整屏 Overlay 抢走。
    player_home_overlay_hide();
}

static uint64_t player_home_snapshot_total_ms(const AudioStateSnapshot &snapshot)
{
    if (snapshot.sample_rate_hz == 0U || snapshot.total_frames == 0U) {
        return 0U;
    }
    return (snapshot.total_frames * 1000ULL) / snapshot.sample_rate_hz;
}

static int32_t player_home_progress_value_from_ms(uint64_t position_ms, uint64_t total_ms)
{
    if (total_ms == 0U) {
        return 0;
    }
    if (position_ms >= total_ms) {
        return kProgressScale;
    }
    return static_cast<int32_t>((position_ms * static_cast<uint64_t>(kProgressScale)) / total_ms);
}

static uint64_t player_home_progress_ms_from_value(int32_t value, uint64_t total_ms)
{
    if (total_ms == 0U || value <= 0) {
        return 0U;
    }
    if (value >= kProgressScale) {
        return total_ms > 0U ? total_ms - 1U : 0U;
    }
    return (total_ms * static_cast<uint64_t>(value)) / static_cast<uint64_t>(kProgressScale);
}

static void player_home_format_time(uint64_t ms, char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U) {
        return;
    }
    const uint64_t total_seconds = ms / 1000ULL;
    const uint64_t hours = total_seconds / 3600ULL;
    const uint64_t minutes = (total_seconds / 60ULL) % 60ULL;
    const uint64_t seconds = total_seconds % 60ULL;
    if (hours > 0U) {
        snprintf(buffer, buffer_size, "%llu:%02llu:%02llu",
            static_cast<unsigned long long>(hours),
            static_cast<unsigned long long>(minutes),
            static_cast<unsigned long long>(seconds));
    } else {
        snprintf(buffer, buffer_size, "%llu:%02llu",
            static_cast<unsigned long long>(total_seconds / 60ULL),
            static_cast<unsigned long long>(seconds));
    }
}

static void player_home_update_time_labels(uint64_t position_ms, uint64_t total_ms)
{
    char current[24] = {};
    char total[24] = {};
    player_home_format_time(position_ms, current, sizeof(current));
    player_home_format_time(total_ms, total, sizeof(total));
    if (g_current_time != nullptr) {
        lv_label_set_text(g_current_time, current);
    }
    if (g_total_time != nullptr) {
        lv_label_set_text(g_total_time, total);
    }
}

static bool player_home_snapshot_can_scrub(const AudioStateSnapshot &snapshot)
{
    if (!snapshot.ready || !snapshot.seek_supported || snapshot.track_index == UINT32_MAX) {
        return false;
    }
    if (snapshot.track_index != player_state_get_index()) {
        return false;
    }
    if (snapshot.sample_rate_hz == 0U || snapshot.total_frames == 0U) {
        return false;
    }

    // P1.3.5.4.2：FLAC 的任意位置 Seek 会完整重建 decoder/prefetch/I2S pipeline。
    // 实机已确认“播放中拖动”会在 UI 刷新与预取竞争下出现卡顿甚至进入 Error。
    // 稳定模式下 FLAC 必须先暂停再拖进度条；MP3/WAV 仍允许播放中 Seek。
    if (snapshot.format == MediaFormat::FLAC) {
        return snapshot.state == AudioPlaybackState::Paused;
    }

    return snapshot.state == AudioPlaybackState::Playing ||
        snapshot.state == AudioPlaybackState::Paused ||
        snapshot.state == AudioPlaybackState::Seeking;
}

static void player_home_progress_set_enabled(bool enabled)
{
    if (g_progress == nullptr) {
        return;
    }
    if (enabled) {
        lv_obj_remove_state(g_progress, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(g_progress, LV_STATE_DISABLED);
    }
}

static void player_home_cancel_progress_interaction()
{
    g_progress_dragging = false;
    g_progress_seek_pending = false;
    g_progress_total_ms = 0U;
    g_progress_preview_ms = 0U;
    g_progress_track_index = UINT32_MAX;
    g_progress_playback_revision = 0U;
    g_progress_seek_base_revision = 0U;
    g_progress_seek_started_tick = 0U;
}

static void player_home_progress_sync(const AudioStateSnapshot &snapshot)
{
    if (g_progress == nullptr) {
        return;
    }

    const uint64_t total_ms = player_home_snapshot_total_ms(snapshot);
    const bool same_track = snapshot.track_index != UINT32_MAX &&
        snapshot.track_index == player_state_get_index();

    if (g_progress_dragging &&
        (!same_track || snapshot.track_index != g_progress_track_index ||
         snapshot.playback_revision != g_progress_playback_revision)) {
        player_home_cancel_progress_interaction();
    }

    if (g_progress_seek_pending) {
        const bool request_context_stale = !same_track ||
            snapshot.track_index != g_progress_track_index ||
            snapshot.playback_revision != g_progress_playback_revision;
        const bool seek_committed = snapshot.seek_revision != g_progress_seek_base_revision;
        const bool seek_failed = snapshot.state == AudioPlaybackState::Error;
        const bool seek_wait_expired = snapshot.state != AudioPlaybackState::Seeking &&
            lv_tick_elaps(g_progress_seek_started_tick) > 10000U;
        if (request_context_stale || seek_committed || seek_failed || seek_wait_expired) {
            g_progress_seek_pending = false;
        }
    }

    player_home_progress_set_enabled(player_home_snapshot_can_scrub(snapshot));

    if (g_progress_dragging || g_progress_seek_pending) {
        player_home_update_time_labels(g_progress_preview_ms, g_progress_total_ms);
        return;
    }

    if (!same_track || total_ms == 0U) {
        lv_slider_set_value(g_progress, 0, LV_ANIM_OFF);
        player_home_update_time_labels(0U, total_ms);
        return;
    }

    lv_slider_set_value(
        g_progress,
        player_home_progress_value_from_ms(snapshot.position_ms, total_ms),
        LV_ANIM_OFF);
    player_home_update_time_labels(snapshot.position_ms, total_ms);
}

static void player_home_progress_cb(lv_event_t *event)
{
    if (event == nullptr || g_progress == nullptr) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        player_home_overlay_show();
        AudioStateSnapshot snapshot = {};
        if (!audio_service_get_snapshot(&snapshot) || !player_home_snapshot_can_scrub(snapshot)) {
            return;
        }
        const uint64_t total_ms = player_home_snapshot_total_ms(snapshot);
        if (total_ms == 0U) {
            return;
        }
        g_progress_dragging = true;
        g_progress_seek_pending = false;
        g_progress_total_ms = total_ms;
        g_progress_track_index = snapshot.track_index;
        g_progress_playback_revision = snapshot.playback_revision;
        g_progress_seek_base_revision = snapshot.seek_revision;
        g_progress_preview_ms = player_home_progress_ms_from_value(
            lv_slider_get_value(g_progress), total_ms);
        player_home_update_time_labels(g_progress_preview_ms, g_progress_total_ms);
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED && g_progress_dragging) {
        g_progress_preview_ms = player_home_progress_ms_from_value(
            lv_slider_get_value(g_progress), g_progress_total_ms);
        player_home_update_time_labels(g_progress_preview_ms, g_progress_total_ms);
        player_home_overlay_arm_timeout();
        return;
    }

    if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) &&
        gesture_router_vertical_adjust_is_engaged()) {
        if (g_progress_dragging) {
            player_home_cancel_progress_interaction();
            player_home_refresh();
        }
        player_home_overlay_arm_timeout();
        return;
    }

    if (code == LV_EVENT_PRESS_LOST && g_progress_dragging) {
        player_home_cancel_progress_interaction();
        player_home_refresh();
        player_home_overlay_arm_timeout();
        return;
    }

    if (code != LV_EVENT_RELEASED || !g_progress_dragging) {
        return;
    }

    AudioStateSnapshot snapshot = {};
    const bool snapshot_ok = audio_service_get_snapshot(&snapshot);
    const bool context_ok = snapshot_ok && player_home_snapshot_can_scrub(snapshot) &&
        snapshot.track_index == g_progress_track_index &&
        snapshot.playback_revision == g_progress_playback_revision;
    if (!context_ok) {
        player_home_cancel_progress_interaction();
        if (snapshot_ok) {
            player_home_refresh();
        }
        player_home_overlay_arm_timeout();
        return;
    }

    g_progress_preview_ms = player_home_progress_ms_from_value(
        lv_slider_get_value(g_progress), g_progress_total_ms);
    g_progress_dragging = false;
    g_progress_seek_base_revision = snapshot.seek_revision;
    g_progress_seek_started_tick = lv_tick_get();

    if (player_control_seek_ms(g_progress_preview_ms)) {
        g_progress_seek_pending = true;
        player_home_update_time_labels(g_progress_preview_ms, g_progress_total_ms);
    } else {
        player_home_cancel_progress_interaction();
        player_home_refresh();
    }
    player_home_overlay_arm_timeout();
}

static void player_home_format_sample_info(
    uint32_t sample_rate_hz,
    uint16_t bits_per_sample,
    char *out,
    size_t out_size)
{
    if (out == nullptr || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    if (sample_rate_hz == 0U) {
        return;
    }

    char rate[24] = {};
    if ((sample_rate_hz % 1000U) == 0U) {
        snprintf(rate, sizeof(rate), "%lukHz",
            static_cast<unsigned long>(sample_rate_hz / 1000U));
    } else {
        const uint32_t tenth_khz = (sample_rate_hz + 50U) / 100U;
        snprintf(rate, sizeof(rate), "%lu.%lukHz",
            static_cast<unsigned long>(tenth_khz / 10U),
            static_cast<unsigned long>(tenth_khz % 10U));
    }

    if (bits_per_sample > 0U) {
        snprintf(out, out_size, "%s / %ubit", rate, static_cast<unsigned>(bits_per_sample));
    } else {
        snprintf(out, out_size, "%s", rate);
    }
}

static void player_home_refresh_track(const AudioStateSnapshot *audio_snapshot)
{
    if (g_title == nullptr || g_artist == nullptr || g_track_info == nullptr) {
        return;
    }

    const size_t library_count = media_library_get_count();
    const size_t list_count = player_state_get_list_count();
    if (library_count == 0U || list_count == 0U) {
        lv_label_set_text(g_title, "暂无歌曲");
        lv_label_set_text(g_artist, "");
        lv_label_set_text(g_track_info, "音乐库为空");
        return;
    }

    const size_t index = player_state_get_index();
    const size_t list_position = player_state_get_list_position();
    MediaTrackViewV2 view = {};
    const bool have_view = media_catalog_v2_get_track_view(index, &view);

    char title_fallback[512] = {};
    const char *title = nullptr;
    if (have_view && view.title != nullptr && view.title[0] != '\0') {
        title = view.title;
    } else if (media_library_copy_display_name(index, title_fallback, sizeof(title_fallback))) {
        title = title_fallback;
    } else {
        snprintf(title_fallback, sizeof(title_fallback), "歌曲 %u", static_cast<unsigned>(index + 1U));
        title = title_fallback;
    }
    lv_label_set_text(g_title, title);

    const char *artist = have_view && view.artist != nullptr && view.artist[0] != '\0'
        ? view.artist
        : "未知歌手";
    lv_label_set_text(g_artist, artist);

    uint32_t sample_rate_hz = 0U;
    uint16_t bits_per_sample = 0U;
    if (audio_snapshot != nullptr && audio_snapshot->track_index == index) {
        sample_rate_hz = audio_snapshot->sample_rate_hz;
        bits_per_sample = audio_snapshot->bits_per_sample;
    }
    if (sample_rate_hz == 0U && have_view && view.row != nullptr) {
        sample_rate_hz = view.row->technical.sample_rate_hz;
        bits_per_sample = view.row->technical.bits_per_sample;
    }

    char sample_info[48] = {};
    player_home_format_sample_info(sample_rate_hz, bits_per_sample, sample_info, sizeof(sample_info));
    if (sample_info[0] != '\0') {
        lv_label_set_text_fmt(
            g_track_info,
            "%u / %u  ·  %s  ·  %s",
            static_cast<unsigned>(list_position + 1U),
            static_cast<unsigned>(list_count),
            media_format_name(player_state_get_format()),
            sample_info);
    } else {
        lv_label_set_text_fmt(
            g_track_info,
            "%u / %u  ·  %s",
            static_cast<unsigned>(list_position + 1U),
            static_cast<unsigned>(list_count),
            media_format_name(player_state_get_format()));
    }
}

static void player_home_refresh_transport_controls(const AudioStateSnapshot *snapshot)
{
    if (g_loop_button != nullptr) {
        // 模式按钮使用自绘图标；切换模式后只需要重绘这个 42x42 小对象。
        lv_obj_invalidate(g_loop_button);
    }

    if (snapshot != nullptr) {
        if (!g_volume_dragging && g_volume_slider != nullptr) {
            lv_slider_set_value(g_volume_slider, snapshot->volume_percent, LV_ANIM_OFF);
        }
        if (!g_volume_dragging && g_volume_label != nullptr) {
            if (snapshot->user_muted) {
                lv_label_set_text(g_volume_label, "静音");
            } else {
                lv_label_set_text_fmt(g_volume_label, "%u%%", static_cast<unsigned>(snapshot->volume_percent));
            }
        }
    }
}

static void player_home_apply_audio_snapshot(const AudioStateSnapshot &snapshot)
{
    const bool pause_icon = snapshot.state == AudioPlaybackState::Playing;
    if (g_play_symbol != nullptr) {
        lv_label_set_text(
            g_play_symbol,
            pause_icon ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if (g_play_icon_pause != pause_icon) {
        g_play_icon_pause = pause_icon;
        if (g_play_button != nullptr) {
            lv_obj_invalidate(g_play_button);
        }
    }

    player_home_progress_sync(snapshot);
    player_home_refresh_track(&snapshot);
    player_home_refresh_transport_controls(&snapshot);
}

static void player_home_artwork_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (ui_touch_input_recent_activity(kInteractionYieldMs)) {
        return;
    }
    now_playing_artwork_update();
    player_home_repaint_controls_after_direct_present();
    // 如果 Overlay 打开期间新 Surface 刚好准备完成，立即切到该曲的 dimmed RGB565。
    if (g_overlay_visible) {
        player_home_overlay_apply_dim_path();
    }
}

static void player_home_audio_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (ui_touch_input_recent_activity(kInteractionYieldMs)) {
        return;
    }
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot)) {
        return;
    }
    if (snapshot.state_revision == g_last_audio_state_revision) {
        return;
    }
    g_last_audio_state_revision = snapshot.state_revision;
    player_home_apply_audio_snapshot(snapshot);
}

static void player_home_prev_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || player_home_click_suppressed()) {
        return;
    }
    player_home_overlay_show();
    ESP_LOGI(TAG, "P1.2 控件命中：上一曲");
    const size_t before_track = player_state_get_index();
    if (player_control_previous()) {
        if (player_state_get_index() != before_track) {
            // R.19：真正跨 Track 后立刻绑定预热 Surface；>3s 的“上一曲=回到曲首”不重复刷封面。
            player_home_artwork_fast_rebind("manual-prev");
        }
        AudioStateSnapshot snapshot = {};
        audio_service_get_snapshot(&snapshot);
        player_home_apply_audio_snapshot(snapshot);
    }
}

static void player_home_next_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || player_home_click_suppressed()) {
        return;
    }
    player_home_overlay_show();
    ESP_LOGI(TAG, "P1.2 控件命中：下一曲");
    const size_t before_track = player_state_get_index();
    if (player_control_next()) {
        if (player_state_get_index() != before_track) {
            player_home_artwork_fast_rebind("manual-next");
        }
        AudioStateSnapshot snapshot = {};
        audio_service_get_snapshot(&snapshot);
        player_home_apply_audio_snapshot(snapshot);
    }
}

static void player_home_play_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || player_home_click_suppressed()) {
        return;
    }
    player_home_overlay_show();
    ESP_LOGI(TAG, "P1.2 控件命中：播放/暂停");
    if (!player_control_toggle_play_pause()) {
        ESP_LOGW(TAG, "播放控制请求未能入队");
    }
}

static void player_home_loop_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || player_home_click_suppressed()) {
        return;
    }
    player_home_overlay_show();
    ESP_LOGI(TAG, "P1.5.3.2R.8 控件命中：播放模式图标");
    player_control_cycle_loop_mode();
    AudioStateSnapshot snapshot = {};
    audio_service_get_snapshot(&snapshot);
    player_home_refresh_transport_controls(&snapshot);
}

static void player_home_volume_mode_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !g_overlay_visible ||
        player_home_click_suppressed()) {
        return;
    }

    const bool armed = !g_volume_adjust_armed;
    player_home_set_volume_adjust_armed(armed);
    player_home_overlay_arm_timeout();
    ESP_LOGI(TAG, "P1.5.3.2R.7 音量手势：%s", armed ? "已进入纵向调节" : "已退出纵向调节");
}

static void player_home_mute_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || player_home_click_suppressed()) {
        return;
    }
    player_home_overlay_show();
    ESP_LOGI(TAG, "P1.2 控件命中：静音");
    if (!player_control_toggle_mute()) {
        ESP_LOGW(TAG, "静音切换请求未能入队");
    }
}

void player_home_refresh()
{
    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot)) {
        return;
    }

    // P1.5R.1.2.1：从歌词/频谱/曲库返回主页时，不能只切换 Artwork context 后
    // 等待下一次 100ms timer。主页此刻已经可见，立即恢复后台 timer，并主动消费一次
    // 当前曲 Surface/Loader 状态，避免切歌期间 timer 被暂停后长期停在“准备封面...”。
    player_home_update_background_timer_qos();
    player_home_apply_audio_snapshot(snapshot);
    now_playing_artwork_refresh_context();
    now_playing_artwork_update();
    player_home_repaint_controls_after_direct_present();
    if (player_state_is_ready() && media_library_get_count() > 0U) {
        g_last_artwork_bound_track = static_cast<uint32_t>(player_state_get_index());
    }
    if (g_overlay_visible) {
        player_home_overlay_apply_dim_path();
    }
    g_last_audio_state_revision = snapshot.state_revision;
}

void player_home_create(lv_obj_t *screen)
{
    if (screen == nullptr) {
        return;
    }

    player_home_cancel_progress_interaction();
    g_volume_dragging = false;
    g_volume_adjust_armed = false;
    g_overlay_visible = false;
    g_overlay_fast_dim = false;
    g_overlay_dim_path_valid = false;
    g_overlay_backdrop = nullptr;
    g_launcher = nullptr;
    g_launcher_backdrop = nullptr;
    g_launcher_panel = nullptr;
    g_launcher_visible = false;
    g_launcher_motion = LauncherMotionState::Hidden;
    g_launcher_selected_index = 0U;
    g_launcher_anim_started_ms = 0U;
    g_prev_button = nullptr;
    g_play_button = nullptr;
    g_next_button = nullptr;
    g_play_symbol = nullptr;
    g_play_icon_pause = false;
    g_volume_mode_button = nullptr;
    g_audio_timer = nullptr;
    g_artwork_timer = nullptr;
    g_background_timers_running = true;
    g_last_artwork_bound_track = UINT32_MAX;
    gesture_router_reset();
    now_playing_artwork_set_direct_present_allowed(true);

    ui_common_lock_object(screen);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(screen, player_home_screen_tap_cb, LV_EVENT_CLICKED, nullptr);

    // 全屏封面是播放器唯一背景；正常路径由 CoverSurfaceTask 提供最终 460x460 RGB565。
    lv_obj_t *cover = nullptr;
    const int32_t cover_size =
        FAKEPOD_LCD_WIDTH > FAKEPOD_LCD_HEIGHT ? FAKEPOD_LCD_WIDTH : FAKEPOD_LCD_HEIGHT;
    const esp_err_t artwork_ret = now_playing_artwork_create(screen, cover_size, &cover);
    if (artwork_ret == ESP_OK && cover != nullptr) {
        lv_obj_center(cover);
        lv_obj_move_background(cover);
    } else {
        ESP_LOGW(TAG, "创建全屏封面失败，继续黑底运行：%s", esp_err_to_name(artwork_ret));
    }

    if (player_state_is_ready() && media_library_get_count() > 0U) {
        g_last_artwork_bound_track = static_cast<uint32_t>(player_state_get_index());
    }

    // P1.2：Overlay 本体只负责承载控件，本身透明且不可点击。backdrop 仍作为最底层
    // 点击命中面，但正常情况下背景透明；只有最终 surface 预处理失败时才启用 alpha 黑层。
    g_overlay = lv_obj_create(screen);
    ui_common_lock_object(g_overlay);
    lv_obj_set_pos(g_overlay, 0, 0);
    lv_obj_set_size(g_overlay, FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT);
    lv_obj_set_style_radius(g_overlay, 0, 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_overlay, 0, 0);
    lv_obj_set_style_shadow_width(g_overlay, 0, 0);
    lv_obj_set_style_pad_all(g_overlay, 0, 0);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_CLICKABLE);

    g_overlay_backdrop = lv_obj_create(g_overlay);
    ui_common_lock_object(g_overlay_backdrop);
    lv_obj_set_pos(g_overlay_backdrop, 0, 0);
    lv_obj_set_size(g_overlay_backdrop, FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT);
    lv_obj_set_style_radius(g_overlay_backdrop, 0, 0);
    lv_obj_set_style_bg_color(g_overlay_backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_overlay_backdrop, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_overlay_backdrop, 0, 0);
    lv_obj_set_style_shadow_width(g_overlay_backdrop, 0, 0);
    lv_obj_set_style_pad_all(g_overlay_backdrop, 0, 0);
    lv_obj_add_flag(g_overlay_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        g_overlay_backdrop, player_home_overlay_backdrop_tap_cb, LV_EVENT_CLICKED, nullptr);

    // P1.5.3.2R.18：Launcher 根层固定不动，Backdrop 只负责一次性暗化；
    // 真正参与滑入/滑出的只有中央 340x340 单对象圆环菜单。
    g_launcher = lv_obj_create(screen);
    ui_common_lock_object(g_launcher);
    lv_obj_set_pos(g_launcher, 0, 0);
    lv_obj_set_size(g_launcher, FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT);
    lv_obj_set_style_radius(g_launcher, 0, 0);
    lv_obj_set_style_bg_opa(g_launcher, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_launcher, 0, 0);
    lv_obj_set_style_shadow_width(g_launcher, 0, 0);
    lv_obj_set_style_pad_all(g_launcher, 0, 0);
    lv_obj_remove_flag(g_launcher, LV_OBJ_FLAG_CLICKABLE);

    g_launcher_backdrop = lv_obj_create(g_launcher);
    ui_common_lock_object(g_launcher_backdrop);
    lv_obj_set_pos(g_launcher_backdrop, 0, 0);
    lv_obj_set_size(g_launcher_backdrop, FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT);
    lv_obj_set_style_radius(g_launcher_backdrop, 0, 0);
    lv_obj_set_style_bg_color(g_launcher_backdrop, lv_color_hex(kLauncherBackdropRgb), 0);
    lv_obj_set_style_bg_opa(g_launcher_backdrop, kLauncherBackdropOpa, 0);
    lv_obj_set_style_border_width(g_launcher_backdrop, 0, 0);
    lv_obj_set_style_shadow_width(g_launcher_backdrop, 0, 0);
    lv_obj_set_style_pad_all(g_launcher_backdrop, 0, 0);
    lv_obj_add_flag(g_launcher_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        g_launcher_backdrop,
        player_home_launcher_backdrop_tap_cb,
        LV_EVENT_CLICKED,
        nullptr);

    g_launcher_panel = lv_obj_create(g_launcher);
    ui_common_lock_object(g_launcher_panel);
    lv_obj_set_pos(g_launcher_panel, kLauncherPanelX, kLauncherPanelHiddenY);
    lv_obj_set_size(g_launcher_panel, kLauncherPanelSize, kLauncherPanelSize);
    lv_obj_set_style_radius(g_launcher_panel, 0, 0);
    lv_obj_set_style_bg_opa(g_launcher_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_launcher_panel, 0, 0);
    lv_obj_set_style_shadow_width(g_launcher_panel, 0, 0);
    lv_obj_set_style_pad_all(g_launcher_panel, 0, 0);
    lv_obj_add_flag(g_launcher_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_launcher_panel, player_home_control_capture_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(
        g_launcher_panel,
        player_home_launcher_panel_draw_cb,
        LV_EVENT_DRAW_MAIN,
        nullptr);
    lv_obj_add_event_cb(
        g_launcher_panel,
        player_home_launcher_panel_click_cb,
        LV_EVENT_CLICKED,
        nullptr);

    player_home_launcher_apply_selection();
    lv_obj_add_flag(g_launcher, LV_OBJ_FLAG_HIDDEN);

    // P1.5.3.2R.9：重建 Overlay 为清晰的纵向信息层级：
    // 歌名 -> 歌手 -> 列表位置/格式/采样 -> 大号播放控制 -> 进度 -> 模式/音量。
    g_title = player_home_create_label(
        g_overlay, "", lv_color_hex(0xFFFFFF), font_manager_get_ui_font());
    lv_label_set_long_mode(g_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_title, 388, 34);
    lv_obj_set_style_text_align(g_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 26);

    g_artist = player_home_create_label(
        g_overlay, "", lv_color_hex(0xD6DAE0), font_manager_get_ui_font());
    lv_label_set_long_mode(g_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_artist, 380, 30);
    lv_obj_set_style_text_align(g_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_artist, LV_ALIGN_TOP_MID, 0, 61);

    g_track_info = player_home_create_label(
        g_overlay, "", lv_color_hex(0x9CA6B4), font_manager_get_ui_font());
    lv_label_set_long_mode(g_track_info, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_track_info, 410, 28);
    lv_obj_set_style_text_align(g_track_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_track_info, LV_ALIGN_TOP_MID, 0, 94);

    // P1.5.3.2R.10：播放控制整体再下移约12px，让顶部信息、播放区、进度区之间留白更均匀。
    // R.9 的 74/94px 真实按钮和触摸面积保持不变，只把内部 Transport 图标明显放大。
    g_prev_button = player_home_create_round_button(g_overlay, 74, LV_SYMBOL_PREV);
    lv_obj_align(g_prev_button, LV_ALIGN_TOP_MID, -98, 157);
    lv_obj_t *prev_symbol = lv_obj_get_child(g_prev_button, 0);
    if (prev_symbol != nullptr) {
        lv_obj_add_flag(prev_symbol, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_event_cb(g_prev_button, player_home_transport_icon_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(g_prev_button, player_home_prev_cb, LV_EVENT_CLICKED, nullptr);

    g_play_button = player_home_create_round_button(g_overlay, 94, LV_SYMBOL_PLAY);
    lv_obj_align(g_play_button, LV_ALIGN_TOP_MID, 0, 147);
    lv_obj_set_style_bg_opa(g_play_button, 225, 0);
    g_play_symbol = lv_obj_get_child(g_play_button, 0);
    if (g_play_symbol != nullptr) {
        // label 仅保存 Play/Pause 状态；实际图标由按钮 DRAW_MAIN 自绘。
        lv_obj_add_flag(g_play_symbol, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_event_cb(g_play_button, player_home_transport_icon_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(g_play_button, player_home_play_cb, LV_EVENT_CLICKED, nullptr);

    g_next_button = player_home_create_round_button(g_overlay, 74, LV_SYMBOL_NEXT);
    lv_obj_align(g_next_button, LV_ALIGN_TOP_MID, 98, 157);
    lv_obj_t *next_symbol = lv_obj_get_child(g_next_button, 0);
    if (next_symbol != nullptr) {
        lv_obj_add_flag(next_symbol, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_event_cb(g_next_button, player_home_transport_icon_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(g_next_button, player_home_next_cb, LV_EVENT_CLICKED, nullptr);

    // 进度区跟随播放控制下移约11px；Stage 11.2 Seek 行为保持不变。
    g_progress = lv_slider_create(g_overlay);
    ui_common_lock_object(g_progress);
    lv_obj_add_flag(g_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g_progress, 340, 10);
    lv_obj_align(g_progress, LV_ALIGN_TOP_MID, 0, 264);
    lv_slider_set_range(g_progress, 0, kProgressScale);
    lv_slider_set_value(g_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(g_progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_progress, 55, LV_PART_MAIN);
    lv_obj_set_style_radius(g_progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_width(g_progress, 18, LV_PART_KNOB);
    lv_obj_set_style_height(g_progress, 18, LV_PART_KNOB);
    lv_obj_set_style_radius(g_progress, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(g_progress, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_COVER, LV_PART_KNOB);
    const lv_style_selector_t disabled_indicator =
        static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
    const lv_style_selector_t disabled_knob =
        static_cast<lv_style_selector_t>(LV_PART_KNOB) | static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_40, disabled_indicator);
    lv_obj_set_style_bg_opa(g_progress, LV_OPA_40, disabled_knob);
    lv_obj_add_event_cb(g_progress, player_home_control_capture_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(g_progress, player_home_progress_cb, LV_EVENT_ALL, nullptr);
    player_home_progress_set_enabled(false);

    g_current_time = player_home_create_label(
        g_overlay, "0:00", lv_color_hex(0xE7E9ED), font_manager_get_ui_font());
    lv_obj_set_size(g_current_time, 110, 28);
    lv_obj_set_style_text_align(g_current_time, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(g_current_time, LV_ALIGN_TOP_MID, -115, 279);

    g_total_time = player_home_create_label(
        g_overlay, "0:00", lv_color_hex(0xE7E9ED), font_manager_get_ui_font());
    lv_obj_set_size(g_total_time, 110, 28);
    lv_obj_set_style_text_align(g_total_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_total_time, LV_ALIGN_TOP_MID, 115, 279);

    // 底部固定为：模式图标 / 音量条 / 音量图标。
    // 模式和音量入口都扩大到 60x60，直接扩大触摸面积；音量图标仍是纵向调节的显式开关。
    g_loop_button = player_home_create_mode_button(g_overlay);
    lv_obj_set_pos(g_loop_button, 48, 346);
    lv_obj_add_event_cb(g_loop_button, player_home_loop_cb, LV_EVENT_CLICKED, nullptr);

    g_volume_slider = lv_slider_create(g_overlay);
    ui_common_lock_object(g_volume_slider);
    lv_obj_remove_flag(g_volume_slider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g_volume_slider, 214, 9);
    lv_obj_set_pos(g_volume_slider, 123, 374);
    lv_slider_set_range(g_volume_slider, 0, 100);
    lv_slider_set_value(g_volume_slider, 80, LV_ANIM_OFF);
    lv_obj_set_style_radius(g_volume_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_volume_slider, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_volume_slider, 55, LV_PART_MAIN);
    lv_obj_set_style_radius(g_volume_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_volume_slider, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_volume_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_width(g_volume_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_height(g_volume_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_radius(g_volume_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(g_volume_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_volume_slider, LV_OPA_COVER, LV_PART_KNOB);

    // 百分比仍保留原静音入口，但收在音量条上方，不打断底部“模式-音量条-音量图标”的主结构。
    lv_obj_t *volume_status = player_home_create_pill_button(g_overlay, 68, 28, "80%", &g_volume_label);
    lv_obj_align(volume_status, LV_ALIGN_TOP_MID, 0, 319);
    lv_obj_add_event_cb(volume_status, player_home_mute_cb, LV_EVENT_CLICKED, nullptr);

    g_volume_mode_button = player_home_create_volume_mode_button(g_overlay);
    lv_obj_set_pos(g_volume_mode_button, 352, 346);
    lv_obj_add_event_cb(g_volume_mode_button, player_home_volume_mode_cb, LV_EVENT_CLICKED, nullptr);

    AudioStateSnapshot snapshot = {};
    audio_service_get_snapshot(&snapshot);
    player_home_apply_audio_snapshot(snapshot);
    g_last_audio_state_revision = snapshot.state_revision;

    g_audio_timer = lv_timer_create(player_home_audio_timer_cb, 100, nullptr);
    g_artwork_timer = lv_timer_create(player_home_artwork_timer_cb, 100, nullptr);
    lv_timer_create(player_home_gesture_timer_cb, 20, nullptr);
    g_overlay_timer = lv_timer_create(player_home_overlay_timeout_cb, kOverlayTimeoutMs, nullptr);
    if (g_overlay_timer != nullptr) {
        lv_timer_pause(g_overlay_timer);
    }

    // 开机/进入播放器先保持纯封面，不把控制层闪现出来。
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);

    char list_label[96] = {};
    if (!player_state_copy_list_label(list_label, sizeof(list_label))) {
        snprintf(list_label, sizeof(list_label), "未知列表");
    }
    ESP_LOGI(TAG,
        "P1.5.3.2R.20：封面normal+dimmed双RGB565 Surface，Overlay命中时取消整屏alpha合成；R.19立即重绑保持；列表=%s 位置=%u/%u track=%u loop=%s volume=%u%% mute=%u",
        list_label,
        static_cast<unsigned>(player_state_get_list_count() > 0 ? player_state_get_list_position() + 1 : 0),
        static_cast<unsigned>(player_state_get_list_count()),
        static_cast<unsigned>(media_library_get_count() > 0 ? player_state_get_index() : 0),
        player_transport_loop_mode_name(player_control_get_loop_mode()),
        static_cast<unsigned>(snapshot.volume_percent),
        static_cast<unsigned>(snapshot.user_muted));
    ESP_LOGI(TAG,
        "P1.5.3.2R.7 Overlay：默认仅点击；点击音量图标后才启用纵向滑动调音量，再次点击或隐藏Overlay即退出；其余行为保持");
    ESP_LOGI(TAG,
        "P1.5.3.2R.8 收口：播放模式图标+随机模式+分段听感音量曲线保持");
    ESP_LOGI(TAG,
        "P1.5.3.2R.10 Overlay微调：上一曲/播放/下一曲整体下移12px，进度与时间下移11px，纵向层级间距更均匀；74/94px触摸按钮保持，Transport图标改为更大的自绘线框");
    ESP_LOGI(TAG,
        "P1.5.3.2R.12 收口：R.11首页纯Tap规则保持；歌词Overlay底部增加模式图标并放大音量入口");
    ESP_LOGI(TAG,
        "P1.5.3.2R.13 音量曲线重分配：30%=-26dB，50%=-18dB，70%=-10dB，80%=-7dB，90%=-4dB，100%=0dB；默认50%保持接近旧版启动响度");
    ESP_LOGI(TAG,
        "P1.3 GestureRouter 已启用：横滑>=72px，顶部/底部边缘=42px，控件优先，滑动后抑制CLICK；顶部下拉=曲库");
}
