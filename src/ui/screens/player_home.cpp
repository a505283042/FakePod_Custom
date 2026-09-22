#include "player_home.h"

#include <stdio.h>
#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "audio_service.h"
#include "app_manager.h"
#include "app_diag_config.h"
#include "assets/launcher_animation_frames.h"
#include "assets/fallback_cover_images.h"
#include "artwork/now_playing_artwork.h"
#include "artwork/cover_surface_cache.h"
#include "cassette_view.h"
#include "board_pins.h"
#include "display_bounded_spi.h"
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
#include "system/battery_service.h"
#include "system/screen_lock_simple.h"
#include "ui_common.h"

static const char *TAG = "首页";

static void player_home_refresh();

#if APP_DIAG_BOOT_VERBOSE
#define HOME_BOOT_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define HOME_BOOT_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif

#if APP_DIAG_UI_INTERACTION
#define HOME_INTERACTION_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define HOME_INTERACTION_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif

#if APP_DIAG_LAUNCHER_PERFORMANCE
#define HOME_LAUNCHER_PERF_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define HOME_LAUNCHER_PERF_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif

#if APP_DIAG_DISPLAY_TRANSPORT
#define HOME_DISPLAY_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define HOME_DISPLAY_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif

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

// P1.5.3.2R.32：圆环仍只占屏幕中央 340x340，但对象从创建起就固定在最终坐标。
// 动画不再移动整个 LVGL 对象，而只改变 DRAW_MAIN 使用的径向展开 progress。
// 这样每帧只失效固定 viewport，不再同时重绘旧位置 + 新位置 + 被暴露的主页区域。
static constexpr int16_t kLauncherPanelSize = 340;
static constexpr int16_t kLauncherPanelX = (460 - kLauncherPanelSize) / 2;
static constexpr int16_t kLauncherPanelShownY = (460 - kLauncherPanelSize) / 2;
static constexpr int32_t kLauncherAnimProgressMax = 1000;
static constexpr int16_t kLauncherCollapsedOuterRadius = 82;
static constexpr int16_t kLauncherCollapsedRingWidth = 20;
static constexpr int16_t kLauncherCollapsedCenterDiameter = 52;
static constexpr int16_t kLauncherCollapsedIconRadius = 24;
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
// P1.5.3.2R.36.6.2：保持 0B PanelWork + PackBits run fast path；producer 直接生成 SPI wire-order，融合背景 copy+swap。
// BoundedSPI 每次请求下一块 DMA staging 时，Strip Compositor 直接从当前
// dimmed 背景（真实 CoverSurface 或无封面 TF 替补图）做 fused copy+native→wire、
// 顺序展开 I4 RLE、叠加中心圆/图标，
// DMA staging 产出即为 SPI wire-order，显示层不再第二次整块 byte-swap。这样继续复用 R.36.4 的有界 transaction 生命周期，同时
// 再释放 231,200B Launcher 专用 PSRAM。
static constexpr uint32_t kLauncherSurfaceStride = FAKEPOD_LCD_WIDTH * sizeof(uint16_t);
static constexpr uint32_t kLauncherPanelWorkBytesSaved =
    static_cast<uint32_t>(kLauncherPanelSize) * kLauncherPanelSize * sizeof(uint16_t);
static constexpr uint32_t kLauncherFullBaseBytesSaved =
    static_cast<uint32_t>(FAKEPOD_LCD_WIDTH) * FAKEPOD_LCD_HEIGHT * sizeof(uint16_t);
static constexpr uint8_t kLauncherFrameInvalid = 0xFFU;
static constexpr lv_opa_t kLauncherSectorAaOpa = 112U;
static_assert(kLauncherAnimationAssetWidth == kLauncherPanelSize);
static_assert(kLauncherAnimationAssetHeight == kLauncherPanelSize);
static_assert(kLauncherAnimationAssetPixelBytes * 2U ==
    static_cast<uint32_t>(kLauncherPanelSize) * static_cast<uint32_t>(kLauncherPanelSize));
static_assert(kLauncherPanelWorkBytesSaved == 231200U);
static_assert(kLauncherFullBaseBytesSaved == 423200U);

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
    AppId app_id;
    const char *name;
    int16_t center_angle;
    uint32_t idle_rgb;
    int8_t optical_x;
    int8_t optical_y;
    // R.32：按 center_angle 预计算 sin/cos ×10000，动画每帧不再做 14 次 sinf/cosf。
    int16_t unit_x_10000;
    int16_t unit_y_10000;
};

// 7 个扇区按约 51.4° 等距分布；扇区宽 46°，相邻之间保留暗缝。
// optical_x/y 只修正不同图形的“视觉重心”，不会改变扇区几何或触摸判定。
static_assert(kLauncherItemCount == static_cast<uint8_t>(AppId::Count));
static constexpr LauncherMenuItemDef kLauncherItems[kLauncherItemCount] = {
    {LauncherIconKind::Music,       AppId::Music,       "音乐",     180, 0x3C527F, -5,  0,     0, -10000},
    {LauncherIconKind::Nsf,         AppId::Nsf,         "电子音流",  129, 0x334A78,  0,  0,  7771,  -6293},
    {LauncherIconKind::MicSpectrum, AppId::MicSpectrum, "拾音频谱",  77, 0x2B426F, -2,  0,  9744,   2250},
    {LauncherIconKind::Mjpg,        AppId::Video,       "视频",      26, 0x263D69,  0,  0,  4384,   8988},
    {LauncherIconKind::Picture,     AppId::Picture,     "图片播放", 334, 0x233861,  0,  0, -4384,   8988},
    {LauncherIconKind::Ebook,       AppId::Ebook,       "电子书",   283, 0x2A406C,  0,  0, -9744,   2250},
    {LauncherIconKind::Settings,    AppId::Settings,    "设置",     231, 0x354B77,  0,  0, -7771,  -6293},
};

// R.35.1：播放页页面级手势继续统一用白名单过滤。
// 封面/歌词/频谱都允许中央纵向 Flick 切歌；各页原有横向导航保持不变。
// Overlay/Launcher 仍锁住页面切歌，顶部/底部边缘继续保留给曲库与 Launcher。
enum class MusicVisualMode : uint8_t
{
    Artwork = 0,
    Cassette,
};

static MusicVisualMode g_music_visual_mode = MusicVisualMode::Artwork;
// 封面→磁带切换时，Artwork 继续保持显示，直到 Cassette 的封面+壳体整套视觉 ready。
static bool g_cassette_visual_switch_pending = false;

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
                action == UiGestureAction::SwipeUpTrack ||
                action == UiGestureAction::SwipeDownTrack ||
                action == UiGestureAction::PullDownFromTop ||
                action == UiGestureAction::PullUpFromBottom;
        case PlaybackGestureScope::Lyrics:
            return action == UiGestureAction::SwipeLeft ||
                action == UiGestureAction::SwipeUpTrack ||
                action == UiGestureAction::SwipeDownTrack;
        case PlaybackGestureScope::Spectrum:
            return action == UiGestureAction::SwipeRight ||
                action == UiGestureAction::SwipeUpTrack ||
                action == UiGestureAction::SwipeDownTrack;
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
static lv_obj_t *g_visual_mode_button = nullptr;
static lv_obj_t *g_visual_mode_label = nullptr;

static lv_obj_t *g_launcher = nullptr;
static lv_obj_t *g_launcher_backdrop = nullptr;
static lv_obj_t *g_launcher_panel = nullptr;
static CoverSurfaceLease g_launcher_surface_lease = {};
static FallbackCoverImageLease g_launcher_fallback_lease = {};
// 无封面 Launcher 只在菜单打开期间持有一张暗化副本；收起立即释放，不长期增加 PSRAM 常驻量。
static uint8_t *g_launcher_fallback_dimmed = nullptr;
static const uint8_t *g_launcher_surface_source = nullptr;
static bool g_launcher_surface_is_fallback = false;
static bool g_launcher_frame_cache_ready = false;
static bool g_launcher_frame_cache_active = false;
static bool g_launcher_surface_lease_ready = false;
static uint32_t g_launcher_surface_lease_track = UINT32_MAX;
static uint32_t g_launcher_surface_lease_us = 0U;
// I4 仍负责把 7 个扇区身份压在 Flash 中。纯 RGB565 路径只需要颜色 LUT；
// index=0 直接保留从 dimmed Surface lease 拷入 DMA strip 的背景，8..14 仅在稀疏 AA 边缘 blend。
static uint16_t g_launcher_index_color565[16] = {};
static uint8_t g_launcher_index_alpha[16] = {};
static uint32_t g_launcher_color_pair_lut[256] = {};
static uint32_t g_launcher_color_pair_wire_lut[256] = {};
// R.36.6.1：把每个 packed pair 预分类，热路径不再每 pair 重复查两次 alpha。
// 0=完全透明保留背景，1=双像素全不透明可32bit批量写，2=需要逐像素透明/AA处理。
static uint8_t g_launcher_pair_mode[256] = {};
static uint8_t g_launcher_frame_index = kLauncherFrameInvalid;
static uint32_t g_launcher_frame_decode_count = 0U;
static uint32_t g_launcher_frame_decode_us = 0U;
static uint32_t g_launcher_frame_decode_max_us = 0U;
// R.33.2：Launcher 动画曾复用 esp_lcd Panel IO 做 Continuous GRAM，实机反复开关会被
// Panel IO 内部 portMAX_DELAY 永久卡死。R.36.2.1 因此先回退 LVGL 安全路径。
// R.36.3：重新启用高速路径，但不再调用旧 PanelIO DirectPresent；改用 display 层的
// Launcher BoundedSPI session：同一个 SPI device、独立 transaction identity、所有 queue/get
// 都有 deadline；可安全回收的失败结束 session 并回到 R.36.2.1 LVGL Canvas，无法回收
// descriptor 的异常则受控重启，避免旧式永久卡死。
#ifndef APP_DISPLAY_LAUNCHER_BOUNDED_SPI
#define APP_DISPLAY_LAUNCHER_BOUNDED_SPI 1
#endif
static constexpr bool kLauncherBoundedSpiEnabled = APP_DISPLAY_LAUNCHER_BOUNDED_SPI != 0;
static bool g_launcher_bounded_session_active = false;
static uint32_t g_launcher_bounded_present_count = 0U;
static uint32_t g_launcher_bounded_present_us = 0U;
static uint32_t g_launcher_bounded_present_max_us = 0U;
static uint32_t g_launcher_bounded_stream_us = 0U;
static uint32_t g_launcher_bounded_stream_max_us = 0U;
static uint32_t g_launcher_bounded_failures = 0U;
static bool g_launcher_visible = false;
// C2.4.13：Cassette 进入高速 Launcher 时先由 Launcher acquire 当前 CoverSurface，
// 再隐藏/释放 Cassette root。菜单收起后恢复，避免全屏封面背景上叠着大小轮继续转。
static bool g_launcher_cassette_scene_hidden = false;
static LauncherMotionState g_launcher_motion = LauncherMotionState::Hidden;
static uint8_t g_launcher_selected_index = 0U;
static uint32_t g_launcher_anim_started_ms = 0U;
static int32_t g_launcher_anim_progress = 0;

static lv_timer_t *g_audio_timer = nullptr;
static lv_timer_t *g_artwork_timer = nullptr;
static lv_timer_t *g_gesture_timer = nullptr;
static bool g_background_timers_running = true;
static bool g_app_foreground = true;
static bool g_launcher_open_after_foreground = false;
// BoundedSPI 退出已把当前封面恢复到 GRAM 时，下一次 Artwork resume 只同步 lease/source，
// 不再产生一笔 460x460 LVGL invalidation。
static bool g_artwork_resume_without_invalidation = false;

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
// R.34：播放模式按钮只在模式真正变化时重绘，避免每个 Audio Snapshot 无条件 invalidate。
static PlayerLoopMode g_last_loop_mode = PlayerLoopMode::Sequential;
static bool g_last_loop_mode_valid = false;
static lv_obj_t *g_volume_label = nullptr;
static lv_obj_t *g_volume_slider = nullptr;
static lv_obj_t *g_volume_mode_button = nullptr;
// Battery UI V1：控件页把电量与音量百分比并排显示；图标完全自绘，不依赖额外字体 glyph。
static lv_obj_t *g_battery_status = nullptr;
static lv_obj_t *g_battery_label = nullptr;
static uint32_t g_last_battery_sequence = UINT32_MAX;
static uint8_t g_last_battery_percent = 0U;
static bool g_last_battery_valid = false;

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
static void player_home_apply_audio_snapshot(const AudioStateSnapshot &snapshot);
static void player_home_overlay_hide();
static bool player_home_finish_cassette_switch_if_ready();

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

// P1.5.3.2R.34：所有高频 Audio Snapshot -> LVGL 写入都先比较当前值。
// 目标不是增加缓存，而是阻止“值没变仍 set_*”制造 dirty area。
static void player_home_label_set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label == nullptr) {
        return;
    }
    const char *target = text != nullptr ? text : "";
    const char *current = lv_label_get_text(label);
    if (current != nullptr && strcmp(current, target) == 0) {
        return;
    }
    lv_label_set_text(label, target);
}

static void player_home_slider_set_value_if_changed(lv_obj_t *slider, int32_t value)
{
    if (slider == nullptr || lv_slider_get_value(slider) == value) {
        return;
    }
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
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

    if (obj == g_prev_button || obj == g_next_button) {
        // R.34：上一曲/下一曲改成标准 Track Previous / Track Next：
        // 独立 track bar + 实心三角。仍使用原 74px 实体按钮和命中区域。
        const bool previous = obj == g_prev_button;
        const int32_t dir = previous ? -1 : 1;
        line.width = 5;
        line.round_start = 0U;
        line.round_end = 0U;

        // Track bar。
        draw(cx + dir * 16, cy - 15, cx + dir * 16, cy + 15);

        // 用 2px 水平扫描线填充三角形，避免引入额外图片/字体资源。
        line.width = 2;
        for (int32_t y = -14; y <= 14; y += 2) {
            const int32_t inset = (abs(y) * 20) / 14;
            if (previous) {
                draw(cx - 10 + inset, cy + y, cx + 10, cy + y);
            } else {
                draw(cx - 10, cy + y, cx + 10 - inset, cy + y);
            }
        }
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
    // R.35.3：播放模式图标线宽与音量图标统一，避免顺序/循环/随机显得过细。
    line.width = 3;
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

static void player_home_volume_status_draw_cb(lv_event_t *event)
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
    const int32_t cx = coords.x1 + 21;
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

    // 小扬声器只画一层声波，尺寸与右侧电池图标匹配。
    draw(cx - 8, cy - 4, cx - 4, cy - 4);
    draw(cx - 8, cy + 4, cx - 4, cy + 4);
    draw(cx - 8, cy - 4, cx - 8, cy + 4);
    draw(cx - 4, cy - 4, cx + 2, cy - 9);
    draw(cx - 4, cy + 4, cx + 2, cy + 9);
    draw(cx + 2, cy - 9, cx + 2, cy + 9);
    draw(cx + 7, cy - 5, cx + 10, cy - 2);
    draw(cx + 10, cy - 2, cx + 10, cy + 2);
    draw(cx + 10, cy + 2, cx + 7, cy + 5);
}

static void player_home_battery_status_draw_cb(lv_event_t *event)
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
    const int32_t x = coords.x1 + 10;
    const int32_t y = (coords.y1 + coords.y2) / 2 - 6;
    const lv_opa_t icon_opa = g_last_battery_valid ? LV_OPA_COVER : LV_OPA_50;

    lv_draw_line_dsc_t line = {};
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(0xF5F7FA);
    line.width = 2;
    line.opa = icon_opa;
    line.round_start = 0U;
    line.round_end = 0U;

    auto draw = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        line.p1.x = x0; line.p1.y = y0;
        line.p2.x = x1; line.p2.y = y1;
        lv_draw_line(layer, &line);
    };

    // 18x12 电池轮廓 + 2px 正极帽；与百分比放在同一胶囊内。
    draw(x, y, x + 18, y);
    draw(x, y + 12, x + 18, y + 12);
    draw(x, y, x, y + 12);
    draw(x + 18, y, x + 18, y + 12);
    draw(x + 20, y + 4, x + 20, y + 8);

    if (!g_last_battery_valid || g_last_battery_percent == 0U) {
        return;
    }

    const int32_t inner_width = 14;
    int32_t fill_width = (inner_width * static_cast<int32_t>(g_last_battery_percent) + 99) / 100;
    if (fill_width < 1) fill_width = 1;
    if (fill_width > inner_width) fill_width = inner_width;

    lv_draw_rect_dsc_t fill = {};
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = lv_color_hex(0xF5F7FA);
    fill.bg_opa = 220;
    fill.border_width = 0;
    fill.radius = 1;
    lv_area_t fill_area = {x + 2, y + 2, x + 1 + fill_width, y + 10};
    lv_draw_rect(layer, &fill, &fill_area);
}

static lv_obj_t *player_home_create_battery_status(lv_obj_t *parent)
{
    lv_obj_t *status = lv_obj_create(parent);
    ui_common_lock_object(status);
    lv_obj_set_size(status, 94, 28);
    lv_obj_set_style_radius(status, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(status, 28, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_shadow_width(status, 0, 0);
    lv_obj_set_style_pad_all(status, 0, 0);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(status, player_home_battery_status_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

    g_battery_label = player_home_create_label(
        status, "--%", lv_color_hex(0xF5F7FA), font_manager_get_ui_font());
    lv_obj_set_size(g_battery_label, 50, 28);
    lv_obj_set_style_text_align(g_battery_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_battery_label, LV_ALIGN_RIGHT_MID, -4, 0);
    return status;
}

static void player_home_refresh_battery_status()
{
    if (g_battery_status == nullptr || g_battery_label == nullptr) {
        return;
    }

    BatterySnapshot battery = {};
    const bool valid = battery_service_get_snapshot(&battery);
    if (valid && g_last_battery_valid && battery.sequence == g_last_battery_sequence) {
        return;
    }
    if (!valid && !g_last_battery_valid && g_last_battery_sequence == 0U) {
        return;
    }

    g_last_battery_valid = valid;
    g_last_battery_sequence = valid ? battery.sequence : 0U;
    g_last_battery_percent = valid ? battery.percent : 0U;

    if (valid) {
        char label[16] = {};
        snprintf(label, sizeof(label), "%u%%", static_cast<unsigned>(battery.percent));
        player_home_label_set_text_if_changed(g_battery_label, label);
    } else {
        player_home_label_set_text_if_changed(g_battery_label, "--%");
    }
    lv_obj_invalidate(g_battery_status);
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
    lv_color_t color,
    lv_opa_t opa)
{
    lv_draw_rect_dsc_t dot = {};
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = color;
    dot.bg_opa = opa;
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
    int32_t scale_percent,
    lv_opa_t opa)
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
    line.width = scale_percent >= 120 ? 4 : (scale_percent >= 70 ? 3 : 2);
    line.opa = opa;
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
            layer, cx + s(x), cy + s(y), s(d) < 3 ? 3 : s(d), color, opa);
    };

    switch (icon) {
        case LauncherIconKind::Music:
            // R.35.3.2：与 Flash 生成器/BoundedSPI 中心图标共用同一套 19px 间距双音符几何。
            draw(-4, -16, -4, 7);
            draw(-4, -16, 15, -20);
            draw(15, -20, 15, 2);
            dot(-11, 10, 10);
            dot(8, 4, 10);
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

static uint8_t player_home_launcher_frame_for_progress(int32_t progress)
{
    if (progress <= 0) {
        return 0U;
    }
    if (progress >= kLauncherAnimProgressMax) {
        return static_cast<uint8_t>(kLauncherAnimationAssetFrameCount - 1U);
    }
    const int32_t scaled =
        progress * static_cast<int32_t>(kLauncherAnimationAssetFrameCount - 1U) +
        kLauncherAnimProgressMax / 2;
    return static_cast<uint8_t>(scaled / kLauncherAnimProgressMax);
}

static uint16_t player_home_launcher_blend_rgb565(uint16_t background, uint16_t foreground, uint8_t opacity)
{
    if (opacity == 0U) {
        return background;
    }
    if (opacity == LV_OPA_COVER) {
        return foreground;
    }

    const uint32_t inv = 255U - opacity;
    const uint32_t br = (background >> 11) & 0x1FU;
    const uint32_t bg = (background >> 5) & 0x3FU;
    const uint32_t bb = background & 0x1FU;
    const uint32_t fr = (foreground >> 11) & 0x1FU;
    const uint32_t fg = (foreground >> 5) & 0x3FU;
    const uint32_t fb = foreground & 0x1FU;
    const uint32_t rr = (fr * opacity + br * inv + 127U) / 255U;
    const uint32_t rg = (fg * opacity + bg * inv + 127U) / 255U;
    const uint32_t rb = (fb * opacity + bb * inv + 127U) / 255U;
    return static_cast<uint16_t>((rr << 11) | (rg << 5) | rb);
}

static inline uint16_t player_home_launcher_wire565(uint16_t native)
{
    return static_cast<uint16_t>((native << 8U) | (native >> 8U));
}

static inline uint32_t player_home_launcher_wire565_pair(uint32_t native_pair)
{
    return ((native_pair & 0x00FF00FFU) << 8U) |
        ((native_pair & 0xFF00FF00U) >> 8U);
}

static void player_home_launcher_release_surface_lease()
{
    if (g_launcher_surface_lease.slot_index != 0xFFU) {
        cover_surface_cache_release(&g_launcher_surface_lease);
    }
    if (g_launcher_fallback_lease.slot_index != 0xFFU) {
        fallback_cover_image_release(&g_launcher_fallback_lease);
    }
    if (g_launcher_fallback_dimmed != nullptr) {
        heap_caps_free(g_launcher_fallback_dimmed);
    }
    g_launcher_surface_lease = {};
    g_launcher_fallback_lease = {};
    g_launcher_fallback_dimmed = nullptr;
    g_launcher_surface_source = nullptr;
    g_launcher_surface_is_fallback = false;
    g_launcher_surface_lease_ready = false;
    g_launcher_surface_lease_track = UINT32_MAX;
    g_launcher_surface_lease_us = 0U;
    fallback_cover_image_discard_unpinned();
}

static bool player_home_launcher_acquire_surface(
    uint32_t track_index,
    CoverSurfaceLease *out_lease,
    uint32_t *out_acquire_us)
{
    if (out_lease == nullptr) {
        return false;
    }
    *out_lease = {};
    if (out_acquire_us != nullptr) {
        *out_acquire_us = 0U;
    }

    const int64_t started_us = esp_timer_get_time();
    CoverSurfaceLease lease = {};
    if (!cover_surface_cache_acquire(track_index, &lease)) {
        return false;
    }

    const bool valid = lease.dimmed_rgb565 != nullptr &&
        lease.width == FAKEPOD_LCD_WIDTH && lease.height == FAKEPOD_LCD_HEIGHT &&
        lease.data_size >= static_cast<size_t>(FAKEPOD_LCD_WIDTH) * FAKEPOD_LCD_HEIGHT * 2U;
    if (!valid) {
        cover_surface_cache_release(&lease);
        return false;
    }

    if (out_acquire_us != nullptr) {
        *out_acquire_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
    }
    *out_lease = lease;
    return true;
}

struct LauncherSurfaceCandidate
{
    CoverSurfaceLease cover = {};
    FallbackCoverImageLease fallback = {};
    uint8_t *owned_dimmed = nullptr;
    const uint8_t *source = nullptr;
    uint32_t track = UINT32_MAX;
    uint32_t acquire_us = 0U;
    bool is_fallback = false;
};

static inline uint16_t player_home_launcher_dim_rgb565(uint16_t pixel)
{
    // 与 CoverSurface.dimmed 保持同一亮度比例：13/32。
    static constexpr uint32_t kDimNum = 13U;
    static constexpr uint32_t kDimShift = 5U;
    static constexpr uint32_t kRound = 1U << (kDimShift - 1U);
    const uint32_t r =
        (static_cast<uint32_t>((pixel >> 11U) & 0x1FU) * kDimNum + kRound) >> kDimShift;
    const uint32_t g =
        (static_cast<uint32_t>((pixel >> 5U) & 0x3FU) * kDimNum + kRound) >> kDimShift;
    const uint32_t b =
        (static_cast<uint32_t>(pixel & 0x1FU) * kDimNum + kRound) >> kDimShift;
    return static_cast<uint16_t>((r << 11U) | (g << 5U) | b);
}

static void player_home_launcher_release_candidate(LauncherSurfaceCandidate *candidate)
{
    if (candidate == nullptr) return;
    if (candidate->cover.slot_index != 0xFFU) {
        cover_surface_cache_release(&candidate->cover);
    }
    if (candidate->fallback.slot_index != 0xFFU) {
        fallback_cover_image_release(&candidate->fallback);
    }
    if (candidate->owned_dimmed != nullptr) {
        heap_caps_free(candidate->owned_dimmed);
    }
    *candidate = {};
    fallback_cover_image_discard_unpinned();
}

static bool player_home_launcher_acquire_fallback_surface(
    uint32_t track_index,
    LauncherSurfaceCandidate *out_candidate)
{
    if (out_candidate == nullptr) return false;

    MediaArtworkViewV2 artwork = {};
    if (media_library_get_artwork_view(track_index, &artwork)) {
        // 有真实封面但 Surface 还没 ready 时继续等待，不允许替补图抢占真实封面。
        return false;
    }

    const FallbackCoverImageKind kind =
        g_music_visual_mode == MusicVisualMode::Cassette
            ? FallbackCoverImageKind::Cassette
            : FallbackCoverImageKind::Artwork;

    const int64_t started_us = esp_timer_get_time();
    FallbackCoverImageLease fallback = {};
    if (!fallback_cover_image_acquire(kind, &fallback) || fallback.rgb565 == nullptr ||
        fallback.width != FAKEPOD_LCD_WIDTH || fallback.height != FAKEPOD_LCD_HEIGHT ||
        fallback.data_size < static_cast<size_t>(FAKEPOD_LCD_WIDTH) * FAKEPOD_LCD_HEIGHT * 2U) {
        if (fallback.slot_index != 0xFFU) {
            fallback_cover_image_release(&fallback);
            fallback_cover_image_discard_unpinned();
        }
        return false;
    }

    const size_t bytes =
        static_cast<size_t>(FAKEPOD_LCD_WIDTH) * FAKEPOD_LCD_HEIGHT * sizeof(uint16_t);
    uint8_t *dimmed = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16U, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (dimmed == nullptr) {
        fallback_cover_image_release(&fallback);
        fallback_cover_image_discard_unpinned();
        ESP_LOGW(TAG,
            "Launcher替补背景暗化缓存分配失败：track=%u bytes=%u，回退LVGL背景",
            static_cast<unsigned>(track_index),
            static_cast<unsigned>(bytes));
        return false;
    }

    const uint16_t *src = reinterpret_cast<const uint16_t *>(fallback.rgb565);
    uint16_t *dst = reinterpret_cast<uint16_t *>(dimmed);
    const size_t pixels = static_cast<size_t>(FAKEPOD_LCD_WIDTH) * FAKEPOD_LCD_HEIGHT;
    for (size_t i = 0U; i < pixels; ++i) {
        dst[i] = player_home_launcher_dim_rgb565(src[i]);
    }

    out_candidate->fallback = fallback;
    out_candidate->owned_dimmed = dimmed;
    out_candidate->source = dimmed;
    out_candidate->track = track_index;
    out_candidate->acquire_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
    out_candidate->is_fallback = true;
    return true;
}

static bool player_home_launcher_acquire_candidate(
    uint32_t track_index,
    LauncherSurfaceCandidate *out_candidate)
{
    if (out_candidate == nullptr) return false;
    *out_candidate = {};

    CoverSurfaceLease cover = {};
    uint32_t acquire_us = 0U;
    if (player_home_launcher_acquire_surface(track_index, &cover, &acquire_us)) {
        out_candidate->cover = cover;
        out_candidate->source = cover.dimmed_rgb565;
        out_candidate->track = track_index;
        out_candidate->acquire_us = acquire_us;
        out_candidate->is_fallback = false;
        return true;
    }

    return player_home_launcher_acquire_fallback_surface(track_index, out_candidate);
}

static LauncherSurfaceCandidate player_home_launcher_detach_current_surface()
{
    LauncherSurfaceCandidate current = {};
    current.cover = g_launcher_surface_lease;
    current.fallback = g_launcher_fallback_lease;
    current.owned_dimmed = g_launcher_fallback_dimmed;
    current.source = g_launcher_surface_source;
    current.track = g_launcher_surface_lease_track;
    current.acquire_us = g_launcher_surface_lease_us;
    current.is_fallback = g_launcher_surface_is_fallback;

    g_launcher_surface_lease = {};
    g_launcher_fallback_lease = {};
    g_launcher_fallback_dimmed = nullptr;
    g_launcher_surface_source = nullptr;
    g_launcher_surface_lease_track = UINT32_MAX;
    g_launcher_surface_lease_us = 0U;
    g_launcher_surface_lease_ready = false;
    g_launcher_surface_is_fallback = false;
    return current;
}

static void player_home_launcher_commit_candidate(LauncherSurfaceCandidate *candidate)
{
    if (candidate == nullptr || candidate->source == nullptr || candidate->track == UINT32_MAX) {
        return;
    }

    g_launcher_surface_lease = candidate->cover;
    g_launcher_fallback_lease = candidate->fallback;
    g_launcher_fallback_dimmed = candidate->owned_dimmed;
    g_launcher_surface_source = candidate->source;
    g_launcher_surface_lease_track = candidate->track;
    g_launcher_surface_lease_us = candidate->acquire_us;
    g_launcher_surface_lease_ready = true;
    g_launcher_surface_is_fallback = candidate->is_fallback;

    candidate->cover = {};
    candidate->fallback = {};
    candidate->owned_dimmed = nullptr;
    candidate->source = nullptr;
    candidate->track = UINT32_MAX;
    candidate->acquire_us = 0U;
    candidate->is_fallback = false;
}

static bool player_home_launcher_prepare_surface_lease()
{
    player_home_launcher_release_surface_lease();
    g_launcher_surface_lease_us = 0U;
    if (!player_state_is_ready()) {
        return false;
    }

    const uint32_t track_index = static_cast<uint32_t>(player_state_get_index());
    LauncherSurfaceCandidate candidate = {};
    if (!player_home_launcher_acquire_candidate(track_index, &candidate)) {
        return false;
    }

    // 真实封面继续 pin CoverSurface；无封面时 pin 当前视图对应的 TF 替补 JPG，
    // 并只在 Launcher 生命周期内持有暗化副本。
    player_home_launcher_commit_candidate(&candidate);
    return true;
}

static void player_home_launcher_update_frame_lut()
{
    memset(g_launcher_index_color565, 0, sizeof(g_launcher_index_color565));
    memset(g_launcher_index_alpha, 0, sizeof(g_launcher_index_alpha));

    // 模板索引：0=透明；1..7=扇区实色；8..14=对应扇区AA边缘；15=白色外圈图标。
    for (uint8_t i = 0; i < kLauncherItemCount; ++i) {
        const uint32_t rgb =
            i == g_launcher_selected_index ? kLauncherSelectedSectorRgb : kLauncherItems[i].idle_rgb;
        const uint16_t c565 = lv_color_to_u16(lv_color_hex(rgb));
        g_launcher_index_color565[1U + i] = c565;
        g_launcher_index_alpha[1U + i] = LV_OPA_COVER;
        g_launcher_index_color565[8U + i] = c565;
        g_launcher_index_alpha[8U + i] = kLauncherSectorAaOpa;
    }
    g_launcher_index_color565[15U] = lv_color_to_u16(lv_color_hex(0xF8FAFF));
    g_launcher_index_alpha[15U] = LV_OPA_COVER;

    // 对完全不透明的 pair 继续使用 32bit LUT 快速写入；包含透明/AA 的 pair 在解码时
    // 只处理真正需要覆盖的像素，透明 index=0 保留预合成 Base。
    for (uint32_t packed = 0U; packed < 256U; ++packed) {
        const uint8_t left = static_cast<uint8_t>((packed >> 4) & 0x0FU);
        const uint8_t right = static_cast<uint8_t>(packed & 0x0FU);
        g_launcher_color_pair_lut[packed] =
            static_cast<uint32_t>(g_launcher_index_color565[left]) |
            (static_cast<uint32_t>(g_launcher_index_color565[right]) << 16);
        g_launcher_color_pair_wire_lut[packed] =
            player_home_launcher_wire565_pair(g_launcher_color_pair_lut[packed]);
        const uint8_t left_alpha = g_launcher_index_alpha[left];
        const uint8_t right_alpha = g_launcher_index_alpha[right];
        if (left_alpha == 0U && right_alpha == 0U) {
            g_launcher_pair_mode[packed] = 0U;
        }
        else if (left_alpha == LV_OPA_COVER && right_alpha == LV_OPA_COVER) {
            g_launcher_pair_mode[packed] = 1U;
        }
        else {
            g_launcher_pair_mode[packed] = 2U;
        }
    }
}

struct LauncherStripRasterTarget
{
    uint16_t *pixels = nullptr;
    int32_t y_begin = 0;
    int32_t rows = 0;
};

struct LauncherStripComposeContext
{
    const LauncherAnimationFrameAsset *asset = nullptr;
    const uint8_t *surface = nullptr;
    const uint8_t *src = nullptr;
    const uint8_t *src_end = nullptr;
    uint32_t pair_index = 0U;
    uint32_t run_remaining = 0U;
    uint32_t fast_skip_pairs = 0U;
    uint32_t fast_fill_pairs = 0U;
    uint32_t literal_pairs = 0U;
    uint32_t repeat_blend_pairs = 0U;
    uint8_t repeat_value = 0U;
    bool repeat_run = false;
};

static void player_home_launcher_raster_pixel(
    const LauncherStripRasterTarget &target,
    int32_t x,
    int32_t y,
    uint16_t color,
    uint8_t opacity)
{
    if (target.pixels == nullptr || x < 0 || x >= kLauncherPanelSize ||
        y < target.y_begin || y >= target.y_begin + target.rows) {
        return;
    }
    uint16_t &dst = target.pixels[
        static_cast<size_t>(y - target.y_begin) * kLauncherPanelSize + x];
    if (opacity == LV_OPA_COVER) {
        dst = player_home_launcher_wire565(color);
        return;
    }
    const uint16_t native_background = player_home_launcher_wire565(dst);
    const uint16_t native_result = player_home_launcher_blend_rgb565(
        native_background, color, opacity);
    dst = player_home_launcher_wire565(native_result);
}

static void player_home_launcher_raster_disk(
    const LauncherStripRasterTarget &target,
    int32_t cx,
    int32_t cy,
    int32_t diameter,
    uint16_t color,
    uint8_t opacity)
{
    if (diameter <= 0 || target.rows <= 0) {
        return;
    }
    const int32_t radius = diameter / 2;
    const int32_t radius_sq = radius * radius;
    const int32_t y_first = std::max(cy - radius, target.y_begin);
    const int32_t y_last = std::min(cy + radius, target.y_begin + target.rows - 1);
    for (int32_t y = y_first; y <= y_last; ++y) {
        const int32_t dy = y - cy;
        for (int32_t x = cx - radius; x <= cx + radius; ++x) {
            const int32_t dx = x - cx;
            if (dx * dx + dy * dy <= radius_sq) {
                player_home_launcher_raster_pixel(target, x, y, color, opacity);
            }
        }
    }
}

static void player_home_launcher_raster_line(
    const LauncherStripRasterTarget &target,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    int32_t width,
    uint16_t color,
    uint8_t opacity)
{
    int32_t dx = abs(x1 - x0);
    const int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy = -abs(y1 - y0);
    const int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    const int32_t diameter = width < 2 ? 2 : width;
    while (true) {
        if (y0 + diameter / 2 >= target.y_begin &&
            y0 - diameter / 2 < target.y_begin + target.rows) {
            player_home_launcher_raster_disk(target, x0, y0, diameter, color, opacity);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void player_home_launcher_raster_icon(
    const LauncherStripRasterTarget &target,
    LauncherIconKind icon,
    int32_t cx,
    int32_t cy,
    int32_t scale_percent,
    uint16_t color)
{
    auto scale = [scale_percent](int32_t value) -> int32_t {
        const int32_t scaled = (value * scale_percent + (value >= 0 ? 50 : -50)) / 100;
        if (value != 0 && scaled == 0) {
            return value > 0 ? 1 : -1;
        }
        return scaled;
    };
    const int32_t line_width = scale_percent >= 120 ? 4 : (scale_percent >= 70 ? 3 : 2);
    auto line = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        player_home_launcher_raster_line(
            target,
            cx + scale(x0), cy + scale(y0),
            cx + scale(x1), cy + scale(y1),
            line_width, color, LV_OPA_COVER);
    };
    auto dot = [&](int32_t x, int32_t y, int32_t diameter) {
        const int32_t d = scale(diameter) < 3 ? 3 : scale(diameter);
        player_home_launcher_raster_disk(
            target, cx + scale(x), cy + scale(y), d, color, LV_OPA_COVER);
    };

    switch (icon) {
        case LauncherIconKind::Music:
            line(-4, -16, -4, 7); line(-4, -16, 15, -20); line(15, -20, 15, 2);
            dot(-11, 10, 10); dot(8, 4, 10);
            break;
        case LauncherIconKind::Nsf:
            line(-13,-13,13,-13); line(13,-13,13,13); line(13,13,-13,13); line(-13,13,-13,-13);
            line(-8,-18,-8,-13); line(0,-18,0,-13); line(8,-18,8,-13);
            line(-8,13,-8,18); line(0,13,0,18); line(8,13,8,18);
            line(-18,-8,-13,-8); line(-18,0,-13,0); line(-18,8,-13,8);
            line(13,-8,18,-8); line(13,0,18,0); line(13,8,18,8);
            line(-7,4,-2,-4); line(-2,-4,3,4); line(3,4,8,-4);
            break;
        case LauncherIconKind::MicSpectrum:
            line(-11,-14,-11,6); line(-11,-14,-4,-18); line(-4,-18,3,-14); line(3,-14,3,6);
            line(3,6,-4,10); line(-4,10,-11,6); line(-15,4,-15,7); line(-15,7,-9,13);
            line(-9,13,-4,14); line(-4,14,2,12); line(-4,14,-4,19); line(-10,19,2,19);
            line(8,10,8,17); line(13,4,13,17); line(18,-3,18,17);
            break;
        case LauncherIconKind::Mjpg:
            line(-18,-13,11,-13); line(11,-13,11,13); line(11,13,-18,13); line(-18,13,-18,-13);
            line(11,-7,18,-12); line(18,-12,18,12); line(18,12,11,7);
            line(-6,-7,-6,7); line(-6,-7,5,0); line(5,0,-6,7);
            break;
        case LauncherIconKind::Picture:
            line(-18,-15,18,-15); line(18,-15,18,15); line(18,15,-18,15); line(-18,15,-18,-15);
            line(-14,10,-5,0); line(-5,0,1,6); line(1,6,8,-4); line(8,-4,15,10); dot(9,-9,6);
            break;
        case LauncherIconKind::Ebook:
            line(0,-14,0,15); line(-1,-12,-7,-15); line(-7,-15,-18,-12); line(-18,-12,-18,12);
            line(-18,12,-7,10); line(-7,10,-1,13); line(1,-12,7,-15); line(7,-15,18,-12);
            line(18,-12,18,12); line(18,12,7,10); line(7,10,1,13);
            break;
        case LauncherIconKind::Settings:
            dot(0,0,10); line(0,-18,0,-11); line(0,11,0,18); line(-18,0,-11,0); line(11,0,18,0);
            line(-13,-13,-8,-8); line(8,8,13,13); line(13,-13,8,-8); line(-8,8,-13,13);
            break;
    }
}

static void player_home_launcher_raster_center(
    const LauncherStripRasterTarget &target,
    int32_t progress)
{
    if (target.pixels == nullptr || progress <= 0) {
        return;
    }
    if (progress > kLauncherAnimProgressMax) {
        progress = kLauncherAnimProgressMax;
    }
    auto lerp = [progress](int32_t from, int32_t to) -> int32_t {
        return from + ((to - from) * progress + kLauncherAnimProgressMax / 2) /
            kLauncherAnimProgressMax;
    };

    const int32_t diameter = lerp(kLauncherCollapsedCenterDiameter, kLauncherCenterDiameter);
    const int32_t radius = diameter / 2;
    const int32_t border_width = progress >= 500 ? 2 : 1;
    const int32_t inner_radius = radius - border_width;
    const int32_t radius_sq = radius * radius;
    const int32_t inner_sq = inner_radius * inner_radius;
    const uint16_t fill = lv_color_to_u16(lv_color_hex(0x05070B));
    const uint16_t border = lv_color_to_u16(lv_color_hex(0x161B27));
    const int32_t y_first = std::max(kLauncherCenterY - radius, target.y_begin);
    const int32_t y_last = std::min(
        kLauncherCenterY + radius, target.y_begin + target.rows - 1);
    for (int32_t y = y_first; y <= y_last; ++y) {
        const int32_t dy = y - kLauncherCenterY;
        for (int32_t x = kLauncherCenterX - radius; x <= kLauncherCenterX + radius; ++x) {
            const int32_t dx = x - kLauncherCenterX;
            const int32_t d2 = dx * dx + dy * dy;
            if (d2 > radius_sq) {
                continue;
            }
            player_home_launcher_raster_pixel(
                target, x, y,
                d2 >= inner_sq ? border : fill,
                d2 >= inner_sq ? static_cast<uint8_t>(LV_OPA_COVER) : static_cast<uint8_t>(245U));
        }
    }

    const LauncherMenuItemDef &selected = kLauncherItems[g_launcher_selected_index];
    const int32_t icon_scale = 78 + (54 * progress) / kLauncherAnimProgressMax;
    player_home_launcher_raster_icon(
        target,
        selected.icon,
        kLauncherCenterX + (selected.optical_x * progress) / kLauncherAnimProgressMax,
        kLauncherCenterY + (selected.optical_y * progress) / kLauncherAnimProgressMax,
        icon_scale,
        lv_color_to_u16(lv_color_hex(kLauncherAccentRgb)));
}

static bool player_home_launcher_strip_load_run(
    LauncherStripComposeContext *context)
{
    if (context == nullptr || context->src == nullptr) {
        return false;
    }
    if (context->run_remaining != 0U) {
        return true;
    }
    if (context->src >= context->src_end) {
        return false;
    }

    const uint8_t control = *context->src++;
    context->run_remaining = static_cast<uint32_t>(control & 0x7FU) + 1U;
    context->repeat_run = (control & 0x80U) != 0U;
    if (context->repeat_run) {
        if (context->src >= context->src_end) {
            return false;
        }
        context->repeat_value = *context->src++;
    }
    else if (context->run_remaining > static_cast<uint32_t>(context->src_end - context->src)) {
        return false;
    }
    return true;
}

static inline void player_home_launcher_apply_packed_pair(
    uint8_t packed,
    uint16_t *dst)
{
    const uint8_t mode = g_launcher_pair_mode[packed];
    if (mode == 0U) {
        return;
    }
    if (mode == 1U) {
        const uint32_t pair_color = g_launcher_color_pair_wire_lut[packed];
        memcpy(dst, &pair_color, sizeof(pair_color));
        return;
    }

    const uint8_t left = static_cast<uint8_t>((packed >> 4) & 0x0FU);
    const uint8_t right = static_cast<uint8_t>(packed & 0x0FU);
    const uint8_t left_alpha = g_launcher_index_alpha[left];
    const uint8_t right_alpha = g_launcher_index_alpha[right];
    if (left_alpha == LV_OPA_COVER) {
        dst[0] = player_home_launcher_wire565(g_launcher_index_color565[left]);
    }
    else if (left_alpha != 0U) {
        const uint16_t native_background = player_home_launcher_wire565(dst[0]);
        dst[0] = player_home_launcher_wire565(player_home_launcher_blend_rgb565(
            native_background, g_launcher_index_color565[left], left_alpha));
    }
    if (right_alpha == LV_OPA_COVER) {
        dst[1] = player_home_launcher_wire565(g_launcher_index_color565[right]);
    }
    else if (right_alpha != 0U) {
        const uint16_t native_background = player_home_launcher_wire565(dst[1]);
        dst[1] = player_home_launcher_wire565(player_home_launcher_blend_rgb565(
            native_background, g_launcher_index_color565[right], right_alpha));
    }
}

static esp_err_t player_home_launcher_compose_strip(
    void *opaque,
    uint16_t source_y,
    uint16_t rows,
    uint16_t width,
    uint8_t *dst_wire_rgb565,
    size_t dst_bytes)
{
    LauncherStripComposeContext *context =
        static_cast<LauncherStripComposeContext *>(opaque);
    if (context == nullptr || context->asset == nullptr || context->surface == nullptr ||
        dst_wire_rgb565 == nullptr || width != kLauncherPanelSize || rows == 0U ||
        dst_bytes < static_cast<size_t>(width) * rows * sizeof(uint16_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t expected_pair =
        static_cast<uint32_t>(source_y) * static_cast<uint32_t>(width / 2U);
    if (context->pair_index != expected_pair) {
        ESP_LOGE(TAG,
            "Launcher Strip 顺序异常：expected_pair=%u actual=%u y=%u rows=%u",
            static_cast<unsigned>(expected_pair),
            static_cast<unsigned>(context->pair_index),
            static_cast<unsigned>(source_y),
            static_cast<unsigned>(rows));
        return ESP_ERR_INVALID_STATE;
    }

    // R.36.6.2：背景 copy 与 native→wire byte-order 转换合并成一次 32-bit pair 搬运。
    // 这样不再先 memcpy 整个 strip、再由 display 层第二次完整扫描 staging 做 byte swap。
    for (uint16_t row = 0U; row < rows; ++row) {
        const uint8_t *src_row = context->surface +
            static_cast<size_t>(source_y + row + kLauncherPanelShownY) * kLauncherSurfaceStride +
            static_cast<size_t>(kLauncherPanelX) * sizeof(uint16_t);
        uint8_t *dst_row = dst_wire_rgb565 +
            static_cast<size_t>(row) * width * sizeof(uint16_t);
        const bool pair_aligned =
            ((reinterpret_cast<uintptr_t>(src_row) | reinterpret_cast<uintptr_t>(dst_row)) & 0x3U) == 0U &&
            (width & 1U) == 0U;
        if (pair_aligned) {
            const uint32_t *src32 = reinterpret_cast<const uint32_t *>(src_row);
            uint32_t *dst32 = reinterpret_cast<uint32_t *>(dst_row);
            for (uint16_t pair = 0U; pair < width / 2U; ++pair) {
                dst32[pair] = player_home_launcher_wire565_pair(src32[pair]);
            }
        }
        else {
            const uint16_t *src16 = reinterpret_cast<const uint16_t *>(src_row);
            uint16_t *dst16 = reinterpret_cast<uint16_t *>(dst_row);
            for (uint16_t x = 0U; x < width; ++x) {
                dst16[x] = player_home_launcher_wire565(src16[x]);
            }
        }
    }

    uint16_t *pixels = reinterpret_cast<uint16_t *>(dst_wire_rgb565);
    const uint32_t strip_pairs =
        static_cast<uint32_t>(rows) * static_cast<uint32_t>(width / 2U);
    uint32_t local_pair = 0U;
    while (local_pair < strip_pairs) {
        if (!player_home_launcher_strip_load_run(context)) {
            ESP_LOGE(TAG,
                "Launcher I4 流提前结束：frame_progress=%d pair=%u/%u",
                static_cast<int>(context->asset->progress),
                static_cast<unsigned>(context->pair_index),
                static_cast<unsigned>(kLauncherAnimationAssetPixelBytes));
            return ESP_ERR_INVALID_SIZE;
        }

        const uint32_t take = std::min(
            context->run_remaining, strip_pairs - local_pair);
        uint16_t *dst = pixels + local_pair * 2U;

        if (context->repeat_run) {
            const uint8_t packed = context->repeat_value;
            const uint8_t mode = g_launcher_pair_mode[packed];
            if (mode == 0U) {
                // 大段透明 pair 直接跨过，不再逐 pair 进入状态机。
                context->fast_skip_pairs += take;
            }
            else if (mode == 1U) {
                // 两个像素都全不透明时，一个32bit颜色对直接批量覆盖。
                const uint32_t pair_color = g_launcher_color_pair_wire_lut[packed];
                uint32_t *dst32 = reinterpret_cast<uint32_t *>(dst);
                std::fill_n(dst32, take, pair_color);
                context->fast_fill_pairs += take;
            }
            else {
                for (uint32_t i = 0U; i < take; ++i) {
                    player_home_launcher_apply_packed_pair(packed, dst + i * 2U);
                }
                context->repeat_blend_pairs += take;
            }
        }
        else {
            // literal run 本来就很短；只对其实际 packed pair 做逐项处理。
            for (uint32_t i = 0U; i < take; ++i) {
                player_home_launcher_apply_packed_pair(context->src[i], dst + i * 2U);
            }
            context->src += take;
            context->literal_pairs += take;
        }

        context->run_remaining -= take;
        context->pair_index += take;
        local_pair += take;
    }

    const LauncherStripRasterTarget target = {
        pixels,
        static_cast<int32_t>(source_y),
        static_cast<int32_t>(rows)};
    player_home_launcher_raster_center(target, context->asset->progress);

    if (static_cast<uint32_t>(source_y) + rows == kLauncherPanelSize) {
        if (context->pair_index != kLauncherAnimationAssetPixelBytes ||
            context->run_remaining != 0U || context->src != context->src_end) {
            ESP_LOGE(TAG,
                "Launcher I4 流长度异常：pairs=%u/%u src=%u/%u remain=%u",
                static_cast<unsigned>(context->pair_index),
                static_cast<unsigned>(kLauncherAnimationAssetPixelBytes),
                static_cast<unsigned>(context->src - context->asset->data),
                static_cast<unsigned>(context->asset->size),
                static_cast<unsigned>(context->run_remaining));
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

static bool player_home_launcher_decode_cached_frame(uint8_t frame_index)
{
    if (g_launcher_surface_source == nullptr || !g_launcher_surface_lease_ready ||
        frame_index >= kLauncherAnimationAssetFrameCount) {
        return false;
    }
    // R.36.6 不再生成完整 340x340 Work；这里只更新离散帧身份。
    // 真正 I4 解码在 BoundedSPI 请求每个 staging strip 时顺序完成。
    g_launcher_frame_index = frame_index;
    return true;
}

static void player_home_launcher_reset_bounded_stats()
{
    g_launcher_bounded_present_count = 0U;
    g_launcher_bounded_present_us = 0U;
    g_launcher_bounded_present_max_us = 0U;
    g_launcher_bounded_stream_us = 0U;
    g_launcher_bounded_stream_max_us = 0U;
    g_launcher_bounded_failures = 0U;
}

static bool player_home_launcher_present_work_bounded()
{
    if (!g_launcher_bounded_session_active || !g_launcher_surface_lease_ready ||
        g_launcher_surface_source == nullptr ||
        g_launcher_frame_index >= kLauncherAnimationAssetFrameCount ||
        !display_launcher_bounded_spi_session_active()) {
        return false;
    }

    const LauncherAnimationFrameAsset &asset = g_launcher_animation_frames[g_launcher_frame_index];
    LauncherStripComposeContext compose = {};
    compose.asset = &asset;
    compose.surface = g_launcher_surface_source;
    compose.src = asset.data;
    compose.src_end = asset.data + asset.size;

    DisplayBoundedSpiStats stats = {};
    const esp_err_t ret = display_launcher_bounded_spi_present_stream(
        player_home_launcher_compose_strip,
        &compose,
        static_cast<uint16_t>(kLauncherPanelX),
        static_cast<uint16_t>(kLauncherPanelShownY),
        static_cast<uint16_t>(kLauncherPanelSize),
        static_cast<uint16_t>(kLauncherPanelSize),
        true,
        false,
        &stats);
    if (ret != ESP_OK) {
        ++g_launcher_bounded_failures;
        ESP_LOGW(TAG,
            "Launcher WireStripFrame 失败：gen=%u frame=%u ret=%s failures=%u",
            static_cast<unsigned>(stats.generation),
            static_cast<unsigned>(g_launcher_frame_index),
            esp_err_to_name(ret),
            static_cast<unsigned>(g_launcher_bounded_failures));
        return false;
    }

#if APP_DIAG_LAUNCHER_PERFORMANCE
    ++g_launcher_frame_decode_count;
    g_launcher_frame_decode_us += stats.producer_us;
    if (stats.producer_us > g_launcher_frame_decode_max_us) {
        g_launcher_frame_decode_max_us = stats.producer_us;
    }
    ++g_launcher_bounded_present_count;
    g_launcher_bounded_present_us += stats.total_us;
    g_launcher_bounded_stream_us += stats.stream_us;
    if (stats.total_us > g_launcher_bounded_present_max_us) {
        g_launcher_bounded_present_max_us = stats.total_us;
    }
    if (stats.stream_us > g_launcher_bounded_stream_max_us) {
        g_launcher_bounded_stream_max_us = stats.stream_us;
    }
    if (g_launcher_bounded_present_count <= 3U ||
        g_launcher_frame_index == static_cast<uint8_t>(kLauncherAnimationAssetFrameCount - 1U)) {
        HOME_LAUNCHER_PERF_LOGI(
            "Launcher frame：gen=%u seq=%u..%u frame=%u total=%uus compose=%uus stream=%uus swap=%uus wait=%uus pairs(skip/fill/lit/blend)=%u/%u/%u/%u chunks=%u staging=%u行×%u PanelWork=0B",
            static_cast<unsigned>(stats.generation),
            static_cast<unsigned>(stats.first_sequence),
            static_cast<unsigned>(stats.last_sequence),
            static_cast<unsigned>(g_launcher_frame_index),
            static_cast<unsigned>(stats.total_us),
            static_cast<unsigned>(stats.producer_us),
            static_cast<unsigned>(stats.stream_us),
            static_cast<unsigned>(stats.byte_swap_us),
            static_cast<unsigned>(stats.queue_wait_us),
            static_cast<unsigned>(compose.fast_skip_pairs),
            static_cast<unsigned>(compose.fast_fill_pairs),
            static_cast<unsigned>(compose.literal_pairs),
            static_cast<unsigned>(compose.repeat_blend_pairs),
            static_cast<unsigned>(stats.chunks),
            static_cast<unsigned>(stats.staging_rows),
            static_cast<unsigned>(stats.staging_buffers));
    }
#endif
    return true;
}

static void player_home_launcher_switch_to_lvgl_fallback()
{
    if (!g_launcher_bounded_session_active) {
        return;
    }
    // R.36.3：先结束 BoundedSPI session，释放固定 DMA staging。display 层保证只有在
    // 所有 raw descriptor 已有界回收后才会返回；无法回收的极端状态直接受控重启。
    display_launcher_bounded_spi_session_end();
    g_launcher_bounded_session_active = false;
    if (g_launcher_backdrop != nullptr) {
        lv_obj_set_style_bg_opa(g_launcher_backdrop, kLauncherBackdropOpa, 0);
    }
    // R.36.6：没有整帧 Canvas。降级后直接启用 R.32 LVGL 实时圆弧绘制。
    g_launcher_frame_cache_active = false;
    if (g_launcher_panel != nullptr) {
        lv_obj_invalidate(g_launcher_panel);
    }
    // BoundedSPI 正常路径让 Launcher LVGL 根对象保持隐藏；降级时必须重新显示根对象，
    // 否则 R.32 fallback 虽已重绘但仍不可见。
    if (g_launcher != nullptr) {
        lv_obj_remove_flag(g_launcher, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_launcher);
        screen_lock_simple_raise();
        lv_obj_invalidate(g_launcher);
    }
    ESP_LOGW(TAG, "Launcher BoundedSPI 已降级到 LVGL 实时圆弧安全路径");
}

static void player_home_launcher_try_surface_rebind()
{
    if (!g_launcher_visible || g_launcher_motion != LauncherMotionState::Shown ||
        !g_launcher_frame_cache_active || !g_launcher_surface_lease_ready ||
        g_launcher_surface_source == nullptr || !player_state_is_ready()) {
        return;
    }

    const uint32_t current_track = static_cast<uint32_t>(player_state_get_index());
    if (current_track == g_launcher_surface_lease_track) {
        return;
    }

    LauncherSurfaceCandidate next = {};
    if (!player_home_launcher_acquire_candidate(current_track, &next)) {
        // 有真实封面但 Surface 尚未 ready 时继续旧背景；真正无封面歌曲会直接取得 TF 替补图。
        return;
    }
    if (!player_state_is_ready() || static_cast<uint32_t>(player_state_get_index()) != current_track) {
        player_home_launcher_release_candidate(&next);
        return;
    }

    LauncherSurfaceCandidate old = player_home_launcher_detach_current_surface();
    const uint8_t frame_index = g_launcher_frame_index < kLauncherAnimationAssetFrameCount
        ? g_launcher_frame_index
        : static_cast<uint8_t>(kLauncherAnimationAssetFrameCount - 1U);

    // 先同时 pin A/B，再把 source 指向 B。B 可以是真实 CoverSurface，也可以是当前视图
    // 对应的无封面 TF 替补图；旧 A 一直保留到 B frame 提交完成。
    player_home_launcher_commit_candidate(&next);

    if (!player_home_launcher_decode_cached_frame(frame_index)) {
        LauncherSurfaceCandidate failed = player_home_launcher_detach_current_surface();
        player_home_launcher_release_candidate(&failed);
        player_home_launcher_commit_candidate(&old);
        (void) player_home_launcher_decode_cached_frame(frame_index);
        ESP_LOGW(TAG,
            "Launcher 背景换绑取消：%u -> %u，新背景/帧身份校验失败，继续旧背景",
            static_cast<unsigned>(g_launcher_surface_lease_track),
            static_cast<unsigned>(current_track));
        return;
    }

    DisplayBoundedSpiStats base_stats = {};
    bool bounded_ok = g_launcher_bounded_session_active;
    if (bounded_ok) {
        const esp_err_t base_ret = display_launcher_bounded_spi_present(
            g_launcher_surface_source,
            0U,
            0U,
            FAKEPOD_LCD_WIDTH,
            FAKEPOD_LCD_HEIGHT,
            false,
            true,
            &base_stats);
        if (base_ret != ESP_OK) {
            ESP_LOGW(TAG,
                "Launcher 新背景 BoundedSPI 提交失败：track=%u ret=%s，降级LVGL",
                static_cast<unsigned>(current_track),
                esp_err_to_name(base_ret));
            player_home_launcher_switch_to_lvgl_fallback();
            bounded_ok = false;
        }
        else if (!player_home_launcher_present_work_bounded()) {
            ESP_LOGW(TAG,
                "Launcher 新背景 StripFrame 提交失败：track=%u，降级LVGL",
                static_cast<unsigned>(current_track));
            player_home_launcher_switch_to_lvgl_fallback();
            bounded_ok = false;
        }
    }

    const uint32_t old_track = old.track;
    const uint32_t acquire_us = g_launcher_surface_lease_us;
    const bool fallback_source = g_launcher_surface_is_fallback;
    player_home_launcher_release_candidate(&old);
    HOME_DISPLAY_LOGI(
        "Launcher背景换绑：%u -> %u acquire=%uus gen=%u base=%uus stream=%uus frame=%u bounded=%d fallback=%d",
        static_cast<unsigned>(old_track),
        static_cast<unsigned>(current_track),
        static_cast<unsigned>(acquire_us),
        static_cast<unsigned>(base_stats.generation),
        static_cast<unsigned>(base_stats.total_us),
        static_cast<unsigned>(base_stats.stream_us),
        static_cast<unsigned>(frame_index),
        bounded_ok ? 1 : 0,
        fallback_source ? 1 : 0);
}

static bool player_home_launcher_present_frame_bounded(uint8_t frame_index)
{
    if (!g_launcher_bounded_session_active) {
        return false;
    }
    if (g_launcher_frame_index != frame_index && !player_home_launcher_decode_cached_frame(frame_index)) {
        player_home_launcher_switch_to_lvgl_fallback();
        return false;
    }
    if (!player_home_launcher_present_work_bounded()) {
        player_home_launcher_switch_to_lvgl_fallback();
        return false;
    }
    return true;
}

static bool player_home_launcher_begin_bounded_scene()
{
    if (!g_launcher_frame_cache_active || !g_launcher_surface_lease_ready || g_launcher_surface_source == nullptr ||
        !kLauncherBoundedSpiEnabled || !display_launcher_bounded_spi_available()) {
        return false;
    }

    const esp_err_t begin_ret = display_launcher_bounded_spi_session_begin();
    if (begin_ret != ESP_OK) {
        ESP_LOGW(TAG,
            "Launcher BoundedSPI Session 启动失败：%s，回退LVGL",
            esp_err_to_name(begin_ret));
        return false;
    }

    // Session 只常驻 DMA staging / generation，不长期占用 SPI bus；每个 present 自己有界
    // 回收 Panel IO pending transaction，再提交完全由本层追踪的 raw SPI descriptor。
    DisplayBoundedSpiStats stats = {};
    const esp_err_t ret = display_launcher_bounded_spi_present(
        g_launcher_surface_source,
        0U,
        0U,
        FAKEPOD_LCD_WIDTH,
        FAKEPOD_LCD_HEIGHT,
        false,
        true,
        &stats);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
            "Launcher BoundedSPI 底图提交失败：%s，回退LVGL",
            esp_err_to_name(ret));
        display_launcher_bounded_spi_session_end();
        return false;
    }

    g_launcher_bounded_session_active = true;
    player_home_launcher_reset_bounded_stats();
    g_launcher_frame_index = 0U;
    HOME_LAUNCHER_PERF_LOGI(
        "Launcher scene：gen=%u lease=%uus track=%u fullPresent=%uus drain=%uus stream=%uus base=%s owner=bounded-spi root=hidden",
        static_cast<unsigned>(stats.generation),
        static_cast<unsigned>(g_launcher_surface_lease_us),
        static_cast<unsigned>(g_launcher_surface_lease_track),
        static_cast<unsigned>(stats.total_us),
        static_cast<unsigned>(stats.panel_drain_us),
        static_cast<unsigned>(stats.stream_us),
        g_launcher_surface_is_fallback ? "TF-fallback.dimmed" : "CoverSurface.dimmed");
    return true;
}

static bool player_home_present_current_cover_bounded(const char *reason)
{
    if (!player_state_is_ready() || !display_launcher_bounded_spi_session_active()) {
        return false;
    }
    const uint32_t track_index = static_cast<uint32_t>(player_state_get_index());
    CoverSurfaceLease lease = {};
    const bool has_cover_surface = cover_surface_cache_acquire(track_index, &lease);
    const uint8_t *source = nullptr;
    FallbackCoverImageLease fallback = {};
    bool using_fallback = false;

    if (has_cover_surface) {
        source = g_overlay_visible ? lease.dimmed_rgb565 : lease.normal_rgb565;
    } else {
        MediaArtworkViewV2 artwork = {};
        if (media_library_get_artwork_view(track_index, &artwork) ||
            !fallback_cover_image_acquire(FallbackCoverImageKind::Artwork, &fallback)) {
            return false;
        }
        source = fallback.rgb565;
        using_fallback = source != nullptr &&
            fallback.width == FAKEPOD_LCD_WIDTH && fallback.height == FAKEPOD_LCD_HEIGHT;
        if (!using_fallback) {
            fallback_cover_image_release(&fallback);
            fallback_cover_image_discard_unpinned();
            return false;
        }
    }

    DisplayBoundedSpiStats stats = {};
    const esp_err_t ret = source == nullptr
        ? ESP_ERR_INVALID_STATE
        : display_launcher_bounded_spi_present(
            source,
            0U,
            0U,
            FAKEPOD_LCD_WIDTH,
            FAKEPOD_LCD_HEIGHT,
            false,
            true,
            &stats);
    if (has_cover_surface) {
        cover_surface_cache_release(&lease);
    }
    if (fallback.slot_index != 0xFFU) {
        fallback_cover_image_release(&fallback);
        fallback_cover_image_discard_unpinned();
    }
    if (ret == ESP_OK) {
        HOME_DISPLAY_LOGI(
            "Launcher退出封面恢复：reason=%s track=%lu gen=%u total=%uus drain=%uus stream=%uus fallback=%d",
            reason != nullptr ? reason : "unknown",
            static_cast<unsigned long>(track_index),
            static_cast<unsigned>(stats.generation),
            static_cast<unsigned>(stats.total_us),
            static_cast<unsigned>(stats.panel_drain_us),
            static_cast<unsigned>(stats.stream_us),
            using_fallback ? 1 : 0);
        return true;
    }
    ESP_LOGW(TAG,
        "Launcher 退出封面恢复失败：reason=%s track=%lu ret=%s，交给LVGL重绘",
        reason != nullptr ? reason : "unknown",
        static_cast<unsigned long>(track_index),
        esp_err_to_name(ret));
    return false;
}

static void player_home_launcher_panel_draw_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }
    // R.36.6 BoundedSPI Strip 激活时，R.32 实时圆弧只作为备用路径，不重复绘制。
    if (g_launcher_frame_cache_active) {
        return;
    }

    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    if (layer == nullptr || obj == nullptr) {
        return;
    }

    const int32_t progress = g_launcher_anim_progress < 0
        ? 0
        : (g_launcher_anim_progress > kLauncherAnimProgressMax
            ? kLauncherAnimProgressMax
            : g_launcher_anim_progress);
    if (progress <= 0) {
        return;
    }

    lv_area_t coords = {};
    lv_obj_get_coords(obj, &coords);
    const int32_t cx = coords.x1 + kLauncherCenterX;
    const int32_t cy = coords.y1 + kLauncherCenterY;

    // R.32：对象坐标固定，只让几何从中心向最终半径展开。
    // 1000 时所有数值严格回到 R.31 的最终几何，因此菜单静态外观与命中区域不变。
    auto lerp_progress = [progress](int32_t from, int32_t to) -> int32_t {
        return from + ((to - from) * progress + kLauncherAnimProgressMax / 2) /
            kLauncherAnimProgressMax;
    };
    const int32_t outer_radius = lerp_progress(kLauncherCollapsedOuterRadius, kLauncherOuterRadius);
    const int32_t ring_width = lerp_progress(kLauncherCollapsedRingWidth, kLauncherRingWidth);
    const int32_t center_diameter = lerp_progress(kLauncherCollapsedCenterDiameter, kLauncherCenterDiameter);
    const int32_t icon_radius = lerp_progress(kLauncherCollapsedIconRadius, kLauncherIconRadius);
    // 动画期间保持扇区/图标不透明，避免为了淡入额外支付 alpha blend；
    // “展开感”完全由半径/线宽/图标位置与缩放提供。
    const lv_opa_t geometry_opa = LV_OPA_COVER;

    // 外圈图标稍晚于扇区出现，避免展开起点 7 个图标堆在中心。
    static constexpr int32_t kOuterIconDelay = 140;
    const int32_t icon_progress = progress <= kOuterIconDelay
        ? 0
        : ((progress - kOuterIconDelay) * kLauncherAnimProgressMax) /
            (kLauncherAnimProgressMax - kOuterIconDelay);
    const lv_opa_t icon_opa = LV_OPA_COVER;
    const int32_t outer_icon_scale = 62 + (38 * icon_progress) / kLauncherAnimProgressMax;

    // 1) 七个扇区。只修改 radius/width/opa，不移动 340x340 panel。
    for (uint8_t i = 0; i < kLauncherItemCount; ++i) {
        const LauncherMenuItemDef &item = kLauncherItems[i];
        lv_draw_arc_dsc_t arc = {};
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_hex(
            i == g_launcher_selected_index ? kLauncherSelectedSectorRgb : item.idle_rgb);
        arc.width = ring_width;
        arc.start_angle = player_home_launcher_normalize_angle(
            static_cast<int16_t>(item.center_angle - kLauncherSectorHalfSpanDeg));
        arc.end_angle = player_home_launcher_normalize_angle(
            static_cast<int16_t>(item.center_angle + kLauncherSectorHalfSpanDeg));
        arc.center.x = cx;
        arc.center.y = cy;
        arc.radius = outer_radius;
        arc.opa = geometry_opa;
        arc.rounded = 0U;
        lv_draw_arc(layer, &arc);
    }

    // 2) 七个图标沿同一极坐标半径从中心向最终位置展开。
    if (icon_progress > 0) {
        for (uint8_t i = 0; i < kLauncherItemCount; ++i) {
            const LauncherMenuItemDef &item = kLauncherItems[i];
            const int32_t icon_x = kLauncherCenterX +
                (static_cast<int32_t>(item.unit_x_10000) * icon_radius +
                    (item.unit_x_10000 >= 0 ? 5000 : -5000)) / 10000 +
                (item.optical_x * icon_progress) / kLauncherAnimProgressMax;
            const int32_t icon_y = kLauncherCenterY +
                (static_cast<int32_t>(item.unit_y_10000) * icon_radius +
                    (item.unit_y_10000 >= 0 ? 5000 : -5000)) / 10000 +
                (item.optical_y * icon_progress) / kLauncherAnimProgressMax;
            player_home_launcher_draw_icon(
                layer,
                item.icon,
                coords.x1 + icon_x,
                coords.y1 + icon_y,
                lv_color_hex(0xF8FAFF),
                outer_icon_scale,
                icon_opa);
        }
    }

    // 3) 中心圆同步展开。progress=1000 时与 R.31 的 116px 中心圆完全一致。
    lv_draw_rect_dsc_t center = {};
    lv_draw_rect_dsc_init(&center);
    center.bg_color = lv_color_hex(0x05070B);
    center.bg_opa = 245;
    center.radius = LV_RADIUS_CIRCLE;
    center.border_width = progress >= 500 ? 2 : 1;
    center.border_color = lv_color_hex(0x161B27);
    center.border_opa = geometry_opa;
    const int32_t half = center_diameter / 2;
    lv_area_t center_area = {cx - half, cy - half, cx + half, cy + half};
    lv_draw_rect(layer, &center, &center_area);

    const int32_t center_icon_scale = 78 + (54 * progress) / kLauncherAnimProgressMax;
    player_home_launcher_draw_icon(
        layer,
        kLauncherItems[g_launcher_selected_index].icon,
        cx + (kLauncherItems[g_launcher_selected_index].optical_x * progress) /
            kLauncherAnimProgressMax,
        cy + (kLauncherItems[g_launcher_selected_index].optical_y * progress) /
            kLauncherAnimProgressMax,
        lv_color_hex(kLauncherAccentRgb),
        center_icon_scale,
        geometry_opa);
}

static void player_home_launcher_apply_selection()
{
    if (g_launcher_frame_cache_active) {
        // R.36.6：选中项变化只更新 LUT 并重新流式提交当前离散帧；fallback 直接 invalidate panel。
        player_home_launcher_update_frame_lut();
        const uint8_t frame = g_launcher_frame_index == kLauncherFrameInvalid
            ? player_home_launcher_frame_for_progress(g_launcher_anim_progress)
            : g_launcher_frame_index;
        if (player_home_launcher_decode_cached_frame(frame) && g_launcher_bounded_session_active) {
            if (!player_home_launcher_present_work_bounded()) {
                player_home_launcher_switch_to_lvgl_fallback();
            }
        }
        return;
    }
    if (g_launcher_panel != nullptr) {
        lv_obj_invalidate(g_launcher_panel);
    }
}

static int8_t player_home_launcher_hit_test_geometry(
    int32_t screen_x,
    int32_t screen_y,
    int32_t panel_x,
    int32_t panel_y)
{
    const int32_t dx = screen_x - (panel_x + kLauncherCenterX);
    const int32_t dy = screen_y - (panel_y + kLauncherCenterY);
    const int32_t radius_sq = dx * dx + dy * dy;
    const int32_t min_radius_sq = kLauncherTouchInnerRadius * kLauncherTouchInnerRadius;
    const int32_t max_radius_sq = kLauncherTouchOuterRadius * kLauncherTouchOuterRadius;
    if (radius_sq < min_radius_sq || radius_sq > max_radius_sq) {
        return -1;
    }

    // 纯几何命中核心：不读取 Music Launcher 的可见态、对象指针或动画状态。
    // atan2(x, y) 刻意交换参数，使角度系与 Launcher 绘制定义一致：
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

static int8_t player_home_launcher_hit_test(int32_t screen_x, int32_t screen_y)
{
    // Music 自己的事件入口仍保留生命周期门禁；只有 Music 原生 Launcher 完全展开时接收点击。
    if (g_launcher_panel == nullptr || g_launcher_motion != LauncherMotionState::Shown) {
        return -1;
    }

    lv_area_t coords = {};
    lv_obj_get_coords(g_launcher_panel, &coords);
    return player_home_launcher_hit_test_geometry(
        screen_x, screen_y, coords.x1, coords.y1);
}

static void player_home_launcher_progress_anim_exec(void *var, int32_t value)
{
    lv_obj_t *panel = static_cast<lv_obj_t *>(var);
    g_launcher_anim_progress = value < 0
        ? 0
        : (value > kLauncherAnimProgressMax ? kLauncherAnimProgressMax : value);

    if (g_launcher_frame_cache_active) {
        const uint8_t next_frame = player_home_launcher_frame_for_progress(g_launcher_anim_progress);
        if (next_frame != g_launcher_frame_index) {
            if (g_launcher_bounded_session_active) {
                (void) player_home_launcher_present_frame_bounded(next_frame);
            }
            else {
                (void) player_home_launcher_decode_cached_frame(next_frame);
            }
        }
        return;
    }

    if (panel != nullptr) {
        // 预烘焙工作帧不可用时保留 R.32 实时圆弧作为安全回退。
        lv_obj_invalidate(panel);
    }
}

static void player_home_launcher_enter_done(lv_anim_t *anim)
{
    (void)anim;
    if (g_launcher_motion != LauncherMotionState::Entering) {
        return;
    }
    g_launcher_anim_progress = kLauncherAnimProgressMax;
    g_launcher_motion = LauncherMotionState::Shown;
    now_playing_artwork_set_bounded_present_allowed(false);
    if (g_launcher_frame_cache_active) {
        const uint8_t final_frame = static_cast<uint8_t>(kLauncherAnimationAssetFrameCount - 1U);
        if (g_launcher_frame_index != final_frame) {
            if (g_launcher_bounded_session_active) {
                (void) player_home_launcher_present_frame_bounded(final_frame);
            }
            else {
                (void) player_home_launcher_decode_cached_frame(final_frame);
            }
        }
    }
#if APP_DIAG_LAUNCHER_PERFORMANCE
    const uint32_t elapsed = static_cast<uint32_t>(lv_tick_get()) - g_launcher_anim_started_ms;
    HOME_LAUNCHER_PERF_LOGI(
        "Launcher展开：%ums frame=%u/%u lease=%uus compose(avg/max)=%u/%uus present(avg/max)=%u/%uus stream(avg/max)=%u/%uus frames=%u failures=%u bounded=%d",
        static_cast<unsigned>(elapsed),
        static_cast<unsigned>(g_launcher_frame_index),
        static_cast<unsigned>(kLauncherAnimationAssetFrameCount - 1U),
        static_cast<unsigned>(g_launcher_surface_lease_us),
        static_cast<unsigned>(g_launcher_frame_decode_count == 0U ? 0U :
            g_launcher_frame_decode_us / g_launcher_frame_decode_count),
        static_cast<unsigned>(g_launcher_frame_decode_max_us),
        static_cast<unsigned>(g_launcher_bounded_present_count == 0U ? 0U :
            g_launcher_bounded_present_us / g_launcher_bounded_present_count),
        static_cast<unsigned>(g_launcher_bounded_present_max_us),
        static_cast<unsigned>(g_launcher_bounded_present_count == 0U ? 0U :
            g_launcher_bounded_stream_us / g_launcher_bounded_present_count),
        static_cast<unsigned>(g_launcher_bounded_stream_max_us),
        static_cast<unsigned>(g_launcher_bounded_present_count),
        static_cast<unsigned>(g_launcher_bounded_failures),
        g_launcher_bounded_session_active ? 1 : 0);
#endif
}

static void player_home_launcher_leave_done(lv_anim_t *anim)
{
    (void)anim;
    if (g_launcher_motion != LauncherMotionState::Leaving) {
        return;
    }
    g_launcher_anim_progress = 0;
    g_launcher_motion = LauncherMotionState::Hidden;
    g_launcher_visible = false;
    if (g_launcher != nullptr) {
        lv_obj_add_flag(g_launcher, LV_OBJ_FLAG_HIDDEN);
    }
    const bool used_bounded = g_launcher_bounded_session_active;
    bool restored = false;
    if (used_bounded) {
        if (g_music_visual_mode == MusicVisualMode::Artwork) {
            // Artwork 模式保持 R.36.3：最后一笔恢复当前 Surface 到 GRAM。
            restored = player_home_present_current_cover_bounded("launcher-hide");
        }
        display_launcher_bounded_spi_session_end();
    }
    g_launcher_bounded_session_active = false;

    // C2.4.13：高速 Launcher 在 Cassette 模式下临时隐藏了整个磁带 root。
    // BoundedSPI 已结束后再重新绑定 Cassette，避免 LVGL 与 Launcher 直接写 GRAM 并发。
    if (g_music_visual_mode == MusicVisualMode::Cassette) {
        // fallback 可能临时恢复了 Artwork；回到 Cassette 前明确关掉。
        now_playing_artwork_set_active(false);
        if (g_launcher_cassette_scene_hidden) {
            (void)cassette_view_set_active(true);
        }
        cassette_view_set_launcher_suspended(false);
        g_launcher_cassette_scene_hidden = false;
        // Cassette 不是一张全屏 Cover，Launcher 退出后必须让 LVGL 重合成完整磁带场景。
        lv_obj_t *screen = lv_screen_active();
        if (screen != nullptr) lv_obj_invalidate(screen);
    } else if (used_bounded && !restored) {
        lv_obj_t *screen = lv_screen_active();
        if (screen != nullptr) lv_obj_invalidate(screen);
    }
    g_artwork_resume_without_invalidation =
        restored && g_music_visual_mode == MusicVisualMode::Artwork;
    if (g_launcher_backdrop != nullptr) {
        lv_obj_set_style_bg_opa(g_launcher_backdrop, kLauncherBackdropOpa, 0);
    }
    now_playing_artwork_set_bounded_present_allowed(
        g_music_visual_mode == MusicVisualMode::Artwork);
#if APP_DIAG_LAUNCHER_PERFORMANCE
    const uint32_t elapsed = static_cast<uint32_t>(lv_tick_get()) - g_launcher_anim_started_ms;
    HOME_LAUNCHER_PERF_LOGI(
        "Launcher收拢：%ums frame=%u compose(avg/max)=%u/%uus present(avg/max)=%u/%uus frames=%u failures=%u bounded=%d",
        static_cast<unsigned>(elapsed),
        static_cast<unsigned>(g_launcher_frame_index),
        static_cast<unsigned>(g_launcher_frame_decode_count == 0U ? 0U :
            g_launcher_frame_decode_us / g_launcher_frame_decode_count),
        static_cast<unsigned>(g_launcher_frame_decode_max_us),
        static_cast<unsigned>(g_launcher_bounded_present_count == 0U ? 0U :
            g_launcher_bounded_present_us / g_launcher_bounded_present_count),
        static_cast<unsigned>(g_launcher_bounded_present_max_us),
        static_cast<unsigned>(g_launcher_bounded_present_count),
        static_cast<unsigned>(g_launcher_bounded_failures),
        used_bounded ? 1 : 0);
#endif
    g_launcher_frame_cache_active = false;
    player_home_launcher_release_surface_lease();
}

static void player_home_launcher_start_animation(
    int32_t target_progress,
    LauncherMotionState motion,
    lv_anim_completed_cb_t completed_cb)
{
    if (g_launcher_panel == nullptr) {
        return;
    }

    lv_anim_delete(g_launcher_panel, player_home_launcher_progress_anim_exec);
    const int32_t start_progress = g_launcher_anim_progress;
    g_launcher_motion = motion;
    g_launcher_anim_started_ms = static_cast<uint32_t>(lv_tick_get());
    g_launcher_frame_decode_count = 0U;
    g_launcher_frame_decode_us = 0U;
    g_launcher_frame_decode_max_us = 0U;
    if (g_launcher_bounded_session_active) {
        player_home_launcher_reset_bounded_stats();
    }

    lv_anim_t animation = {};
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, g_launcher_panel);
    lv_anim_set_exec_cb(&animation, player_home_launcher_progress_anim_exec);
    lv_anim_set_values(&animation, start_progress, target_progress);
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

    // 选中项以 AppManager 发布的目标为准；从 Ebook 返回时中心图标直接指向“电子书”。
    const AppId launcher_target = app_manager_launcher_target();
    if (launcher_target != AppId::None &&
        static_cast<uint8_t>(launcher_target) < kLauncherItemCount) {
        g_launcher_selected_index = static_cast<uint8_t>(launcher_target);
    }

    player_home_overlay_hide();
    now_playing_artwork_set_bounded_present_allowed(false);
    if (!g_launcher_visible) {
        g_launcher_visible = true;
        // BoundedSPI 动画期间主页 Audio/Artwork timer 不应在背后触发任何 LVGL invalidation。
        // 这里立即暂停 timer，但先不要释放 Artwork UI lease：Launcher 必须先 acquire 同一
        // current Surface，再把主页 lease 交出去，避免 pin=0 的短暂可驱逐窗口。
        if (g_audio_timer != nullptr) lv_timer_pause(g_audio_timer);
        if (g_artwork_timer != nullptr) lv_timer_pause(g_artwork_timer);
        g_launcher_motion = LauncherMotionState::Entering;
        g_launcher_anim_progress = 0;
        g_launcher_frame_cache_active = false;
        g_launcher_bounded_session_active = false;
        player_home_launcher_release_surface_lease();
        g_launcher_frame_index = kLauncherFrameInvalid;
        if (g_launcher_backdrop != nullptr) {
            lv_obj_set_style_bg_opa(g_launcher_backdrop, kLauncherBackdropOpa, 0);
        }

        // R.37.3.2：先由 Launcher acquire 当前 Surface，再 suspend Artwork UI。这样当前槽的
        // pin_count 从 UI=1 → UI+Launcher=2 → Launcher=1，整个 handoff 期间不会出现 pin=0。
        // 旧实现先把 g_background_timers_running 置 false，却漏掉 set_active(false)，导致后续
        // QoS 因状态相等提前 return，主页旧 lease 永久占住一个槽；A→B 后形成 A/B 双 pin，
        // B→C 即无交换槽可用。
        const bool cassette_launcher = g_music_visual_mode == MusicVisualMode::Cassette;
        if (cassette_launcher) {
            // 与 C2.4.12 Controls Freeze 保持同一原则：Launcher 期间不允许机械层继续刷新。
            cassette_view_set_launcher_suspended(true);
        }

        const bool launcher_surface_ready =
            g_launcher_frame_cache_ready && player_home_launcher_prepare_surface_lease();
        const bool bounded_candidate =
            launcher_surface_ready && kLauncherBoundedSpiEnabled && display_launcher_bounded_spi_available();

        // Cassette 与 Artwork 使用同样的 Surface handoff：必须先让 Launcher pin 当前 Surface，
        // 再隐藏/释放 Cassette 自己的 lease，避免出现 pin=0 窗口。仅高速候选路径隐藏整页；
        // 如果 Surface/BoundedSPI 不可用，保留静态 Cassette 作为 LVGL fallback 背景。
        g_launcher_cassette_scene_hidden = false;
        if (cassette_launcher && bounded_candidate) {
            // 与 Artwork 的 quiet suspend 一样，handoff 时不留下稍后才刷出的 460x460 LVGL
            // invalidation；否则 BoundedSPI 已接管 GRAM 后，迟到的主页重绘仍可能叠回磁带组件。
            lv_display_t *display = lv_display_get_default();
            const bool invalidation_was_enabled =
                display != nullptr && lv_display_is_invalidation_enabled(display);
            if (invalidation_was_enabled) lv_display_enable_invalidation(display, false);
            g_launcher_cassette_scene_hidden = cassette_view_set_active(false);
            if (invalidation_was_enabled) lv_display_enable_invalidation(display, true);
        }

        now_playing_artwork_set_active(false, bounded_candidate);
        g_background_timers_running = false;
        g_artwork_resume_without_invalidation = false;

        // R.36.6：pin 当前 dimmed CoverSurface 后直接启动 BoundedSPI Strip Session。
        // 若 session 无法安全启动，不再创建 RGB565 Canvas，而是立即回退 R.32 LVGL 实时圆弧。
        if (launcher_surface_ready) {
            player_home_launcher_update_frame_lut();
            g_launcher_frame_cache_active = true;
            bool bounded_scene_started = false;
            if (kLauncherBoundedSpiEnabled && display_launcher_bounded_spi_available()) {
                bounded_scene_started = player_home_launcher_begin_bounded_scene();
            }
            if (!bounded_scene_started) {
                g_launcher_bounded_session_active = false;
                g_launcher_frame_cache_active = false;
                // quiet suspend 只适用于真正接管物理 GRAM 的 BoundedSPI。若启动失败转 LVGL fallback，
                // 立即恢复主页 Artwork source/lease，让 fallback 仍有合法背景。
                if (bounded_candidate) {
                    now_playing_artwork_set_active(true);
                }
            }
        }

        // R.36.6 BoundedSPI 模式由 Strip Compositor 把中心圆/图标直接合成进 DMA strip；
        // fallback 则由 R.32 panel DRAW_MAIN 实时绘制，不再维护 Canvas/center overlay。
        if (!g_launcher_frame_cache_active) {
            player_home_launcher_release_surface_lease();
            ESP_LOGW(TAG,
                "Launcher Strip/Surface lease 不可用：track=%u，当前展开回退 LVGL 实时圆弧",
                static_cast<unsigned>(player_state_is_ready() ? player_state_get_index() : 0U));
        }
        if (g_launcher_bounded_session_active) {
            // BoundedSPI 模式下视觉由 GRAM strip stream 持有。Launcher LVGL 根对象从进入前
            // 就保持 hidden，这里刻意不调用任何 LVGL visibility/style API；
            // 点击改由 screen 回调复用同一径向 hit-test。这样全屏 Base 提交后不会再发生一次
            // 460x460 LVGL repaint，也就不会形成“只有中间340x340半透明”的方形边界。
        } else {
            lv_obj_remove_flag(g_launcher, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(g_launcher);
            screen_lock_simple_raise();
        }
    }

    // 对象固定在最终位置；BoundedSPI 模式下 progress 只决定 12 个预烘焙帧中的哪一帧提交。
    player_home_launcher_start_animation(
        kLauncherAnimProgressMax,
        LauncherMotionState::Entering,
        player_home_launcher_enter_done);
    if (!g_launcher_bounded_session_active && !g_launcher_frame_cache_active) {
        player_home_launcher_apply_selection();
    }
}

static void player_home_launcher_hide()
{
    if (g_launcher == nullptr || g_launcher_panel == nullptr || !g_launcher_visible ||
        g_launcher_motion == LauncherMotionState::Hidden ||
        g_launcher_motion == LauncherMotionState::Leaving) {
        return;
    }

    // R.32：Backdrop 固定，panel 也固定；离场只把径向几何 progress 收回到 0。
    player_home_launcher_start_animation(
        0,
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

static void player_home_launcher_activate_index(
    uint8_t index,
    int32_t touch_x,
    int32_t touch_y)
{
    bool should_launch = false;
    const AppId target = player_home_launcher_shared_select_index(index, &should_launch);
    if (target == AppId::None) {
        return;
    }

    g_launcher_selected_index = index;
    player_home_launcher_apply_selection();
    HOME_INTERACTION_LOGI("Launcher径向命中：index=%u name=%s touch=(%ld,%ld)",
        static_cast<unsigned>(index),
        kLauncherItems[index].name,
        static_cast<long>(touch_x),
        static_cast<long>(touch_y));

    // 共享Launcher统一语义：所有扇区都先选中；当前 APP 或未注册 APP 不跳转。
    if (!should_launch) {
        return;
    }

    const esp_err_t ret = app_manager_request_foreground(
        target, AppTransitionMode::PreserveBackground);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Launcher进入APP失败：name=%s ret=%s",
            app_manager_name(target), esp_err_to_name(ret));
    }
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

    player_home_launcher_activate_index(
        static_cast<uint8_t>(hit), point.x, point.y);
}

static void player_home_set_volume_adjust_armed(bool armed)
{
    const bool target = armed && g_overlay_visible;
    const bool changed = g_volume_adjust_armed != target;
    g_volume_adjust_armed = target;
    gesture_router_set_vertical_adjust_enabled(g_volume_adjust_armed);
    if (!g_volume_adjust_armed) {
        g_volume_dragging = false;
    }

    // 音量图标同时作为“当前允许纵向调音量”的状态提示；
    // 状态没变时不重复写 style。
    if (changed && g_volume_mode_button != nullptr) {
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

struct PlayerHomeOverlayInvalidationBatch {
    lv_display_t *display = nullptr;
    bool restore_invalidation = false;
};

static PlayerHomeOverlayInvalidationBatch player_home_overlay_begin_atomic_transition()
{
    PlayerHomeOverlayInvalidationBatch batch = {};
    batch.display = lv_display_get_default();
    batch.restore_invalidation =
        batch.display != nullptr && lv_display_is_invalidation_enabled(batch.display);
    if (batch.restore_invalidation) {
        // R.34.2：Overlay 显隐、normal/dimmed source 与 child HIDDEN 属于同一视觉事务。
        // 先暂停 invalidation，避免 13 个 child + lv_image_set_src() 旧/新区重复登记 dirty。
        lv_display_enable_invalidation(batch.display, false);
    }
    return batch;
}

static void player_home_overlay_end_atomic_transition(
    const PlayerHomeOverlayInvalidationBatch &batch)
{
    if (!batch.restore_invalidation || batch.display == nullptr) {
        return;
    }

    lv_display_enable_invalidation(batch.display, true);
    if (g_overlay != nullptr) {
        // normal<->dimmed 本来就改变整张 460x460 封面；事务完成后只登记一次整屏 dirty。
        // 这一帧同时重画封面与最终显隐状态的 Overlay 控件。
        lv_obj_invalidate(g_overlay);
    }
}

static void player_home_overlay_set_backdrop_opa_if_changed(lv_opa_t opa)
{
    if (g_overlay_backdrop == nullptr) {
        return;
    }
    if (lv_obj_get_style_bg_opa(g_overlay_backdrop, LV_PART_MAIN) == opa) {
        return;
    }
    lv_obj_set_style_bg_opa(g_overlay_backdrop, opa, 0);
}

static void player_home_overlay_set_backdrop_clickable(bool clickable)
{
    if (g_overlay_backdrop == nullptr) {
        return;
    }
    const bool current = lv_obj_has_flag(g_overlay_backdrop, LV_OBJ_FLAG_CLICKABLE);
    if (current == clickable) {
        return;
    }
    if (clickable) {
        lv_obj_add_flag(g_overlay_backdrop, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(g_overlay_backdrop, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void player_home_overlay_set_controls_visible(bool visible)
{
    if (g_overlay == nullptr) {
        return;
    }

    // R.34.1：460x460 overlay root 常驻透明，绝不再通过 HIDDEN 切整屏 bounds。
    // 只显隐真正有视觉内容的 child；backdrop 自己始终存在，隐藏态只关闭点击。
    for (int32_t index = 0;; ++index) {
        lv_obj_t *child = lv_obj_get_child(g_overlay, index);
        if (child == nullptr) {
            break;
        }
        if (child == g_overlay_backdrop) {
            continue;
        }

        const bool hidden = lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN);
        if (visible && hidden) {
            lv_obj_remove_flag(child, LV_OBJ_FLAG_HIDDEN);
        } else if (!visible && !hidden) {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void player_home_overlay_apply_dim_path()
{
    if (g_overlay_backdrop == nullptr) {
        return;
    }

    // R.20：CoverSurface 命中时直接切预暗 RGB565，backdrop 保持全透明；
    // 只有 Overlay 可见且压缩 JPEG/PNG 回退路径没有 dimmed Surface 时，才启用旧 alpha 黑层。
    bool fast_dim = false;
    if (g_music_visual_mode == MusicVisualMode::Artwork) {
        fast_dim = now_playing_artwork_set_dimmed(g_overlay_visible);
    } else if (g_music_visual_mode == MusicVisualMode::Cassette) {
        // Round 22：磁带命中预暗 PSRAM RGB565 时，不再叠实时 Alpha 黑层。
        fast_dim = cassette_view_controls_cache_active();
    }
    const lv_opa_t backdrop_opa =
        (g_overlay_visible && !fast_dim)
            ? kOverlayDimOpacity
            : static_cast<lv_opa_t>(LV_OPA_TRANSP);
    player_home_overlay_set_backdrop_opa_if_changed(backdrop_opa);
}

static void player_home_overlay_show()
{
    if (g_overlay == nullptr) {
        return;
    }

    player_home_launcher_hide();

    if (!g_overlay_visible) {
        const PlayerHomeOverlayInvalidationBatch batch =
            player_home_overlay_begin_atomic_transition();
        g_overlay_visible = true;
        player_home_set_volume_adjust_armed(false);
        cassette_view_set_controls_visible(true);
        player_home_overlay_apply_dim_path();
        player_home_overlay_set_controls_visible(true);
        player_home_overlay_set_backdrop_clickable(true);
        player_home_overlay_end_atomic_transition(batch);
    }
    player_home_overlay_arm_timeout();
}

static void player_home_overlay_hide()
{
    if (g_overlay == nullptr || !g_overlay_visible) {
        return;
    }

    const PlayerHomeOverlayInvalidationBatch batch =
        player_home_overlay_begin_atomic_transition();
    player_home_set_volume_adjust_armed(false);
    g_overlay_visible = false;
    if (g_overlay_timer != nullptr) {
        lv_timer_pause(g_overlay_timer);
    }
    cassette_view_set_controls_visible(false);

    // R.34.2：child 显隐和 normal Surface 恢复统一在同一 invalidation 事务中完成。
    player_home_overlay_set_controls_visible(false);
    player_home_overlay_set_backdrop_clickable(false);
    player_home_overlay_apply_dim_path();
    player_home_overlay_end_atomic_transition(batch);
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
    player_home_slider_set_value_if_changed(g_volume_slider, value);

    char label[16] = {};
    snprintf(label, sizeof(label), "%u%%", static_cast<unsigned>(value));
    player_home_label_set_text_if_changed(g_volume_label, label);
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
    HOME_INTERACTION_LOGI(
        "Overlay音量提交：start=%u%% delta_y=%dpx -> %u%%",
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

static void player_home_repaint_controls_after_bounded_present()
{
    if (!now_playing_artwork_take_bounded_present_event()) {
        return;
    }

    // BoundedSPI 直写整屏时，右上角 [锁]（Normal+Locked）也被覆盖，
    // 必须显式标脏重绘，否则新歌封面会把锁图标盖住且不再回来。
    screen_lock_simple_invalidate_lock_icon();

    // BoundedSPI 已经把整张 dimmed/normal Surface 写入 GRAM，因此会暂时覆盖
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
        g_visual_mode_button,
        g_volume_mode_button,
        g_battery_status,
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
    if (g_music_visual_mode == MusicVisualMode::Cassette) {
        cassette_view_update();
        // Overlay 保持打开并切歌时，缓存会先失效再重建；同步切换兜底/快路径。
        if (g_overlay_visible) {
            player_home_overlay_apply_dim_path();
        }
    } else {
        now_playing_artwork_refresh_context();
        now_playing_artwork_update();
        player_home_repaint_controls_after_bounded_present();
    }
    g_last_artwork_bound_track = track_index;

    HOME_INTERACTION_LOGI(
        "%s立即重绑：reason=%s track=%lu cost=%lldus",
        g_music_visual_mode == MusicVisualMode::Cassette ? "磁带封面" : "封面",
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
    // 继续在 LVGL P3 后台醒来。Music APP 真正转入 Background 时，Gesture timer 也由 Adapter 单独暂停。
    const bool should_run =
        g_app_foreground &&
        !library_view_is_visible() &&
        !lyrics_view_is_visible() &&
        !spectrum_view_is_visible() &&
        !g_launcher_visible;
    if (should_run == g_background_timers_running) {
        return;
    }

    g_background_timers_running = should_run;

    // R.36：主页被完整覆盖时释放 Artwork UI lease，让交换槽可立即回收旧 Surface。
    // Launcher 若已用 BoundedSPI 恢复当前封面到 GRAM，则 resume 只同步 LVGL source/lease，
    // 不再额外触发整屏刷新；其它页面恢复仍走普通 invalidation。
    if (!should_run && g_cassette_visual_switch_pending) {
        // 页面被 Launcher/曲库等完整覆盖时取消尚未完成的首次切换，返回主页后仍保持 Artwork。
        g_cassette_visual_switch_pending = false;
    }
    const bool artwork_should_run = should_run &&
        (g_music_visual_mode == MusicVisualMode::Artwork || g_cassette_visual_switch_pending);
    const bool cassette_should_run = should_run &&
        (g_music_visual_mode == MusicVisualMode::Cassette || g_cassette_visual_switch_pending);
    const bool quiet_resume = artwork_should_run && g_artwork_resume_without_invalidation;
    now_playing_artwork_set_active(artwork_should_run, quiet_resume);
    (void)cassette_view_set_active(cassette_should_run);
    if (should_run) {
        g_artwork_resume_without_invalidation = false;
    }

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
    HOME_INTERACTION_LOGI("主页后台timer：%s", should_run ? "恢复" : "暂停");
}

static bool player_home_handle_vertical_track_swipe(
    UiGestureAction action,
    const char *surface_name,
    bool home_visible)
{
    if (action != UiGestureAction::SwipeUpTrack &&
        action != UiGestureAction::SwipeDownTrack) {
        return false;
    }

    const bool next = action == UiGestureAction::SwipeUpTrack;
    const size_t before_track = player_state_get_index();
    const bool ok = next ? player_control_next() : player_control_previous();
    if (!ok) {
        ESP_LOGW(TAG,
            "%s纵向切歌请求失败：%s",
            surface_name != nullptr ? surface_name : "播放页",
            next ? "上滑下一曲" : "下滑上一曲");
        return true;
    }

    const size_t after_track = player_state_get_index();
    HOME_INTERACTION_LOGI(
        "%s纵向切歌：%s track=%lu -> %lu",
        surface_name != nullptr ? surface_name : "播放页",
        next ? "上滑下一曲" : "下滑上一曲",
        static_cast<unsigned long>(before_track),
        static_cast<unsigned long>(after_track));

    if (home_visible) {
        // 主页沿用按钮的 R.19 快速重绑路径。上一曲若因“已播放>3秒”只 Seek(0)，
        // Playlist track 不变，因此不重复刷封面。
        if (after_track != before_track) {
            player_home_artwork_fast_rebind(next ? "swipe-next" : "swipe-prev");
        }
        AudioStateSnapshot snapshot = {};
        if (audio_service_get_snapshot(&snapshot)) {
            player_home_apply_audio_snapshot(snapshot);
        }
    }
    return true;
}

static void player_home_gesture_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g_app_foreground) {
        return;
    }
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

        HOME_INTERACTION_LOGI("歌词页手势：%s", gesture_router_action_name(lyrics_action));
        if (player_home_handle_vertical_track_swipe(lyrics_action, "歌词页", false)) {
            return;
        }
        if (lyrics_action == UiGestureAction::SwipeLeft) {
            lyrics_view_close();
            player_home_resume_from_fullscreen_view("lyrics");
        }
        return;
    }

    // R.35.1：频谱页保留右滑返回主页，同时允许中央纵向 Flick 切歌。
    // Tap 仍由 SpectrumView 自己用于切换显示样式，互不冲突。
    if (spectrum_view_is_visible()) {
        UiGestureAction spectrum_action = UiGestureAction::None;
        if (!gesture_router_take_action(&spectrum_action)) {
            return;
        }
        if (!player_home_page_allows_gesture(PlaybackGestureScope::Spectrum, spectrum_action)) {
            return;
        }

        HOME_INTERACTION_LOGI("频谱页手势：%s", gesture_router_action_name(spectrum_action));
        if (player_home_handle_vertical_track_swipe(spectrum_action, "频谱页", false)) {
            return;
        }
        if (spectrum_action == UiGestureAction::SwipeRight) {
            spectrum_view_close();
            player_home_resume_from_fullscreen_view("spectrum");
        }
        return;
    }

    // R.19：主页可见时每 20ms 只比较一次轻量 Track index。自然 EOF、随机播放或其他入口
    // 改变 Playlist Context 后，立即复用与“歌词/频谱返回主页”相同的封面绑定快路径。
    player_home_artwork_watch_track_change();

    // Launcher 菜单显示时，页面级导航先全部让位；外部轻点由 backdrop 关闭，
    // 若继续滑动任意方向，也直接先收起 Launcher，不在这一版里叠加更多行为。
    if (g_launcher_visible) {
        // R.36.5.1：自然 EOF/自动随机切歌后，SystemLoop 会继续准备新 current Surface。
        // 新 dimmed Surface ready 后在同一 Launcher Session 内原子换绑；未 ready 时保持旧背景。
        player_home_launcher_try_surface_rebind();

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

    HOME_INTERACTION_LOGI("主页手势：%s", gesture_router_action_name(action));
    if (player_home_handle_vertical_track_swipe(action, "封面页", true)) {
        return;
    }
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
    if (!g_app_foreground) {
        return;
    }
    if (g_launcher_visible) {
        // BoundedSPI 模式下 Launcher LVGL 根对象保持 hidden，Tap 会落到主页 screen。
        // 动画完成后在这里复用同一极坐标 hit-test，使物理显示与输入保持解耦。
        if (g_launcher_bounded_session_active &&
            g_launcher_motion == LauncherMotionState::Shown &&
            lv_event_get_code(event) == LV_EVENT_CLICKED &&
            !player_home_click_suppressed() &&
            gesture_router_press_was_tap(kOverlayTapMaxMovePx)) {
            lv_indev_t *indev = lv_indev_active();
            if (indev != nullptr) {
                lv_point_t point = {};
                lv_indev_get_point(indev, &point);
                const int8_t hit = player_home_launcher_hit_test(point.x, point.y);
                if (hit >= 0) {
                    player_home_launcher_activate_index(
                        static_cast<uint8_t>(hit), point.x, point.y);
                } else if (g_launcher_panel != nullptr) {
                    lv_area_t panel = {};
                    lv_obj_get_coords(g_launcher_panel, &panel);
                    const bool inside_panel =
                        point.x >= panel.x1 && point.x <= panel.x2 &&
                        point.y >= panel.y1 && point.y <= panel.y2;
                    // 与原 LVGL hit 行为保持一致：panel 内环外/中心空白不关闭；
                    // 只有点到 340x340 panel 外的 backdrop 区域才收起 Launcher。
                    if (!inside_panel) {
                        player_home_launcher_hide();
                    }
                }
            }
        }
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
    player_home_label_set_text_if_changed(g_current_time, current);
    player_home_label_set_text_if_changed(g_total_time, total);
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

    return snapshot.state == AudioPlaybackState::Playing ||
        snapshot.state == AudioPlaybackState::Paused ||
        snapshot.state == AudioPlaybackState::Seeking;
}

static void player_home_progress_set_enabled(bool enabled)
{
    if (g_progress == nullptr) {
        return;
    }
    const bool disabled = lv_obj_has_state(g_progress, LV_STATE_DISABLED);
    if (enabled && disabled) {
        lv_obj_remove_state(g_progress, LV_STATE_DISABLED);
    } else if (!enabled && !disabled) {
        lv_obj_add_state(g_progress, LV_STATE_DISABLED);
    }
}

static void player_home_cancel_progress_interaction()
{
    cassette_view_set_seek_frozen(false);
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
            // seek_revision只在AudioTask成功定位后递增；此处恢复动画时已经拿到新播放位置。
            cassette_view_set_seek_frozen(false);
        }
    }

    player_home_progress_set_enabled(player_home_snapshot_can_scrub(snapshot));

    if (g_progress_dragging || g_progress_seek_pending) {
        player_home_update_time_labels(g_progress_preview_ms, g_progress_total_ms);
        return;
    }

    if (!same_track || total_ms == 0U) {
        player_home_slider_set_value_if_changed(g_progress, 0);
        player_home_update_time_labels(0U, total_ms);
        return;
    }

    player_home_slider_set_value_if_changed(
        g_progress,
        player_home_progress_value_from_ms(snapshot.position_ms, total_ms));
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
        // 从手指按下进度条开始冻结磁带机械层；松手后继续冻结到AudioTask确认Seek完成。
        cassette_view_set_seek_frozen(true);
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
        player_home_label_set_text_if_changed(g_title, "暂无歌曲");
        player_home_label_set_text_if_changed(g_artist, "");
        player_home_label_set_text_if_changed(g_track_info, "音乐库为空");
        return;
    }

    const size_t index = player_state_get_index();
    const size_t list_position = player_state_get_list_position();
    size_t list_display_position = list_count > 0U ? list_position + 1U : 0U;
    if (player_control_get_folder_scope() != PlayerFolderScope::All) {
        PlayerFolderQueueSnapshot folder_queue = {};
        if (player_state_get_folder_queue_snapshot(&folder_queue) && !folder_queue.current_in_queue) {
            list_display_position = 0U;
        }
    }
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
    player_home_label_set_text_if_changed(g_title, title);

    const char *artist = have_view && view.artist != nullptr && view.artist[0] != '\0'
        ? view.artist
        : "未知歌手";
    player_home_label_set_text_if_changed(g_artist, artist);

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
    char track_info[128] = {};
    if (sample_info[0] != '\0') {
        snprintf(
            track_info,
            sizeof(track_info),
            "%u / %u  ·  %s  ·  %s",
            static_cast<unsigned>(list_display_position),
            static_cast<unsigned>(list_count),
            media_format_name(player_state_get_format()),
            sample_info);
    } else {
        snprintf(
            track_info,
            sizeof(track_info),
            "%u / %u  ·  %s",
            static_cast<unsigned>(list_display_position),
            static_cast<unsigned>(list_count),
            media_format_name(player_state_get_format()));
    }
    player_home_label_set_text_if_changed(g_track_info, track_info);
}

static void player_home_refresh_transport_controls(const AudioStateSnapshot *snapshot)
{
    const PlayerLoopMode loop_mode = player_control_get_loop_mode();
    if (!g_last_loop_mode_valid || g_last_loop_mode != loop_mode) {
        g_last_loop_mode = loop_mode;
        g_last_loop_mode_valid = true;
        if (g_loop_button != nullptr) {
            // 模式真正变化时才重绘这个小对象。
            lv_obj_invalidate(g_loop_button);
        }
    }

    if (snapshot != nullptr && !g_volume_dragging) {
        player_home_slider_set_value_if_changed(g_volume_slider, snapshot->volume_percent);

        if (snapshot->user_muted) {
            player_home_label_set_text_if_changed(g_volume_label, "静音");
        } else {
            char volume[16] = {};
            snprintf(
                volume,
                sizeof(volume),
                "%u%%",
                static_cast<unsigned>(snapshot->volume_percent));
            player_home_label_set_text_if_changed(g_volume_label, volume);
        }
    }
}

static void player_home_apply_audio_snapshot(const AudioStateSnapshot &snapshot)
{
    const bool pause_icon = snapshot.state == AudioPlaybackState::Playing;
    player_home_label_set_text_if_changed(
        g_play_symbol,
        pause_icon ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
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
    // 屏幕动作菜单是半透明悬浮层；菜单显示期间底层主页必须保持最后一帧，
    // 避免封面/磁带在胶囊菜单背后继续刷新。
    if (screen_action_menu_is_open()) {
        return;
    }
    if (ui_touch_input_recent_activity(kInteractionYieldMs)) {
        return;
    }
    if (g_cassette_visual_switch_pending) {
        // 切换准备期间 Artwork 继续留在屏幕上；这里只推进隐藏的 Cassette 状态机。
        cassette_view_update();
        if (player_home_finish_cassette_switch_if_ready()) {
            return;
        }
    }

    if (g_music_visual_mode == MusicVisualMode::Cassette) {
        cassette_view_update();
        if (g_overlay_visible) {
            // 新歌缓存可能在本次后台刷新刚刚准备完成，立即撤掉实时 Alpha Backdrop。
            player_home_overlay_apply_dim_path();
        }
    } else {
        now_playing_artwork_update();
        player_home_repaint_controls_after_bounded_present();
        // 如果 Overlay 打开期间新 Surface 刚好准备完成，立即切到该曲的 dimmed RGB565。
        if (g_overlay_visible) {
            player_home_overlay_apply_dim_path();
        }
    }
}

static void player_home_audio_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    // 动作菜单覆盖主页时，底层进度/标题也停止刷新；音频服务本身继续运行。
    if (screen_action_menu_is_open()) {
        return;
    }
    if (ui_touch_input_recent_activity(kInteractionYieldMs)) {
        return;
    }
    // BatteryService 每2秒才更新一次 sequence；100ms Home timer 只做轻量快照比较，
    // 不会重复重绘电池胶囊。它必须独立于 Audio state_revision，否则暂停时电量不会更新。
    player_home_refresh_battery_status();

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
    HOME_INTERACTION_LOGI("控件命中：上一曲");
    const size_t before_track = player_state_get_index();
    if (player_control_previous()) {
        if (player_state_get_index() != before_track) {
            // R.36：真正跨 Track 后立即刷新封面 context；新 Surface 未完成时继续保持旧 GRAM。
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
    HOME_INTERACTION_LOGI("控件命中：下一曲");
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
    HOME_INTERACTION_LOGI("控件命中：播放/暂停");
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
    HOME_INTERACTION_LOGI("控件命中：播放模式");
    player_control_cycle_loop_mode();
    AudioStateSnapshot snapshot = {};
    audio_service_get_snapshot(&snapshot);
    player_home_refresh_transport_controls(&snapshot);
}

static void player_home_refresh_visual_mode_button()
{
    if (g_visual_mode_label == nullptr) return;
    player_home_label_set_text_if_changed(
        g_visual_mode_label,
        g_music_visual_mode == MusicVisualMode::Cassette ? "切换到封面" : "切换到磁带");
}

static bool player_home_finish_cassette_switch_if_ready()
{
    if (!g_cassette_visual_switch_pending) return false;
    if (!cassette_view_is_present_ready() || !cassette_view_try_present_deferred()) {
        return false;
    }

    // Cassette root 到这里才真正显示；同一个 LVGL 回调内随后关闭 Artwork，屏幕不会经过粉色中间帧。
    g_cassette_visual_switch_pending = false;
    g_music_visual_mode = MusicVisualMode::Cassette;
    now_playing_artwork_set_bounded_present_allowed(false);
    (void)now_playing_artwork_set_dimmed(false);
    now_playing_artwork_set_active(false);
    g_artwork_resume_without_invalidation = false;

    player_home_refresh_visual_mode_button();
    player_home_overlay_apply_dim_path();
    lv_obj_t *screen = lv_screen_active();
    if (screen != nullptr) lv_obj_invalidate(screen);
    ESP_LOGI(TAG, "Music视觉模式：磁带（封面+壳体已同步就绪）");
    return true;
}

static bool player_home_set_visual_mode(MusicVisualMode mode)
{
    if (mode == MusicVisualMode::Artwork && g_cassette_visual_switch_pending) {
        g_cassette_visual_switch_pending = false;
        (void)cassette_view_set_active(false);
    }

    if (mode == g_music_visual_mode) {
        player_home_refresh_visual_mode_button();
        return true;
    }

    if (mode == MusicVisualMode::Cassette) {
        if (g_cassette_visual_switch_pending) return true;

        // 不再先显示粉色 Cassette 再关闭 Artwork。磁带层保持隐藏，后台准备当前封面和壳体颜色；
        // ready 后由 player_home_finish_cassette_switch_if_ready() 在同一帧完成视觉交接。
        if (!cassette_view_prepare_deferred_active()) {
            ESP_LOGW(TAG, "切换磁带模式失败：磁带视觉层未就绪");
            return false;
        }
        cassette_view_set_controls_visible(g_overlay_visible);
        g_cassette_visual_switch_pending = true;
        if (!player_home_finish_cassette_switch_if_ready()) {
            ESP_LOGI(TAG, "Music视觉模式：磁带准备中，继续保持当前封面");
        }
        return true;
    }

    g_music_visual_mode = MusicVisualMode::Artwork;
    (void)cassette_view_set_active(false);
    const bool should_run =
        g_app_foreground &&
        !library_view_is_visible() &&
        !lyrics_view_is_visible() &&
        !spectrum_view_is_visible() &&
        !g_launcher_visible;
    now_playing_artwork_set_bounded_present_allowed(should_run);
    (void)now_playing_artwork_set_dimmed(g_overlay_visible);
    now_playing_artwork_set_active(should_run);
    if (should_run) {
        now_playing_artwork_refresh_context();
        now_playing_artwork_update();
        player_home_repaint_controls_after_bounded_present();
    }

    player_home_refresh_visual_mode_button();
    player_home_overlay_apply_dim_path();
    lv_obj_t *screen = lv_screen_active();
    if (screen != nullptr) lv_obj_invalidate(screen);
    ESP_LOGI(TAG, "Music视觉模式：封面");
    return true;
}

static void player_home_visual_mode_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !g_overlay_visible ||
        player_home_click_suppressed()) {
        return;
    }
    player_home_overlay_show();
    const MusicVisualMode next = g_music_visual_mode == MusicVisualMode::Artwork
        ? MusicVisualMode::Cassette
        : MusicVisualMode::Artwork;
    (void)player_home_set_visual_mode(next);
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
    HOME_INTERACTION_LOGI("音量手势：%s", armed ? "已进入纵向调节" : "已退出纵向调节");
}

static void player_home_mute_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || player_home_click_suppressed()) {
        return;
    }
    player_home_overlay_show();
    HOME_INTERACTION_LOGI("控件命中：静音");
    if (!player_control_toggle_mute()) {
        ESP_LOGW(TAG, "静音切换请求未能入队");
    }
}

static void player_home_refresh()
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
    if (g_music_visual_mode == MusicVisualMode::Cassette) {
        cassette_view_update();
    } else {
        now_playing_artwork_refresh_context();
        now_playing_artwork_update();
        player_home_repaint_controls_after_bounded_present();
    }
    if (player_state_is_ready() && media_library_get_count() > 0U) {
        g_last_artwork_bound_track = static_cast<uint32_t>(player_state_get_index());
    }
    if (g_overlay_visible) {
        player_home_overlay_apply_dim_path();
    }
    g_last_audio_state_revision = snapshot.state_revision;
}

bool player_home_prepare_track_transition_hold()
{
    if (g_music_visual_mode == MusicVisualMode::Cassette) {
        return cassette_view_prepare_track_transition_hold();
    }
    return now_playing_artwork_prepare_track_transition_hold();
}

void player_home_cancel_track_transition_hold()
{
    if (g_music_visual_mode == MusicVisualMode::Cassette) {
        cassette_view_cancel_track_transition_hold();
    } else {
        now_playing_artwork_cancel_track_transition_hold();
    }
}

void player_home_resume_from_fullscreen_view(const char *reason)
{
    // 页面调用方必须先隐藏自己的全屏 root；这样 QoS 才会把主页 Artwork/timer 恢复为 active。
    player_home_refresh();

    // 页面恢复固定交给 LVGL 官方 flush 链；大面积高速提交只由 BoundedSPI 路径负责。
    lv_obj_t *screen = lv_screen_active();
    if (screen != nullptr) {
        lv_obj_invalidate(screen);
    }
    HOME_INTERACTION_LOGI(
        "HomeResume：reason=%s artwork=%u overlay=%u",
        reason != nullptr ? reason : "unknown",
        now_playing_artwork_has_fast_surface() ? 1U : 0U,
        g_overlay_visible ? 1U : 0U);
}

static void player_home_launcher_abort_for_app_switch()
{
    if (g_launcher_panel != nullptr) {
        lv_anim_delete(g_launcher_panel, player_home_launcher_progress_anim_exec);
    }
    if (g_launcher_bounded_session_active) {
        display_launcher_bounded_spi_session_end();
    }
    g_launcher_bounded_session_active = false;
    player_home_launcher_release_surface_lease();
    g_launcher_frame_cache_active = false;
    g_launcher_frame_index = kLauncherFrameInvalid;
    if (g_music_visual_mode == MusicVisualMode::Cassette) {
        // 直接切换到其他 APP 时不要恢复一帧磁带动画；先保持 Cassette inactive，再清冻结标记。
        (void)cassette_view_set_active(false);
        cassette_view_set_launcher_suspended(false);
    }
    g_launcher_cassette_scene_hidden = false;
    g_launcher_visible = false;
    g_launcher_motion = LauncherMotionState::Hidden;
    g_launcher_anim_progress = 0;
    g_artwork_resume_without_invalidation = false;
    if (g_launcher != nullptr) {
        lv_obj_add_flag(g_launcher, LV_OBJ_FLAG_HIDDEN);
    }
    now_playing_artwork_set_bounded_present_allowed(false);
}

esp_err_t player_home_app_leave_background()
{
    if (!g_app_foreground) {
        return ESP_OK;
    }

    // Music 的子页面必须先静默收口，不能调用 HomeResume；否则会在 Manager 已准备切 APP 时
    // 重新 acquire Artwork lease / 恢复 timer。
    library_view_suspend_for_app_switch();
    lyrics_view_close();
    spectrum_view_close();
    player_home_launcher_abort_for_app_switch();
    player_home_overlay_hide();
    player_home_cancel_progress_interaction();
    g_volume_dragging = false;
    g_volume_adjust_armed = false;
    gesture_router_set_vertical_adjust_enabled(false);
    gesture_router_reset();

    g_app_foreground = false;
    if (g_gesture_timer != nullptr) {
        lv_timer_pause(g_gesture_timer);
    }
    if (g_overlay_timer != nullptr) {
        lv_timer_pause(g_overlay_timer);
    }
    player_home_update_background_timer_qos();

    ESP_LOGI(TAG, "Music前台已挂起：AudioTask继续运行，Home/Artwork/手势timer暂停");
    return ESP_OK;
}

esp_err_t player_home_app_enter_foreground()
{
    if (g_app_foreground) {
        if (g_launcher_open_after_foreground) {
            g_launcher_open_after_foreground = false;
            player_home_launcher_show();
        }
        return ESP_OK;
    }

    g_app_foreground = true;
    now_playing_artwork_set_bounded_present_allowed(
        g_music_visual_mode == MusicVisualMode::Artwork);
    if (g_gesture_timer != nullptr) {
        lv_timer_reset(g_gesture_timer);
        lv_timer_resume(g_gesture_timer);
    }

    // APP 返回 Music 固定落到 Home；歌词/频谱/曲库下次由用户重新打开。
    player_home_refresh();
    lv_obj_t *screen = lv_screen_active();
    if (screen != nullptr) {
        lv_obj_invalidate(screen);
    }

    const bool open_launcher = g_launcher_open_after_foreground;
    g_launcher_open_after_foreground = false;
    if (open_launcher) {
        player_home_launcher_show();
        ESP_LOGI(TAG, "Music恢复前台：复用原生Launcher自动展开");
    } else {
        ESP_LOGI(TAG, "Music恢复前台：Home/Artwork/手势timer恢复");
    }
    return ESP_OK;
}

bool player_home_app_is_foreground()
{
    return g_app_foreground;
}

esp_err_t player_home_request_launcher_foreground()
{
    if (!app_manager_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (app_manager_foreground() == AppId::Music && g_app_foreground) {
        player_home_launcher_show();
        return ESP_OK;
    }

    // AppManager 会同步调用 Music enter()，因此必须在切换前置位；失败则立即撤销。
    g_launcher_open_after_foreground = true;
    const esp_err_t ret = app_manager_request_foreground(
        AppId::Music, AppTransitionMode::Exclusive);
    if (ret != ESP_OK) {
        g_launcher_open_after_foreground = false;
    }
    return ret;
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
    g_music_visual_mode = MusicVisualMode::Artwork;
    g_cassette_visual_switch_pending = false;
    g_visual_mode_button = nullptr;
    g_visual_mode_label = nullptr;
    g_last_loop_mode_valid = false;
    g_overlay_backdrop = nullptr;
    g_launcher = nullptr;
    g_launcher_backdrop = nullptr;
    g_launcher_panel = nullptr;
    g_launcher_frame_cache_ready = false;
    g_launcher_frame_cache_active = false;
    player_home_launcher_release_surface_lease();
    g_launcher_surface_lease_us = 0U;
    g_launcher_frame_index = kLauncherFrameInvalid;
    g_launcher_frame_decode_count = 0U;
    g_launcher_frame_decode_us = 0U;
    g_launcher_frame_decode_max_us = 0U;
    g_launcher_bounded_session_active = false;
    player_home_launcher_reset_bounded_stats();
    g_launcher_visible = false;
    g_launcher_cassette_scene_hidden = false;
    g_launcher_motion = LauncherMotionState::Hidden;
    g_launcher_selected_index = 0U;
    g_launcher_anim_started_ms = 0U;
    g_launcher_anim_progress = 0;
    g_prev_button = nullptr;
    g_play_button = nullptr;
    g_next_button = nullptr;
    g_play_symbol = nullptr;
    g_play_icon_pause = false;
    g_volume_mode_button = nullptr;
    g_battery_status = nullptr;
    g_battery_label = nullptr;
    g_last_battery_sequence = UINT32_MAX;
    g_last_battery_percent = 0U;
    g_last_battery_valid = false;
    g_audio_timer = nullptr;
    g_artwork_timer = nullptr;
    g_gesture_timer = nullptr;
    g_background_timers_running = true;
    g_app_foreground = true;
    g_launcher_open_after_foreground = false;
    g_artwork_resume_without_invalidation = false;
    g_last_artwork_bound_track = UINT32_MAX;
    gesture_router_reset();
    now_playing_artwork_set_bounded_present_allowed(true);

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

    const esp_err_t cassette_ret = cassette_view_create(screen);
    if (cassette_ret != ESP_OK) {
        ESP_LOGW(TAG, "创建磁带视觉层失败，保留封面模式：%s", esp_err_to_name(cassette_ret));
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
    // 初始为纯封面；事件回调常驻，但只有 Overlay 显示时才打开 CLICKABLE。
    lv_obj_remove_flag(g_overlay_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        g_overlay_backdrop, player_home_overlay_backdrop_tap_cb, LV_EVENT_CLICKED, nullptr);

    // P1.5.3.2R.32：Launcher 根层、Backdrop、340x340 panel 三者都固定不动。
    // 进入/退出只驱动 panel DRAW_MAIN 的径向展开 progress，消除移动对象造成的旧/新位置双重失效。
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
    lv_obj_set_pos(g_launcher_panel, kLauncherPanelX, kLauncherPanelShownY);
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
        player_home_launcher_panel_click_cb,
        LV_EVENT_CLICKED,
        nullptr);

    // R.36.6：不再分配 340x340 RGB565 PanelWork/Canvas。Flash I4 + 当前 dimmed Surface
    // 直接按 SPI staging strip 现场合成；LVGL fallback 继续使用 R.32 panel DRAW_MAIN。
    g_launcher_frame_cache_ready = true;
    player_home_launcher_update_frame_lut();

    // R.33.1 fallback callback 常驻，但缓存激活时开头立即 return；这样某一首封面暂时拿不到
    // CoverSurface 时只回退这一次，不会永久关闭后续歌曲的纯 RGB565 快速动画路径。
    lv_obj_add_event_cb(
        g_launcher_panel,
        player_home_launcher_panel_draw_cb,
        LV_EVENT_DRAW_MAIN,
        nullptr);

    lv_obj_add_flag(g_launcher, LV_OBJ_FLAG_HIDDEN);
#if APP_DIAG_BOOT_VERBOSE
    uint32_t flash_bytes = 0U;
    for (uint8_t i = 0; i < kLauncherAnimationAssetFrameCount; ++i) {
        flash_bytes += g_launcher_animation_frames[i].size;
    }
    HOME_BOOT_LOGI(
        "Launcher：FlashI4=%uB FullBase=0B PanelWork=0B source=CoverSurface.dimmed producer=wire-order",
        static_cast<unsigned>(flash_bytes));
    HOME_BOOT_LOGI(
        "显示传输：BoundedSPI=%s Launcher=wire-strip Cover=bounded LVGL-fallback=enabled",
        display_launcher_bounded_spi_available() ? "ready" : "unavailable");
#endif

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

    // 视觉模式入口移到音量条下方，并扩大触摸面积。
    // 这样不占用四向手势，也避免原 76x28 小按钮难以命中。
    g_visual_mode_button = player_home_create_pill_button(
        g_overlay, 160, 42, "切换到磁带", &g_visual_mode_label);
    lv_obj_set_pos(g_visual_mode_button, 150, 404);
    lv_obj_set_style_bg_opa(g_visual_mode_button, 46, 0);
    // C2.4.7：视觉仍保持 160x42，但把实际命中区向四周扩 15px。
    // 最终约 190x72；向上覆盖到不可触摸的音量条下沿，按钮更容易命中，界面外观不变。
    lv_obj_set_ext_click_area(g_visual_mode_button, 15);
    lv_obj_add_event_cb(g_visual_mode_button, player_home_visual_mode_cb, LV_EVENT_CLICKED, nullptr);

    // 音量与电量使用等宽胶囊并排显示。音量左侧自绘小扬声器，右侧保留百分比/静音文本。
    // 音量胶囊扩为 94x28，并把实际点击范围再向四周额外扩大 8px，提升静音点击命中率；
    // 两个胶囊之间保留 8px 间距，扩展后的命中区不会覆盖电池区域。
    lv_obj_t *volume_status = player_home_create_pill_button(g_overlay, 94, 28, "80%", &g_volume_label);
    lv_obj_set_pos(volume_status, 132, 319);
    lv_obj_set_ext_click_area(volume_status, 8);
    lv_obj_add_event_cb(volume_status, player_home_volume_status_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_set_size(g_volume_label, 54, 28);
    lv_obj_set_style_text_align(g_volume_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_volume_label, LV_ALIGN_RIGHT_MID, -3, 0);
    lv_obj_add_event_cb(volume_status, player_home_mute_cb, LV_EVENT_CLICKED, nullptr);

    g_battery_status = player_home_create_battery_status(g_overlay);
    lv_obj_set_pos(g_battery_status, 234, 319);

    g_volume_mode_button = player_home_create_volume_mode_button(g_overlay);
    lv_obj_set_pos(g_volume_mode_button, 352, 346);
    lv_obj_add_event_cb(g_volume_mode_button, player_home_volume_mode_cb, LV_EVENT_CLICKED, nullptr);

    AudioStateSnapshot snapshot = {};
    audio_service_get_snapshot(&snapshot);
    player_home_apply_audio_snapshot(snapshot);
    player_home_refresh_battery_status();
    g_last_audio_state_revision = snapshot.state_revision;

    g_audio_timer = lv_timer_create(player_home_audio_timer_cb, 100, nullptr);
    g_artwork_timer = lv_timer_create(player_home_artwork_timer_cb, 100, nullptr);
    g_gesture_timer = lv_timer_create(player_home_gesture_timer_cb, 20, nullptr);
    g_overlay_timer = lv_timer_create(player_home_overlay_timeout_cb, kOverlayTimeoutMs, nullptr);
    if (g_overlay_timer != nullptr) {
        lv_timer_pause(g_overlay_timer);
    }

    // 开机/进入播放器先保持纯封面。R.34.1 不再隐藏 460x460 root，
    // 只隐藏真正有视觉内容的 child，避免以后显隐 root 时制造整屏 dirty。
    player_home_overlay_set_controls_visible(false);
    player_home_overlay_set_backdrop_clickable(false);

#if APP_DIAG_BOOT_VERBOSE
    char list_label[96] = {};
    if (!player_state_copy_list_label(list_label, sizeof(list_label))) {
        snprintf(list_label, sizeof(list_label), "未知列表");
    }
    const size_t boot_list_count = player_state_get_list_count();
    size_t boot_list_display_position = boot_list_count > 0U ? player_state_get_list_position() + 1U : 0U;
    if (player_control_get_folder_scope() != PlayerFolderScope::All) {
        PlayerFolderQueueSnapshot boot_folder_queue = {};
        if (player_state_get_folder_queue_snapshot(&boot_folder_queue) && !boot_folder_queue.current_in_queue) {
            boot_list_display_position = 0U;
        }
    }
    HOME_BOOT_LOGI(
        "主页配置：list=%s pos=%u/%u track=%u loop=%s volume=%u%% mute=%u Cover=normal+dimmed Launcher=wire-strip",
        list_label,
        static_cast<unsigned>(boot_list_display_position),
        static_cast<unsigned>(boot_list_count),
        static_cast<unsigned>(media_library_get_count() > 0 ? player_state_get_index() : 0),
        player_transport_loop_mode_name(player_control_get_loop_mode()),
        static_cast<unsigned>(snapshot.volume_percent),
        static_cast<unsigned>(snapshot.user_muted));
#endif
}


bool player_home_overlay_is_visible()
{
    return g_overlay_visible;
}

bool player_home_launcher_is_visible()
{
    return g_launcher_visible;
}

bool player_home_launcher_is_animating()
{
    return g_launcher_visible &&
        (g_launcher_motion == LauncherMotionState::Entering ||
         g_launcher_motion == LauncherMotionState::Leaving);
}

// -----------------------------------------------------------------------------
// R.39.5.6 Shared Launcher Visual Core
// 非 Music APP 只消费这里导出的像素结果/命中结果；视觉定义仍只有 player_home 一份。
// -----------------------------------------------------------------------------

uint8_t player_home_launcher_shared_frame_for_progress(int32_t progress)
{
    return player_home_launcher_frame_for_progress(progress);
}

int32_t player_home_launcher_shared_frame_progress(uint8_t frame_index)
{
    if (frame_index >= kLauncherAnimationAssetFrameCount) return 0;
    return g_launcher_animation_frames[frame_index].progress;
}

static int8_t player_home_launcher_index_for_app(AppId app)
{
    for (uint8_t i = 0U; i < kLauncherItemCount; ++i) {
        if (kLauncherItems[i].app_id == app) return static_cast<int8_t>(i);
    }
    return -1;
}

esp_err_t player_home_launcher_shared_render_i4(
    AppId selected,
    uint8_t frame_index,
    uint8_t *buffer,
    size_t buffer_size,
    lv_image_dsc_t *out_dsc)
{
    if (buffer == nullptr || out_dsc == nullptr ||
        buffer_size < PLAYER_HOME_LAUNCHER_I4_IMAGE_BYTES ||
        frame_index >= kLauncherAnimationAssetFrameCount) {
        return ESP_ERR_INVALID_ARG;
    }
    const int8_t selected_index = player_home_launcher_index_for_app(selected);
    if (selected_index < 0) return ESP_ERR_INVALID_ARG;

    // LVGL I4 数据前 64B 是 16 项 ARGB8888 palette；后面就是 4bpp packed pixels。
    lv_color32_t palette[16] = {};
    palette[0] = lv_color_to_32(lv_color_hex(0x000000), LV_OPA_TRANSP);
    for (uint8_t i = 0U; i < kLauncherItemCount; ++i) {
        const uint32_t rgb = i == static_cast<uint8_t>(selected_index)
            ? kLauncherSelectedSectorRgb
            : kLauncherItems[i].idle_rgb;
        palette[1U + i] = lv_color_to_32(lv_color_hex(rgb), LV_OPA_COVER);
        palette[8U + i] = lv_color_to_32(lv_color_hex(rgb), kLauncherSectorAaOpa);
    }
    palette[15U] = lv_color_to_32(lv_color_hex(0xF8FAFF), LV_OPA_COVER);
    memcpy(buffer, palette, sizeof(palette));

    uint8_t *pixels = buffer + sizeof(palette);
    const LauncherAnimationFrameAsset &asset = g_launcher_animation_frames[frame_index];
    const uint8_t *src = asset.data;
    const uint8_t *src_end = asset.data + asset.size;
    uint32_t written = 0U;
    while (src < src_end && written < kLauncherAnimationAssetPixelBytes) {
        const uint8_t control = *src++;
        uint32_t count = static_cast<uint32_t>(control & 0x7FU) + 1U;
        const bool repeat = (control & 0x80U) != 0U;
        if (repeat) {
            if (src >= src_end || written + count > kLauncherAnimationAssetPixelBytes) {
                return ESP_ERR_INVALID_SIZE;
            }
            memset(pixels + written, *src++, count);
            written += count;
        }
        else {
            if (count > static_cast<uint32_t>(src_end - src) ||
                written + count > kLauncherAnimationAssetPixelBytes) {
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(pixels + written, src, count);
            src += count;
            written += count;
        }
    }
    if (written != kLauncherAnimationAssetPixelBytes || src != src_end) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_dsc = {};
    out_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    out_dsc->header.cf = LV_COLOR_FORMAT_I4;
    out_dsc->header.flags = 0U;
    out_dsc->header.w = kLauncherPanelSize;
    out_dsc->header.h = kLauncherPanelSize;
    out_dsc->header.stride = kLauncherAnimationAssetStride;
    out_dsc->data_size = PLAYER_HOME_LAUNCHER_I4_IMAGE_BYTES;
    out_dsc->data = buffer;
    return ESP_OK;
}

struct SharedLauncherI2Target {
    uint8_t *pixels = nullptr;
    int32_t size = 0;
    int32_t stride = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
};

static void player_home_launcher_shared_i2_pixel(
    const SharedLauncherI2Target &target,
    int32_t x,
    int32_t y,
    uint8_t index)
{
    const int32_t local_x = x - target.origin_x;
    const int32_t local_y = y - target.origin_y;
    if (target.pixels == nullptr || local_x < 0 || local_y < 0 ||
        local_x >= target.size || local_y >= target.size || index > 3U) return;
    uint8_t &packed = target.pixels[static_cast<size_t>(local_y) * target.stride + local_x / 4];
    const uint8_t shift = static_cast<uint8_t>(6 - (local_x & 3) * 2);
    packed = static_cast<uint8_t>((packed & ~(0x03U << shift)) | ((index & 0x03U) << shift));
}

static void player_home_launcher_shared_i2_disk(
    const SharedLauncherI2Target &target,
    int32_t cx, int32_t cy, int32_t diameter, uint8_t index)
{
    if (diameter <= 0) return;
    const int32_t radius = diameter / 2;
    const int32_t radius_sq = radius * radius;
    for (int32_t y = cy - radius; y <= cy + radius; ++y) {
        const int32_t dy = y - cy;
        for (int32_t x = cx - radius; x <= cx + radius; ++x) {
            const int32_t dx = x - cx;
            if (dx * dx + dy * dy <= radius_sq) {
                player_home_launcher_shared_i2_pixel(target, x, y, index);
            }
        }
    }
}

static void player_home_launcher_shared_i2_line(
    const SharedLauncherI2Target &target,
    int32_t x0, int32_t y0, int32_t x1, int32_t y1,
    int32_t width, uint8_t index)
{
    int32_t dx = abs(x1 - x0);
    const int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy = -abs(y1 - y0);
    const int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    const int32_t diameter = width < 2 ? 2 : width;
    while (true) {
        player_home_launcher_shared_i2_disk(target, x0, y0, diameter, index);
        if (x0 == x1 && y0 == y1) break;
        const int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void player_home_launcher_shared_i2_icon(
    const SharedLauncherI2Target &target,
    LauncherIconKind icon,
    int32_t cx,
    int32_t cy,
    int32_t scale_percent,
    uint8_t index)
{
    auto scale = [scale_percent](int32_t value) -> int32_t {
        const int32_t scaled = (value * scale_percent + (value >= 0 ? 50 : -50)) / 100;
        if (value != 0 && scaled == 0) return value > 0 ? 1 : -1;
        return scaled;
    };
    const int32_t line_width = scale_percent >= 120 ? 4 : (scale_percent >= 70 ? 3 : 2);
    auto line = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        player_home_launcher_shared_i2_line(
            target, cx + scale(x0), cy + scale(y0), cx + scale(x1), cy + scale(y1),
            line_width, index);
    };
    auto dot = [&](int32_t x, int32_t y, int32_t diameter) {
        const int32_t d = scale(diameter) < 3 ? 3 : scale(diameter);
        player_home_launcher_shared_i2_disk(
            target, cx + scale(x), cy + scale(y), d, index);
    };

    switch (icon) {
        case LauncherIconKind::Music:
            line(-4,-16,-4,7); line(-4,-16,15,-20); line(15,-20,15,2); dot(-11,10,10); dot(8,4,10); break;
        case LauncherIconKind::Nsf:
            line(-13,-13,13,-13); line(13,-13,13,13); line(13,13,-13,13); line(-13,13,-13,-13);
            line(-8,-18,-8,-13); line(0,-18,0,-13); line(8,-18,8,-13); line(-8,13,-8,18); line(0,13,0,18); line(8,13,8,18);
            line(-18,-8,-13,-8); line(-18,0,-13,0); line(-18,8,-13,8); line(13,-8,18,-8); line(13,0,18,0); line(13,8,18,8);
            line(-7,4,-2,-4); line(-2,-4,3,4); line(3,4,8,-4); break;
        case LauncherIconKind::MicSpectrum:
            line(-11,-14,-11,6); line(-11,-14,-4,-18); line(-4,-18,3,-14); line(3,-14,3,6); line(3,6,-4,10); line(-4,10,-11,6);
            line(-15,4,-15,7); line(-15,7,-9,13); line(-9,13,-4,14); line(-4,14,2,12); line(-4,14,-4,19); line(-10,19,2,19);
            line(8,10,8,17); line(13,4,13,17); line(18,-3,18,17); break;
        case LauncherIconKind::Mjpg:
            line(-18,-13,11,-13); line(11,-13,11,13); line(11,13,-18,13); line(-18,13,-18,-13); line(11,-7,18,-12); line(18,-12,18,12); line(18,12,11,7);
            line(-6,-7,-6,7); line(-6,-7,5,0); line(5,0,-6,7); break;
        case LauncherIconKind::Picture:
            line(-18,-15,18,-15); line(18,-15,18,15); line(18,15,-18,15); line(-18,15,-18,-15); line(-14,10,-5,0); line(-5,0,1,6); line(1,6,8,-4); line(8,-4,15,10); dot(9,-9,6); break;
        case LauncherIconKind::Ebook:
            line(0,-14,0,15); line(-1,-12,-7,-15); line(-7,-15,-18,-12); line(-18,-12,-18,12); line(-18,12,-7,10); line(-7,10,-1,13);
            line(1,-12,7,-15); line(7,-15,18,-12); line(18,-12,18,12); line(18,12,7,10); line(7,10,1,13); break;
        case LauncherIconKind::Settings:
            dot(0,0,10); line(0,-18,0,-11); line(0,11,0,18); line(-18,0,-11,0); line(11,0,18,0);
            line(-13,-13,-8,-8); line(8,8,13,13); line(13,-13,8,-8); line(-8,8,-13,13); break;
    }
}

esp_err_t player_home_launcher_shared_render_center_i2(
    AppId selected,
    int32_t progress,
    uint8_t *buffer,
    size_t buffer_size,
    lv_image_dsc_t *out_dsc)
{
    if (buffer == nullptr || out_dsc == nullptr ||
        buffer_size < PLAYER_HOME_LAUNCHER_CENTER_IMAGE_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    const int8_t selected_index = player_home_launcher_index_for_app(selected);
    if (selected_index < 0) return ESP_ERR_INVALID_ARG;
    progress = std::max<int32_t>(0, std::min<int32_t>(progress, kLauncherAnimProgressMax));

    lv_color32_t palette[4] = {};
    palette[0] = lv_color_to_32(lv_color_hex(0x000000), LV_OPA_TRANSP);
    palette[1] = lv_color_to_32(lv_color_hex(0x05070B), static_cast<uint8_t>(245U));
    palette[2] = lv_color_to_32(lv_color_hex(0x161B27), LV_OPA_COVER);
    palette[3] = lv_color_to_32(lv_color_hex(kLauncherAccentRgb), LV_OPA_COVER);
    memcpy(buffer, palette, sizeof(palette));
    memset(buffer + sizeof(palette), 0,
        PLAYER_HOME_LAUNCHER_CENTER_IMAGE_BYTES - sizeof(palette));

    constexpr int32_t image_size = PLAYER_HOME_LAUNCHER_CENTER_IMAGE_SIZE;
    constexpr int32_t origin = kLauncherCenterX - image_size / 2;
    SharedLauncherI2Target target = {
        buffer + sizeof(palette), image_size,
        PLAYER_HOME_LAUNCHER_CENTER_I2_STRIDE, origin, origin};

    if (progress > 0) {
        auto lerp = [progress](int32_t from, int32_t to) -> int32_t {
            return from + ((to - from) * progress + kLauncherAnimProgressMax / 2) /
                kLauncherAnimProgressMax;
        };
        const int32_t diameter = lerp(kLauncherCollapsedCenterDiameter, kLauncherCenterDiameter);
        const int32_t radius = diameter / 2;
        const int32_t border_width = progress >= 500 ? 2 : 1;
        const int32_t inner_radius = radius - border_width;
        const int32_t radius_sq = radius * radius;
        const int32_t inner_sq = inner_radius * inner_radius;
        for (int32_t y = kLauncherCenterY - radius; y <= kLauncherCenterY + radius; ++y) {
            const int32_t dy = y - kLauncherCenterY;
            for (int32_t x = kLauncherCenterX - radius; x <= kLauncherCenterX + radius; ++x) {
                const int32_t dx = x - kLauncherCenterX;
                const int32_t d2 = dx * dx + dy * dy;
                if (d2 > radius_sq) continue;
                player_home_launcher_shared_i2_pixel(target, x, y, d2 >= inner_sq ? 2U : 1U);
            }
        }
        const LauncherMenuItemDef &item = kLauncherItems[static_cast<uint8_t>(selected_index)];
        const int32_t icon_scale = 78 + (54 * progress) / kLauncherAnimProgressMax;
        player_home_launcher_shared_i2_icon(
            target, item.icon,
            kLauncherCenterX + (item.optical_x * progress) / kLauncherAnimProgressMax,
            kLauncherCenterY + (item.optical_y * progress) / kLauncherAnimProgressMax,
            icon_scale, 3U);
    }

    *out_dsc = {};
    out_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    out_dsc->header.cf = LV_COLOR_FORMAT_I2;
    out_dsc->header.flags = 0U;
    out_dsc->header.w = image_size;
    out_dsc->header.h = image_size;
    out_dsc->header.stride = PLAYER_HOME_LAUNCHER_CENTER_I2_STRIDE;
    out_dsc->data_size = PLAYER_HOME_LAUNCHER_CENTER_IMAGE_BYTES;
    out_dsc->data = buffer;
    return ESP_OK;
}

int8_t player_home_launcher_shared_hit_test(
    int32_t screen_x, int32_t screen_y, int32_t panel_x, int32_t panel_y)
{
    // 非 Music APP 只共享纯几何命中；panel 原点由调用方自己的 LVGL 对象提供。
    // 这样共享算法既不依赖 Music 私有生命周期，也不假设父对象永远位于 (0,0)。
    return player_home_launcher_hit_test_geometry(
        screen_x, screen_y, panel_x, panel_y);
}

AppId player_home_launcher_shared_app_for_index(uint8_t index)
{
    return index < kLauncherItemCount ? kLauncherItems[index].app_id : AppId::None;
}

AppId player_home_launcher_shared_select_index(uint8_t index, bool *out_should_launch)
{
    if (out_should_launch != nullptr) *out_should_launch = false;
    const AppId target = player_home_launcher_shared_app_for_index(index);
    if (target == AppId::None) return AppId::None;

    // 选中与启动彻底分离：未实现 APP 也能选中，只是不发起 AppManager 跳转。
    app_manager_set_launcher_target(target);
    if (out_should_launch != nullptr) {
        *out_should_launch = target != app_manager_foreground() &&
            app_manager_is_registered(target);
    }
    return target;
}
