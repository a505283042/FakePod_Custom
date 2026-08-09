#include "player_home.h"

#include <stdio.h>

#include "esp_log.h"
#include "audio_service.h"
#include "artwork/now_playing_artwork.h"
#include "board_pins.h"
#include "font/font_manager.h"
#include "gesture/gesture_router.h"
#include "input/touch_input.h"
#include "media_library.h"
#include "player_control.h"
#include "player_state.h"
#include "library_view.h"
#include "lyrics/lyrics_view.h"
#include "spectrum/spectrum_view.h"
#include "ui_common.h"

static const char *TAG = "首页";

// 播放器默认只显示全屏封面。封面后台只预处理 normal RGB565；单击后显示控制 Overlay，
// Overlay 使用固定半透明黑层增强文字/控件可读性，不再生成第二张 dimmed Surface。
// AudioTask / Player Transport 仍沿用既有实现。
static constexpr int32_t kProgressScale = 10000;
static constexpr uint32_t kOverlayTimeoutMs = 5000U;
// P1.3.5.4.5：保持全屏半透明黑层；Overlay 任意位置纵向拖动改为音量预览，松手只提交一次。
// 仍只在 Overlay 显示期间启用，隐藏后立即回到原始全屏封面。
static constexpr lv_opa_t kOverlayDimOpacity = 150U;
static constexpr uint32_t kGestureHintTimeoutMs = 900U;
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

static lv_obj_t *g_overlay = nullptr;
static lv_obj_t *g_overlay_backdrop = nullptr;
static lv_timer_t *g_overlay_timer = nullptr;
static bool g_overlay_visible = false;
static bool g_overlay_fast_dim = false;
static bool g_overlay_dim_path_valid = false;

static lv_obj_t *g_gesture_hint = nullptr;
static lv_obj_t *g_gesture_hint_label = nullptr;
static lv_timer_t *g_gesture_hint_timer = nullptr;
static lv_timer_t *g_audio_timer = nullptr;
static lv_timer_t *g_artwork_timer = nullptr;
static bool g_background_timers_running = true;

static lv_obj_t *g_title = nullptr;
static lv_obj_t *g_track_info = nullptr;
static lv_obj_t *g_play_symbol = nullptr;
static lv_obj_t *g_progress = nullptr;
static lv_obj_t *g_current_time = nullptr;
static lv_obj_t *g_total_time = nullptr;
static lv_obj_t *g_loop_label = nullptr;
static lv_obj_t *g_volume_label = nullptr;
static lv_obj_t *g_volume_slider = nullptr;

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
static uint8_t g_volume_drag_start_percent = 0U;
static uint8_t g_volume_preview_percent = 0U;
static uint32_t g_volume_drag_sequence = 0U;
static uint32_t g_last_audio_state_revision = UINT32_MAX;

// Overlay 纵向音量手势定义在进度同步函数之前，先声明这两个内部 helper。
static void player_home_cancel_progress_interaction();
static void player_home_progress_sync(const AudioStateSnapshot &snapshot);

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

    // P1.3.5.4.4：恢复统一的全屏半透明黑层。CoverSurface 仍只保留 normal RGB565，
    // now_playing_artwork_set_dimmed() 返回 false 时，由这个 backdrop 直接做固定 alpha 暗化。
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

    if (!g_overlay_visible) {
        g_overlay_visible = true;
        g_overlay_dim_path_valid = false;
        player_home_overlay_apply_dim_path();
        lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    gesture_router_set_vertical_adjust_enabled(true);
    player_home_overlay_arm_timeout();
}

static void player_home_overlay_hide()
{
    if (g_overlay == nullptr || !g_overlay_visible) {
        return;
    }
    g_overlay_visible = false;
    gesture_router_set_vertical_adjust_enabled(false);
    g_volume_dragging = false;
    if (g_overlay_timer != nullptr) {
        lv_timer_pause(g_overlay_timer);
    }
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    // 保留接口调用，P1.2.5 下底图始终是 normal RGB565。
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

static void player_home_gesture_hint_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_gesture_hint != nullptr) {
        lv_obj_add_flag(g_gesture_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_gesture_hint_timer != nullptr) {
        lv_timer_pause(g_gesture_hint_timer);
    }
}

static void player_home_show_gesture_hint(const char *text)
{
    if (g_gesture_hint == nullptr || g_gesture_hint_label == nullptr || text == nullptr) {
        return;
    }
    lv_label_set_text(g_gesture_hint_label, text);
    lv_obj_remove_flag(g_gesture_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_gesture_hint);
    if (g_gesture_hint_timer != nullptr) {
        lv_timer_set_period(g_gesture_hint_timer, kGestureHintTimeoutMs);
        lv_timer_reset(g_gesture_hint_timer);
        lv_timer_resume(g_gesture_hint_timer);
    }
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
    if (!g_overlay_visible) {
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
        if (lyrics_view_overlay_is_visible()) {
            ESP_LOGI(TAG, "歌词Overlay 已锁定页面手势：忽略 %s", gesture_router_action_name(lyrics_action));
            return;
        }
        ESP_LOGI(TAG, "P1.4.3.2 歌词页手势命中：%s", gesture_router_action_name(lyrics_action));
        switch (lyrics_action) {
            case UiGestureAction::SwipeLeft:
                lyrics_view_close();
                player_home_refresh();
                break;
            case UiGestureAction::PullDownFromTop:
                // P1.4.3.2：歌词页不再下拉进入曲库。曲库入口只保留在封面主页，
                // 避免“歌词 -> 曲库 -> 返回主页”破坏当前页面上下文。
                ESP_LOGI(TAG, "歌词页顶部下拉已禁用：请返回封面主页后进入曲库");
                break;
            default:
                break;
        }
        return;
    }

    // P1.5.1：频谱页复用同一个 GestureRouter，但第一版只验证横向返回和持续刷新负载。
    // 与歌词页一致，曲库入口仍只保留在封面主页；频谱页不开放顶部下拉。
    if (spectrum_view_is_visible()) {
        UiGestureAction spectrum_action = UiGestureAction::None;
        if (!gesture_router_take_action(&spectrum_action)) {
            return;
        }
        ESP_LOGI(TAG, "P1.5R.1.2 频谱页手势命中：%s", gesture_router_action_name(spectrum_action));
        switch (spectrum_action) {
            case UiGestureAction::SwipeRight:
                spectrum_view_close();
                player_home_refresh();
                break;
            case UiGestureAction::PullDownFromTop:
                ESP_LOGI(TAG, "频谱页顶部下拉已禁用：请返回封面主页后进入曲库");
                break;
            default:
                break;
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

    // Overlay 是控制层：显示期间横向手势归控件层所有，不允许切换到歌词/频谱页。
    // 纯封面态才允许 SwipeLeft / SwipeRight 做播放页横向导航。
    if (g_overlay_visible &&
        (action == UiGestureAction::SwipeLeft || action == UiGestureAction::SwipeRight)) {
        ESP_LOGI(TAG, "Overlay 已锁定横滑：忽略 %s", gesture_router_action_name(action));
        player_home_overlay_arm_timeout();
        return;
    }

    ESP_LOGI(TAG, "P1.3 手势命中：%s", gesture_router_action_name(action));
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
            player_home_show_gesture_hint("Launcher · P1.7");
            break;
        default:
            break;
    }
}

static void player_home_screen_tap_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || player_home_click_suppressed()) {
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
        player_home_click_suppressed()) {
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

static const char *player_home_state_name(const AudioStateSnapshot *snapshot)
{
    if (snapshot == nullptr) {
        return "";
    }
    switch (snapshot->state) {
        case AudioPlaybackState::Playing: return "播放中";
        case AudioPlaybackState::Paused: return "已暂停";
        case AudioPlaybackState::Seeking: return "跳转中";
        case AudioPlaybackState::Finished: return "播放结束";
        case AudioPlaybackState::Error: return "播放错误";
        default: return "";
    }
}

static void player_home_refresh_track(const AudioStateSnapshot *audio_snapshot)
{
    if (g_title == nullptr || g_track_info == nullptr) {
        return;
    }

    const size_t library_count = media_library_get_count();
    const size_t list_count = player_state_get_list_count();
    if (library_count == 0U || list_count == 0U) {
        lv_label_set_text(g_title, "暂无歌曲");
        lv_label_set_text(g_track_info, "音乐库为空");
        return;
    }

    const size_t index = player_state_get_index();
    const size_t list_position = player_state_get_list_position();
    char title[512] = {};
    if (!media_library_copy_display_name(index, title, sizeof(title))) {
        snprintf(title, sizeof(title), "歌曲 %u", static_cast<unsigned>(index + 1U));
    }
    lv_label_set_text(g_title, title);

    const char *state_name = "";
    if (audio_snapshot != nullptr && audio_snapshot->track_index == index) {
        state_name = player_home_state_name(audio_snapshot);
    }

    if (state_name[0] != '\0') {
        lv_label_set_text_fmt(
            g_track_info,
            "%u / %u  ·  %s  ·  %s",
            static_cast<unsigned>(list_position + 1U),
            static_cast<unsigned>(list_count),
            media_format_name(player_state_get_format()),
            state_name);
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
    if (g_loop_label != nullptr) {
        lv_label_set_text(g_loop_label,
            player_transport_loop_mode_name(player_control_get_loop_mode()));
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
    if (g_play_symbol != nullptr) {
        lv_label_set_text(
            g_play_symbol,
            snapshot.state == AudioPlaybackState::Playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
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
    // 如果 Overlay 打开期间后台 surface 刚好准备完成，保持 alpha 黑层，不再切换第二张 Surface。
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
    if (player_control_previous()) {
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
    if (player_control_next()) {
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
    ESP_LOGI(TAG, "P1.2 控件命中：循环模式");
    player_control_cycle_loop_mode();
    AudioStateSnapshot snapshot = {};
    audio_service_get_snapshot(&snapshot);
    player_home_refresh_transport_controls(&snapshot);
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
    g_overlay_visible = false;
    g_overlay_fast_dim = false;
    g_overlay_dim_path_valid = false;
    g_overlay_backdrop = nullptr;
    g_gesture_hint = nullptr;
    g_gesture_hint_label = nullptr;
    g_gesture_hint_timer = nullptr;
    g_audio_timer = nullptr;
    g_artwork_timer = nullptr;
    g_background_timers_running = true;
    gesture_router_reset();

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

    // P1.3 首版只验证路由，不提前创建歌词/频谱/Launcher 页面。
    // 未落地的方向用一个短暂提示确认手势命中；顶部下拉已经直接复用现有曲库页。
    g_gesture_hint = lv_obj_create(screen);
    ui_common_lock_object(g_gesture_hint);
    lv_obj_set_size(g_gesture_hint, 250, 52);
    lv_obj_align(g_gesture_hint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(g_gesture_hint, 26, 0);
    lv_obj_set_style_bg_color(g_gesture_hint, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_gesture_hint, 205, 0);
    lv_obj_set_style_border_width(g_gesture_hint, 0, 0);
    lv_obj_set_style_shadow_width(g_gesture_hint, 0, 0);
    lv_obj_set_style_pad_all(g_gesture_hint, 0, 0);
    lv_obj_remove_flag(g_gesture_hint, LV_OBJ_FLAG_CLICKABLE);
    g_gesture_hint_label = player_home_create_label(
        g_gesture_hint, "", lv_color_hex(0xFFFFFF), font_manager_get_ui_font());
    lv_obj_center(g_gesture_hint_label);
    lv_obj_add_flag(g_gesture_hint, LV_OBJ_FLAG_HIDDEN);

    // 顶部只保留轻量状态：左侧循环模式；右侧区域预留给后续 BatteryService。
    lv_obj_t *loop = player_home_create_pill_button(g_overlay, 94, 36, "顺序", &g_loop_label);
    lv_obj_align(loop, LV_ALIGN_TOP_LEFT, 104, 40);
    lv_obj_add_event_cb(loop, player_home_loop_cb, LV_EVENT_CLICKED, nullptr);

    g_title = player_home_create_label(
        g_overlay, "", lv_color_hex(0xFFFFFF), font_manager_get_ui_font());
    lv_label_set_long_mode(g_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_title, 350, 34);
    lv_obj_set_style_text_align(g_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 92);

    g_track_info = player_home_create_label(
        g_overlay, "", lv_color_hex(0xD2D6DC), font_manager_get_ui_font());
    lv_label_set_long_mode(g_track_info, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_track_info, 360, 30);
    lv_obj_set_style_text_align(g_track_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_track_info, LV_ALIGN_TOP_MID, 0, 127);

    // 中部进度区继续复用 Stage 11.2 Seek 语义。
    g_progress = lv_slider_create(g_overlay);
    ui_common_lock_object(g_progress);
    lv_obj_add_flag(g_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g_progress, 326, 10);
    lv_obj_align(g_progress, LV_ALIGN_TOP_MID, 0, 205);
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
    lv_obj_set_size(g_current_time, 100, 28);
    lv_obj_set_style_text_align(g_current_time, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(g_current_time, LV_ALIGN_TOP_MID, -113, 220);

    g_total_time = player_home_create_label(
        g_overlay, "0:00", lv_color_hex(0xE7E9ED), font_manager_get_ui_font());
    lv_obj_set_size(g_total_time, 100, 28);
    lv_obj_set_style_text_align(g_total_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_total_time, LV_ALIGN_TOP_MID, 113, 220);

    // 播放控制区。
    lv_obj_t *prev = player_home_create_round_button(g_overlay, 60, LV_SYMBOL_PREV);
    lv_obj_align(prev, LV_ALIGN_CENTER, -88, 70);
    lv_obj_add_event_cb(prev, player_home_prev_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *play = player_home_create_round_button(g_overlay, 78, LV_SYMBOL_PLAY);
    lv_obj_align(play, LV_ALIGN_CENTER, 0, 70);
    lv_obj_set_style_bg_opa(play, 225, 0);
    g_play_symbol = lv_obj_get_child(play, 0);
    if (g_play_symbol != nullptr) {
        lv_obj_set_style_text_color(g_play_symbol, lv_color_hex(0x111111), 0);
    }
    lv_obj_add_event_cb(play, player_home_play_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *next = player_home_create_round_button(g_overlay, 60, LV_SYMBOL_NEXT);
    lv_obj_align(next, LV_ALIGN_CENTER, 88, 70);
    lv_obj_add_event_cb(next, player_home_next_cb, LV_EVENT_CLICKED, nullptr);

    // 底部音量条仅作为视觉指示器；Overlay 任意位置上下拖动负责音量预览，松手后提交一次。点击百分比仍切换静音。
    lv_obj_t *volume_status = player_home_create_pill_button(g_overlay, 74, 32, "80%", &g_volume_label);
    lv_obj_align(volume_status, LV_ALIGN_BOTTOM_MID, -100, -52);
    lv_obj_add_event_cb(volume_status, player_home_mute_cb, LV_EVENT_CLICKED, nullptr);

    g_volume_slider = lv_slider_create(g_overlay);
    ui_common_lock_object(g_volume_slider);
    lv_obj_remove_flag(g_volume_slider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g_volume_slider, 190, 9);
    lv_obj_align(g_volume_slider, LV_ALIGN_BOTTOM_MID, 34, -63);
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

    AudioStateSnapshot snapshot = {};
    audio_service_get_snapshot(&snapshot);
    player_home_apply_audio_snapshot(snapshot);
    g_last_audio_state_revision = snapshot.state_revision;

    g_audio_timer = lv_timer_create(player_home_audio_timer_cb, 100, nullptr);
    g_artwork_timer = lv_timer_create(player_home_artwork_timer_cb, 100, nullptr);
    lv_timer_create(player_home_gesture_timer_cb, 20, nullptr);
    g_overlay_timer = lv_timer_create(player_home_overlay_timeout_cb, kOverlayTimeoutMs, nullptr);
    g_gesture_hint_timer = lv_timer_create(
        player_home_gesture_hint_timeout_cb, kGestureHintTimeoutMs, nullptr);
    if (g_gesture_hint_timer != nullptr) {
        lv_timer_pause(g_gesture_hint_timer);
    }
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
        "P1.5R.1.2.2 已启用：主页隐藏释放Artwork lease，恢复时重新绑定当前曲；两槽Surface cache保持current+next；列表=%s 位置=%u/%u track=%u loop=%s volume=%u%% mute=%u",
        list_label,
        static_cast<unsigned>(player_state_get_list_count() > 0 ? player_state_get_list_position() + 1 : 0),
        static_cast<unsigned>(player_state_get_list_count()),
        static_cast<unsigned>(media_library_get_count() > 0 ? player_state_get_index() : 0),
        player_transport_loop_mode_name(player_control_get_loop_mode()),
        static_cast<unsigned>(snapshot.volume_percent),
        static_cast<unsigned>(snapshot.user_muted));
    ESP_LOGI(TAG,
        "P1.3 GestureRouter 已启用：横滑>=72px，顶部/底部边缘=42px，控件优先，滑动后抑制CLICK；顶部下拉=曲库");
}
