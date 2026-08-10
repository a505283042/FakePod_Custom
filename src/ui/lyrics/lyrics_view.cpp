#include "lyrics_view.h"

#include <stdio.h>
#include <string.h>

#include "audio_service.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font/font_manager.h"
#include "gesture/gesture_router.h"
#include "input/touch_input.h"
#include "lyrics_service.h"
#include "media_catalog_v2.h"
#include "player_control.h"
#include "player_state.h"
#include "ui_common.h"

namespace {

static const char *TAG = "歌词界面";

static constexpr int32_t LYRICS_LINE_X = 22;
static constexpr int32_t LYRICS_LINE_W = 416;
static constexpr int32_t LYRICS_CENTER_Y = 232;
static constexpr int32_t LYRICS_FAR_H = 30;
static constexpr int32_t LYRICS_NEAR_H = 32;
static constexpr int32_t LYRICS_CURRENT_SINGLE_H = 34;
static constexpr int32_t LYRICS_CURRENT_DOUBLE_H = 62;
static constexpr int32_t LYRICS_SENTENCE_GAP = 14;
static constexpr int32_t LYRICS_FAR_GAP = 12;
static constexpr int32_t LYRICS_CURRENT_LINE_SPACE = 2;
static constexpr uint32_t LYRICS_POLL_MS = 120U;
static constexpr uint32_t LYRICS_MOTION_MS = 33U;
static constexpr uint32_t LYRICS_TRANSITION_MS = 300U;
// P1.5R.1.2：歌词平滑动画属于装饰层，触摸期间及松手后短窗口直接让路。
static constexpr uint32_t LYRICS_INTERACTION_YIELD_MS = 120U;
static constexpr uint32_t LYRICS_ACCENT_RGB = 0x74B5E3U;

// P1.4.3：歌词页只增加底部局部控制 Overlay，不做 460x460 全屏 alpha。
// P1.5.3.2R.12：5 秒自动隐藏；默认只接受点击。控制行改为
// “模式 / 上一曲 / 播放 / 下一曲 / 音量”，音量仍必须先点击图标才开启纵向调节。
static constexpr uint32_t LYRICS_OVERLAY_TIMEOUT_MS = 5000U;
static constexpr int32_t LYRICS_OVERLAY_Y = 318;
static constexpr int32_t LYRICS_OVERLAY_H = 142;
static constexpr int32_t LYRICS_PROGRESS_SCALE = 10000;
static constexpr int32_t LYRICS_VOLUME_FULL_SCALE_PX = 220;
// 与封面主页共用同一安全点击区策略：边缘只留给页面级手势。
static constexpr int16_t LYRICS_OVERLAY_TAP_SAFE_LEFT_PX = 36;
static constexpr int16_t LYRICS_OVERLAY_TAP_SAFE_TOP_PX = 60;
static constexpr int16_t LYRICS_OVERLAY_TAP_SAFE_RIGHT_PX = 423;
static constexpr int16_t LYRICS_OVERLAY_TAP_SAFE_BOTTOM_PX = 404;
static constexpr int16_t LYRICS_OVERLAY_TAP_MAX_MOVE_PX = 12;

struct LyricsLayout
{
    int32_t y[LYRICS_VIEW_WINDOW_LINES] = {};
    int32_t h[LYRICS_VIEW_WINDOW_LINES] = {};
    bool current_two_lines = false;
};

static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_title = nullptr;
static lv_obj_t *g_artist = nullptr;
static lv_obj_t *g_status = nullptr;
static lv_obj_t *g_lines[LYRICS_VIEW_WINDOW_LINES] = {};
static lv_obj_t *g_time = nullptr;
static lv_timer_t *g_poll_timer = nullptr;
static lv_timer_t *g_motion_timer = nullptr;

static lv_obj_t *g_control_overlay = nullptr;
static lv_obj_t *g_overlay_progress = nullptr;
static lv_obj_t *g_overlay_current_time = nullptr;
static lv_obj_t *g_overlay_total_time = nullptr;
static lv_obj_t *g_overlay_volume = nullptr;
static lv_obj_t *g_overlay_volume_button = nullptr;
static lv_obj_t *g_overlay_mode_button = nullptr;
static lv_obj_t *g_overlay_play_symbol = nullptr;
static lv_timer_t *g_overlay_timer = nullptr;
static bool g_overlay_visible = false;

static bool g_overlay_progress_dragging = false;
static bool g_overlay_seek_pending = false;
static uint64_t g_overlay_progress_total_ms = 0U;
static uint64_t g_overlay_progress_preview_ms = 0U;
static uint32_t g_overlay_progress_track = UINT32_MAX;
static uint32_t g_overlay_progress_playback_revision = 0U;
static uint32_t g_overlay_seek_base_revision = 0U;
static uint32_t g_overlay_seek_started_tick = 0U;

static bool g_overlay_volume_dragging = false;
static bool g_overlay_volume_adjust_armed = false;
static uint8_t g_overlay_volume_start = 0U;
static uint8_t g_overlay_volume_preview = 0U;
static uint32_t g_overlay_volume_sequence = 0U;

static bool g_visible = false;
static uint32_t g_requested_track = UINT32_MAX;
static uint32_t g_last_document_revision = 0U;
static uint32_t g_last_current_line = UINT32_MAX;
static uint32_t g_last_time_second = UINT32_MAX;
static uint32_t g_last_title_track = UINT32_MAX;
static uint32_t g_last_playback_revision = 0U;
static uint32_t g_last_seek_revision = 0U;
static LyricsLayout g_rest_layout = {};
static bool g_have_layout = false;

static bool g_motion_active = false;
static int64_t g_motion_start_us = 0;
static int32_t g_motion_from_y[LYRICS_VIEW_WINDOW_LINES] = {};
static int32_t g_motion_to_y[LYRICS_VIEW_WINDOW_LINES] = {};

static uint32_t g_clock_track = UINT32_MAX;
static uint32_t g_clock_playback_revision = 0U;
static uint32_t g_clock_seek_revision = 0U;
static AudioPlaybackState g_clock_state = AudioPlaybackState::Starting;
static uint64_t g_clock_observed_position_ms = UINT64_MAX;
static uint64_t g_clock_anchor_position_ms = 0U;
static int64_t g_clock_anchor_us = 0;

static lv_obj_t *lyrics_view_create_label(
    lv_obj_t *parent,
    const char *text,
    lv_color_t color,
    int32_t width,
    int32_t height)
{
    lv_obj_t *label = lv_label_create(parent);
    ui_common_lock_object(label);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, font_manager_get_ui_font(), 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(label, 0, 0);
    return label;
}

static bool lyrics_view_click_suppressed()
{
    return gesture_router_should_suppress_click();
}

static void lyrics_view_control_capture_cb(lv_event_t *event)
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

static lv_obj_t *lyrics_view_create_symbol_button(
    lv_obj_t *parent,
    int32_t size,
    const char *symbol)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(button, 38, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, lyrics_view_control_capture_cb, LV_EVENT_ALL, nullptr);

    lv_obj_t *label = lv_label_create(button);
    ui_common_lock_object(label);
    lv_label_set_text(label, symbol != nullptr ? symbol : "");
    lv_obj_set_style_text_font(label, lv_font_default(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
    return button;
}

static void lyrics_view_mode_icon_draw_cb(lv_event_t *event)
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
                draw(cx, cy - 3, cx, cy + 4);
                draw(cx - 2, cy - 1, cx, cy - 3);
            }
            break;
        case PlayerLoopMode::Shuffle:
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

static void lyrics_view_volume_icon_draw_cb(lv_event_t *event)
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
    line.color = lv_color_hex(g_overlay_volume_adjust_armed ? LYRICS_ACCENT_RGB : 0xF5F7FA);
    line.width = 3;
    line.opa = LV_OPA_COVER;
    line.round_start = 1U;
    line.round_end = 1U;

    auto draw = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        line.p1.x = x0; line.p1.y = y0;
        line.p2.x = x1; line.p2.y = y1;
        lv_draw_line(layer, &line);
    };

    // 58px 触摸圆内使用和主页同量级的大号扬声器，歌词页不再使用小字体 glyph。
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

static lv_obj_t *lyrics_view_create_draw_button(
    lv_obj_t *parent,
    int32_t size,
    lv_event_cb_t draw_cb)
{
    lv_obj_t *button = lv_button_create(parent);
    ui_common_lock_object(button);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(button, 38, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, lyrics_view_control_capture_cb, LV_EVENT_ALL, nullptr);
    if (draw_cb != nullptr) {
        lv_obj_add_event_cb(button, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    }
    return button;
}

static void lyrics_view_overlay_arm_timeout()
{
    if (!g_overlay_visible || g_overlay_timer == nullptr) {
        return;
    }
    lv_timer_set_period(g_overlay_timer, LYRICS_OVERLAY_TIMEOUT_MS);
    lv_timer_reset(g_overlay_timer);
    lv_timer_resume(g_overlay_timer);
}

static void lyrics_view_overlay_cancel_progress()
{
    g_overlay_progress_dragging = false;
    g_overlay_seek_pending = false;
    g_overlay_progress_total_ms = 0U;
    g_overlay_progress_preview_ms = 0U;
    g_overlay_progress_track = UINT32_MAX;
    g_overlay_progress_playback_revision = 0U;
    g_overlay_seek_base_revision = 0U;
    g_overlay_seek_started_tick = 0U;
}

static void lyrics_view_set_volume_adjust_armed(bool armed)
{
    g_overlay_volume_adjust_armed = armed && g_overlay_visible;
    gesture_router_set_vertical_adjust_enabled(g_overlay_volume_adjust_armed);
    if (!g_overlay_volume_adjust_armed) {
        g_overlay_volume_dragging = false;
    }
    if (g_overlay_volume_button != nullptr) {
        lv_obj_set_style_bg_opa(
            g_overlay_volume_button,
            g_overlay_volume_adjust_armed ? 150 : 38,
            0);
        lv_obj_invalidate(g_overlay_volume_button);
    }
}

static void lyrics_view_overlay_show()
{
    if (g_control_overlay == nullptr) {
        return;
    }
    if (!g_overlay_visible) {
        g_overlay_visible = true;
        lyrics_view_set_volume_adjust_armed(false);
        lv_obj_remove_flag(g_control_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_control_overlay);
    }
    if (g_overlay_mode_button != nullptr) {
        lv_obj_invalidate(g_overlay_mode_button);
    }
    lyrics_view_overlay_arm_timeout();
}

static void lyrics_view_overlay_hide()
{
    if (g_control_overlay == nullptr || !g_overlay_visible) {
        return;
    }
    lyrics_view_set_volume_adjust_armed(false);
    g_overlay_visible = false;
    lyrics_view_overlay_cancel_progress();
    if (g_overlay_timer != nullptr) {
        lv_timer_pause(g_overlay_timer);
    }
    lv_obj_add_flag(g_control_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void lyrics_view_overlay_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_overlay_progress_dragging || g_overlay_seek_pending || g_overlay_volume_dragging) {
        lyrics_view_overlay_arm_timeout();
        return;
    }
    lyrics_view_overlay_hide();
}

static void lyrics_view_root_tap_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        lyrics_view_click_suppressed() ||
        !gesture_router_press_was_tap(LYRICS_OVERLAY_TAP_MAX_MOVE_PX)) {
        return;
    }
    if (g_overlay_visible) {
        lyrics_view_overlay_hide();
        return;
    }
    if (!gesture_router_press_started_in_rect(
            LYRICS_OVERLAY_TAP_SAFE_LEFT_PX,
            LYRICS_OVERLAY_TAP_SAFE_TOP_PX,
            LYRICS_OVERLAY_TAP_SAFE_RIGHT_PX,
            LYRICS_OVERLAY_TAP_SAFE_BOTTOM_PX)) {
        return;
    }
    lyrics_view_overlay_show();
}

static uint32_t lyrics_view_current_track()
{
    const size_t selected = player_state_get_index();
    return selected <= UINT32_MAX ? static_cast<uint32_t>(selected) : UINT32_MAX;
}

static uint64_t lyrics_view_total_ms(const AudioStateSnapshot &snapshot)
{
    if (snapshot.sample_rate_hz == 0U || snapshot.total_frames == 0U) {
        return 0U;
    }
    return (snapshot.total_frames * 1000ULL) / snapshot.sample_rate_hz;
}

static void lyrics_view_format_time(uint64_t ms, char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U) {
        return;
    }
    const uint64_t seconds_total = ms / 1000ULL;
    const uint64_t minutes = seconds_total / 60ULL;
    const uint64_t seconds = seconds_total % 60ULL;
    snprintf(buffer, buffer_size, "%llu:%02llu",
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(seconds));
}

static int32_t lyrics_view_progress_value_from_ms(uint64_t position_ms, uint64_t total_ms)
{
    if (total_ms == 0U) {
        return 0;
    }
    if (position_ms >= total_ms) {
        return LYRICS_PROGRESS_SCALE;
    }
    return static_cast<int32_t>(
        (position_ms * static_cast<uint64_t>(LYRICS_PROGRESS_SCALE)) / total_ms);
}

static uint64_t lyrics_view_progress_ms_from_value(int32_t value, uint64_t total_ms)
{
    if (total_ms == 0U || value <= 0) {
        return 0U;
    }
    if (value >= LYRICS_PROGRESS_SCALE) {
        return total_ms > 0U ? total_ms - 1U : 0U;
    }
    return (total_ms * static_cast<uint64_t>(value)) /
        static_cast<uint64_t>(LYRICS_PROGRESS_SCALE);
}

static bool lyrics_view_snapshot_can_scrub(const AudioStateSnapshot &snapshot)
{
    if (!snapshot.ready || !snapshot.seek_supported || snapshot.track_index == UINT32_MAX ||
        snapshot.track_index != player_state_get_index() || snapshot.sample_rate_hz == 0U ||
        snapshot.total_frames == 0U) {
        return false;
    }
    // 沿用首页稳定策略：FLAC 只允许暂停态 Seek，避免播放中重建 pipeline 导致卡顿/错误。
    if (snapshot.format == MediaFormat::FLAC) {
        return snapshot.state == AudioPlaybackState::Paused;
    }
    return snapshot.state == AudioPlaybackState::Playing ||
        snapshot.state == AudioPlaybackState::Paused ||
        snapshot.state == AudioPlaybackState::Seeking;
}

static void lyrics_view_overlay_progress_set_enabled(bool enabled)
{
    if (g_overlay_progress == nullptr) {
        return;
    }
    if (enabled) {
        lv_obj_remove_state(g_overlay_progress, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(g_overlay_progress, LV_STATE_DISABLED);
    }
}

static void lyrics_view_overlay_update_time(uint64_t position_ms, uint64_t total_ms)
{
    char current[24] = {};
    char total[24] = {};
    lyrics_view_format_time(position_ms, current, sizeof(current));
    lyrics_view_format_time(total_ms, total, sizeof(total));
    if (g_overlay_current_time != nullptr) {
        lv_label_set_text(g_overlay_current_time, current);
    }
    if (g_overlay_total_time != nullptr) {
        lv_label_set_text(g_overlay_total_time, total);
    }
}

static void lyrics_view_overlay_progress_sync(
    const AudioStateSnapshot &snapshot,
    uint64_t estimated_position_ms)
{
    if (g_overlay_progress == nullptr) {
        return;
    }
    const uint64_t total_ms = lyrics_view_total_ms(snapshot);
    const bool same_track = snapshot.track_index != UINT32_MAX &&
        snapshot.track_index == player_state_get_index();

    if (g_overlay_progress_dragging &&
        (!same_track || snapshot.track_index != g_overlay_progress_track ||
         snapshot.playback_revision != g_overlay_progress_playback_revision)) {
        lyrics_view_overlay_cancel_progress();
    }
    if (g_overlay_seek_pending) {
        const bool stale = !same_track || snapshot.track_index != g_overlay_progress_track ||
            snapshot.playback_revision != g_overlay_progress_playback_revision;
        const bool committed = snapshot.seek_revision != g_overlay_seek_base_revision;
        const bool failed = snapshot.state == AudioPlaybackState::Error;
        const bool expired = snapshot.state != AudioPlaybackState::Seeking &&
            lv_tick_elaps(g_overlay_seek_started_tick) > 10000U;
        if (stale || committed || failed || expired) {
            g_overlay_seek_pending = false;
        }
    }

    lyrics_view_overlay_progress_set_enabled(lyrics_view_snapshot_can_scrub(snapshot));
    if (g_overlay_progress_dragging || g_overlay_seek_pending) {
        lyrics_view_overlay_update_time(
            g_overlay_progress_preview_ms, g_overlay_progress_total_ms);
        return;
    }
    if (!same_track || total_ms == 0U) {
        lv_slider_set_value(g_overlay_progress, 0, LV_ANIM_OFF);
        lyrics_view_overlay_update_time(0U, total_ms);
        return;
    }
    lv_slider_set_value(
        g_overlay_progress,
        lyrics_view_progress_value_from_ms(estimated_position_ms, total_ms),
        LV_ANIM_OFF);
    lyrics_view_overlay_update_time(estimated_position_ms, total_ms);
}

static void lyrics_view_overlay_progress_cb(lv_event_t *event)
{
    if (event == nullptr || g_overlay_progress == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        lyrics_view_overlay_show();
        AudioStateSnapshot snapshot = {};
        if (!audio_service_get_snapshot(&snapshot) || !lyrics_view_snapshot_can_scrub(snapshot)) {
            return;
        }
        const uint64_t total_ms = lyrics_view_total_ms(snapshot);
        if (total_ms == 0U) {
            return;
        }
        g_overlay_progress_dragging = true;
        g_overlay_seek_pending = false;
        g_overlay_progress_total_ms = total_ms;
        g_overlay_progress_track = snapshot.track_index;
        g_overlay_progress_playback_revision = snapshot.playback_revision;
        g_overlay_seek_base_revision = snapshot.seek_revision;
        g_overlay_progress_preview_ms = lyrics_view_progress_ms_from_value(
            lv_slider_get_value(g_overlay_progress), total_ms);
        lyrics_view_overlay_update_time(
            g_overlay_progress_preview_ms, g_overlay_progress_total_ms);
        return;
    }
    if (code == LV_EVENT_VALUE_CHANGED && g_overlay_progress_dragging) {
        g_overlay_progress_preview_ms = lyrics_view_progress_ms_from_value(
            lv_slider_get_value(g_overlay_progress), g_overlay_progress_total_ms);
        lyrics_view_overlay_update_time(
            g_overlay_progress_preview_ms, g_overlay_progress_total_ms);
        lyrics_view_overlay_arm_timeout();
        return;
    }
    if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) &&
        gesture_router_vertical_adjust_is_engaged()) {
        if (g_overlay_progress_dragging) {
            lyrics_view_overlay_cancel_progress();
        }
        lyrics_view_overlay_arm_timeout();
        return;
    }
    if (code == LV_EVENT_PRESS_LOST && g_overlay_progress_dragging) {
        lyrics_view_overlay_cancel_progress();
        lyrics_view_overlay_arm_timeout();
        return;
    }
    if (code != LV_EVENT_RELEASED || !g_overlay_progress_dragging) {
        return;
    }

    AudioStateSnapshot snapshot = {};
    const bool ok = audio_service_get_snapshot(&snapshot) &&
        lyrics_view_snapshot_can_scrub(snapshot) &&
        snapshot.track_index == g_overlay_progress_track &&
        snapshot.playback_revision == g_overlay_progress_playback_revision;
    if (!ok) {
        lyrics_view_overlay_cancel_progress();
        lyrics_view_overlay_arm_timeout();
        return;
    }

    g_overlay_progress_preview_ms = lyrics_view_progress_ms_from_value(
        lv_slider_get_value(g_overlay_progress), g_overlay_progress_total_ms);
    g_overlay_progress_dragging = false;
    g_overlay_seek_base_revision = snapshot.seek_revision;
    g_overlay_seek_started_tick = lv_tick_get();
    if (player_control_seek_ms(g_overlay_progress_preview_ms)) {
        g_overlay_seek_pending = true;
    } else {
        lyrics_view_overlay_cancel_progress();
    }
    lyrics_view_overlay_arm_timeout();
}

static void lyrics_view_overlay_prev_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        lyrics_view_click_suppressed()) {
        return;
    }
    lyrics_view_overlay_show();
    if (!player_control_previous()) {
        ESP_LOGW(TAG, "歌词Overlay：上一曲请求未能入队");
    }
}

static void lyrics_view_overlay_play_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        lyrics_view_click_suppressed()) {
        return;
    }
    lyrics_view_overlay_show();
    if (!player_control_toggle_play_pause()) {
        ESP_LOGW(TAG, "歌词Overlay：播放/暂停请求未能入队");
    }
}

static void lyrics_view_overlay_next_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        lyrics_view_click_suppressed()) {
        return;
    }
    lyrics_view_overlay_show();
    if (!player_control_next()) {
        ESP_LOGW(TAG, "歌词Overlay：下一曲请求未能入队");
    }
}

static void lyrics_view_overlay_mode_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        !g_overlay_visible || lyrics_view_click_suppressed()) {
        return;
    }

    const PlayerLoopMode mode = player_control_cycle_loop_mode();
    if (g_overlay_mode_button != nullptr) {
        lv_obj_invalidate(g_overlay_mode_button);
    }
    lyrics_view_overlay_arm_timeout();
    ESP_LOGI(TAG, "P1.5.3.2R.12 歌词Overlay播放模式：%s",
        player_transport_loop_mode_name(mode));
}

static void lyrics_view_overlay_volume_mode_cb(lv_event_t *event)
{
    if (event == nullptr || lv_event_get_code(event) != LV_EVENT_CLICKED ||
        !g_overlay_visible || lyrics_view_click_suppressed()) {
        return;
    }

    const bool armed = !g_overlay_volume_adjust_armed;
    lyrics_view_set_volume_adjust_armed(armed);
    lyrics_view_overlay_arm_timeout();
    ESP_LOGI(TAG, "P1.5.3.2R.12 歌词Overlay音量手势：%s",
        armed ? "已进入纵向调节" : "已退出纵向调节");
}

static uint8_t lyrics_view_volume_preview_from_delta(uint8_t start_percent, int16_t delta_y)
{
    const int32_t delta_percent =
        (-static_cast<int32_t>(delta_y) * 100) / LYRICS_VOLUME_FULL_SCALE_PX;
    int32_t value = static_cast<int32_t>(start_percent) + delta_percent;
    if (value < 0) {
        value = 0;
    } else if (value > 100) {
        value = 100;
    }
    return static_cast<uint8_t>(value);
}

static bool lyrics_view_overlay_volume_gesture_update()
{
    if (!g_overlay_visible || !g_overlay_volume_adjust_armed) {
        return false;
    }
    UiVerticalAdjustSnapshot drag = {};
    if (!gesture_router_get_vertical_adjust(&drag)) {
        return false;
    }

    AudioStateSnapshot snapshot = {};
    if (!audio_service_get_snapshot(&snapshot)) {
        if (drag.released) {
            gesture_router_ack_vertical_adjust_release(drag.sequence);
        }
        return true;
    }

    if (!g_overlay_volume_dragging || g_overlay_volume_sequence != drag.sequence) {
        g_overlay_volume_dragging = true;
        g_overlay_volume_sequence = drag.sequence;
        g_overlay_volume_start = snapshot.volume_percent;
        g_overlay_volume_preview = snapshot.volume_percent;
        if (g_overlay_progress_dragging) {
            lyrics_view_overlay_cancel_progress();
        }
    }

    const uint8_t preview = lyrics_view_volume_preview_from_delta(
        g_overlay_volume_start, drag.delta_y);
    if (preview != g_overlay_volume_preview) {
        g_overlay_volume_preview = preview;
        if (g_overlay_volume != nullptr) {
            lv_label_set_text_fmt(g_overlay_volume, "%u%%", static_cast<unsigned>(preview));
        }
    }
    lyrics_view_overlay_arm_timeout();

    if (!drag.released) {
        return true;
    }

    g_overlay_volume_dragging = false;
    gesture_router_ack_vertical_adjust_release(drag.sequence);
    if (g_overlay_volume_preview != g_overlay_volume_start &&
        !player_control_set_volume(g_overlay_volume_preview)) {
        ESP_LOGW(TAG, "歌词Overlay：音量请求未能入队");
    }
    ESP_LOGI(TAG,
        "歌词Overlay纵向音量提交：start=%u%% delta_y=%dpx -> %u%%",
        static_cast<unsigned>(g_overlay_volume_start),
        static_cast<int>(drag.delta_y),
        static_cast<unsigned>(g_overlay_volume_preview));
    return true;
}

static size_t lyrics_view_decode_utf8(const uint8_t *p, uint32_t *codepoint)
{
    if (p == nullptr || codepoint == nullptr || p[0] == 0U) {
        return 0U;
    }
    if (p[0] < 0x80U) {
        *codepoint = p[0];
        return 1U;
    }
    if ((p[0] & 0xE0U) == 0xC0U && p[1] != 0U && (p[1] & 0xC0U) == 0x80U) {
        const uint32_t cp = ((p[0] & 0x1FU) << 6U) | (p[1] & 0x3FU);
        if (cp >= 0x80U) {
            *codepoint = cp;
            return 2U;
        }
    }
    if ((p[0] & 0xF0U) == 0xE0U && p[1] != 0U && p[2] != 0U &&
        (p[1] & 0xC0U) == 0x80U && (p[2] & 0xC0U) == 0x80U) {
        const uint32_t cp = ((p[0] & 0x0FU) << 12U) |
            ((p[1] & 0x3FU) << 6U) | (p[2] & 0x3FU);
        if (cp >= 0x800U && !(cp >= 0xD800U && cp <= 0xDFFFU)) {
            *codepoint = cp;
            return 3U;
        }
    }
    if ((p[0] & 0xF8U) == 0xF0U && p[1] != 0U && p[2] != 0U && p[3] != 0U &&
        (p[1] & 0xC0U) == 0x80U && (p[2] & 0xC0U) == 0x80U && (p[3] & 0xC0U) == 0x80U) {
        const uint32_t cp = ((p[0] & 0x07U) << 18U) |
            ((p[1] & 0x3FU) << 12U) | ((p[2] & 0x3FU) << 6U) | (p[3] & 0x3FU);
        if (cp >= 0x10000U && cp <= 0x10FFFFU) {
            *codepoint = cp;
            return 4U;
        }
    }
    return 0U;
}

static int32_t lyrics_view_measure_text_width(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    const lv_font_t *font = font_manager_get_ui_font();
    if (font == nullptr || font->get_glyph_dsc == nullptr) {
        return static_cast<int32_t>(strlen(text)) * 12;
    }

    int32_t width = 0;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(text);
    while (*p != 0U) {
        uint32_t cp = 0U;
        const size_t used = lyrics_view_decode_utf8(p, &cp);
        if (used == 0U) {
            ++p;
            continue;
        }
        uint32_t next_cp = 0U;
        lyrics_view_decode_utf8(p + used, &next_cp);
        width += static_cast<int32_t>(lv_font_get_glyph_width(font, cp, next_cp));
        p += used;
    }
    return width;
}

static bool lyrics_view_current_needs_two_lines(const LyricsWindowSnapshot &window)
{
    const LyricsWindowLine &current = window.lines[2];
    if (!current.valid || !current.current || current.text[0] == '\0') {
        return false;
    }
    return lyrics_view_measure_text_width(current.text) > (LYRICS_LINE_W - 8);
}

static LyricsLayout lyrics_view_build_layout(const LyricsWindowSnapshot &window)
{
    LyricsLayout layout = {};
    layout.current_two_lines = lyrics_view_current_needs_two_lines(window);
    layout.h[0] = LYRICS_FAR_H;
    layout.h[1] = LYRICS_NEAR_H;
    layout.h[2] = layout.current_two_lines ? LYRICS_CURRENT_DOUBLE_H : LYRICS_CURRENT_SINGLE_H;
    layout.h[3] = LYRICS_NEAR_H;
    layout.h[4] = LYRICS_FAR_H;

    layout.y[2] = LYRICS_CENTER_Y - layout.h[2] / 2;
    layout.y[1] = layout.y[2] - LYRICS_SENTENCE_GAP - layout.h[1];
    layout.y[0] = layout.y[1] - LYRICS_FAR_GAP - layout.h[0];
    layout.y[3] = layout.y[2] + layout.h[2] + LYRICS_SENTENCE_GAP;
    layout.y[4] = layout.y[3] + layout.h[3] + LYRICS_FAR_GAP;
    return layout;
}

static void lyrics_view_apply_line_role(size_t slot, const LyricsLayout &layout, bool is_current)
{
    if (slot >= LYRICS_VIEW_WINDOW_LINES || g_lines[slot] == nullptr) {
        return;
    }
    lv_obj_t *line = g_lines[slot];
    lv_obj_set_size(line, LYRICS_LINE_W, layout.h[slot]);
    lv_obj_set_style_text_line_space(line, is_current ? LYRICS_CURRENT_LINE_SPACE : 0, 0);

    if (is_current) {
        lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(line, lv_color_hex(LYRICS_ACCENT_RGB), 0);
        lv_obj_set_style_text_opa(line, LV_OPA_COVER, 0);
    } else {
        lv_label_set_long_mode(line, LV_LABEL_LONG_DOT);
        const bool near_center = slot == 1U || slot == 2U || slot == 3U;
        lv_obj_set_style_text_color(
            line, near_center ? lv_color_hex(0xAEB5BF) : lv_color_hex(0x6F7782), 0);
        lv_obj_set_style_text_opa(line, LV_OPA_COVER, 0);
    }
}

static void lyrics_view_set_positions(const int32_t y[LYRICS_VIEW_WINDOW_LINES])
{
    for (size_t i = 0U; i < LYRICS_VIEW_WINDOW_LINES; ++i) {
        if (g_lines[i] != nullptr) {
            lv_obj_set_pos(g_lines[i], LYRICS_LINE_X, y[i]);
        }
    }
}

static void lyrics_view_cancel_motion(bool snap_to_rest)
{
    if (snap_to_rest && g_have_layout) {
        lyrics_view_set_positions(g_rest_layout.y);
    }
    g_motion_active = false;
    if (g_motion_timer != nullptr) {
        lv_timer_pause(g_motion_timer);
    }
}

static void lyrics_view_clear_lines()
{
    lyrics_view_cancel_motion(false);
    g_have_layout = false;
    for (size_t i = 0U; i < LYRICS_VIEW_WINDOW_LINES; ++i) {
        if (g_lines[i] != nullptr) {
            lv_label_set_text(g_lines[i], "");
            lv_label_set_long_mode(g_lines[i], LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_color(g_lines[i], lv_color_hex(0x6F7782), 0);
            lv_obj_set_style_text_opa(g_lines[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_line_space(g_lines[i], 0, 0);
        }
    }
}

static void lyrics_view_refresh_header(uint32_t track_index)
{
    if (track_index == g_last_title_track || g_title == nullptr || g_artist == nullptr) {
        return;
    }
    g_last_title_track = track_index;

    MediaTrackViewV2 view = {};
    if (!media_catalog_v2_get_track_view(track_index, &view)) {
        lv_label_set_text(g_title, "歌词");
        lv_label_set_text(g_artist, "");
        return;
    }
    lv_label_set_text(g_title, view.title != nullptr && view.title[0] != '\0' ? view.title : "歌词");
    lv_label_set_text(g_artist, view.artist != nullptr ? view.artist : "");
}

static void lyrics_view_apply_ready_window(const LyricsWindowSnapshot &window, bool animate_forward)
{
    if (g_status != nullptr) {
        lv_obj_add_flag(g_status, LV_OBJ_FLAG_HIDDEN);
    }

    const LyricsLayout old_layout = g_rest_layout;
    const bool had_old_layout = g_have_layout;
    const LyricsLayout new_layout = lyrics_view_build_layout(window);

    lyrics_view_cancel_motion(false);
    for (size_t i = 0U; i < LYRICS_VIEW_WINDOW_LINES; ++i) {
        if (g_lines[i] == nullptr) {
            continue;
        }
        const LyricsWindowLine &line = window.lines[i];
        lv_label_set_text(g_lines[i], line.valid ? line.text : "");
        lyrics_view_apply_line_role(i, new_layout, line.current);
    }

    g_rest_layout = new_layout;
    g_have_layout = true;

    if (!animate_forward || !had_old_layout ||
        ui_touch_input_recent_activity(LYRICS_INTERACTION_YIELD_MS)) {
        // 用户正在操作时不启动 300ms 装饰动画，直接落到正确歌词位置。
        lyrics_view_set_positions(new_layout.y);
        return;
    }

    // 新窗口比旧窗口前进一行：new[-2..+2] 对应 old[-1..+3]。
    // 因此从旧窗口中“同一句歌词原来的位置”起步，再在 300ms 内滑到新位置。
    g_motion_from_y[0] = old_layout.y[1];
    g_motion_from_y[1] = old_layout.y[2];
    g_motion_from_y[2] = old_layout.y[3];
    g_motion_from_y[3] = old_layout.y[4];
    const int32_t bottom_step = old_layout.y[4] - old_layout.y[3];
    g_motion_from_y[4] = old_layout.y[4] + (bottom_step > 0 ? bottom_step : 44);
    for (size_t i = 0U; i < LYRICS_VIEW_WINDOW_LINES; ++i) {
        g_motion_to_y[i] = new_layout.y[i];
    }

    lyrics_view_set_positions(g_motion_from_y);
    g_motion_active = true;
    g_motion_start_us = esp_timer_get_time();
    if (g_motion_timer != nullptr) {
        lv_timer_reset(g_motion_timer);
        lv_timer_resume(g_motion_timer);
    }
}

static void lyrics_view_apply_window_state(
    const LyricsWindowSnapshot &window,
    bool animate_forward)
{
    if (g_status == nullptr) {
        return;
    }

    if (window.state == LyricsLoadState::Ready) {
        lyrics_view_apply_ready_window(window, animate_forward);
        return;
    }

    lyrics_view_clear_lines();
    switch (window.state) {
        case LyricsLoadState::Loading:
            lv_label_set_text(g_status, "正在加载歌词…");
            break;
        case LyricsLoadState::NoLyrics:
            lv_label_set_text(g_status, "暂无同步歌词");
            break;
        case LyricsLoadState::Unsupported:
            lv_label_set_text(g_status, "歌词编码暂不支持\n请使用 UTF-8 LRC");
            break;
        case LyricsLoadState::Failed:
            lv_label_set_text(g_status, "歌词读取失败");
            break;
        default:
            lv_label_set_text(g_status, "暂无歌词");
            break;
    }
    lv_obj_remove_flag(g_status, LV_OBJ_FLAG_HIDDEN);
}

static void lyrics_view_request_if_needed(uint32_t track_index)
{
    if (!lyrics_service_is_ready() || track_index == UINT32_MAX || track_index == g_requested_track) {
        return;
    }
    g_requested_track = track_index;
    g_last_document_revision = 0U;
    g_last_current_line = UINT32_MAX;
    g_last_time_second = UINT32_MAX;
    g_last_playback_revision = 0U;
    g_last_seek_revision = 0U;
    g_clock_track = UINT32_MAX;
    g_clock_observed_position_ms = UINT64_MAX;
    lyrics_view_cancel_motion(false);
    if (!lyrics_service_request_track(track_index)) {
        ESP_LOGW(TAG, "歌词请求未能入队：track=%lu", static_cast<unsigned long>(track_index));
    }
}

static uint64_t lyrics_view_estimated_position_ms(const AudioStateSnapshot &audio)
{
    const int64_t now_us = esp_timer_get_time();
    const bool anchor_changed =
        g_clock_track != audio.track_index ||
        g_clock_playback_revision != audio.playback_revision ||
        g_clock_seek_revision != audio.seek_revision ||
        g_clock_state != audio.state ||
        g_clock_observed_position_ms != audio.position_ms;

    if (anchor_changed) {
        g_clock_track = audio.track_index;
        g_clock_playback_revision = audio.playback_revision;
        g_clock_seek_revision = audio.seek_revision;
        g_clock_state = audio.state;
        g_clock_observed_position_ms = audio.position_ms;
        g_clock_anchor_position_ms = audio.position_ms;
        g_clock_anchor_us = now_us;
    }

    uint64_t position = g_clock_anchor_position_ms;
    if (audio.state == AudioPlaybackState::Playing && now_us > g_clock_anchor_us) {
        position += static_cast<uint64_t>((now_us - g_clock_anchor_us) / 1000LL);
    }
    const uint64_t total = lyrics_view_total_ms(audio);
    if (total > 0U && position > total) {
        position = total;
    }
    return position;
}

static void lyrics_view_motion_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g_visible || !g_motion_active || !g_have_layout) {
        if (g_motion_timer != nullptr) {
            lv_timer_pause(g_motion_timer);
        }
        return;
    }

    if (ui_touch_input_recent_activity(LYRICS_INTERACTION_YIELD_MS)) {
        // 点击/拖动/切页优先于歌词缓动；当前动画直接收束到目标位置。
        lyrics_view_cancel_motion(true);
        return;
    }

    const int64_t elapsed_us = esp_timer_get_time() - g_motion_start_us;
    uint32_t elapsed_ms = elapsed_us > 0 ? static_cast<uint32_t>(elapsed_us / 1000LL) : 0U;
    if (elapsed_ms >= LYRICS_TRANSITION_MS) {
        lyrics_view_set_positions(g_motion_to_y);
        g_motion_active = false;
        lv_timer_pause(g_motion_timer);
        return;
    }

    // smoothstep: t*t*(3-2*t)，只做整数运算，避免歌词动画引入浮点热路径。
    const int32_t t = static_cast<int32_t>((elapsed_ms * 1024U) / LYRICS_TRANSITION_MS);
    const int64_t t64 = t;
    const int32_t ease = static_cast<int32_t>(
        (t64 * t64 * (3072LL - 2LL * t64)) / (1024LL * 1024LL));
    int32_t y[LYRICS_VIEW_WINDOW_LINES] = {};
    for (size_t i = 0U; i < LYRICS_VIEW_WINDOW_LINES; ++i) {
        const int32_t delta = g_motion_to_y[i] - g_motion_from_y[i];
        y[i] = g_motion_from_y[i] + (delta * ease) / 1024;
    }
    lyrics_view_set_positions(y);
}

static void lyrics_view_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g_visible || g_root == nullptr) {
        return;
    }

    if (!lyrics_service_is_ready()) {
        if (g_status != nullptr) {
            lyrics_view_clear_lines();
            lv_label_set_text(g_status, "歌词服务不可用");
            lv_obj_remove_flag(g_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    const uint32_t track_index = lyrics_view_current_track();
    if (track_index == UINT32_MAX) {
        return;
    }
    lyrics_view_refresh_header(track_index);
    lyrics_view_request_if_needed(track_index);

    AudioStateSnapshot audio = {};
    const bool audio_ok = audio_service_get_snapshot(&audio);
    const uint64_t position_ms = audio_ok && audio.track_index == track_index
        ? lyrics_view_estimated_position_ms(audio)
        : 0U;

    if (audio_ok && audio.track_index == track_index && g_overlay_visible) {
        if (g_overlay_play_symbol != nullptr) {
            lv_label_set_text(
                g_overlay_play_symbol,
                audio.state == AudioPlaybackState::Playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
        if (g_overlay_volume != nullptr && !g_overlay_volume_dragging) {
            lv_label_set_text_fmt(
                g_overlay_volume, "%u%%", static_cast<unsigned>(audio.volume_percent));
        }
        lyrics_view_overlay_progress_sync(audio, position_ms);
    }

    LyricsWindowSnapshot window = {};
    if (!lyrics_service_get_window(track_index, position_ms, &window)) {
        return;
    }

    const bool document_changed = window.revision != g_last_document_revision;
    const bool line_changed = window.current_line_index != g_last_current_line;
    if (document_changed || line_changed) {
        bool animate_forward = false;
        if (!document_changed &&
            g_last_current_line != UINT32_MAX &&
            window.current_line_index == g_last_current_line + 1U &&
            audio_ok && audio.track_index == track_index &&
            audio.state == AudioPlaybackState::Playing &&
            audio.playback_revision == g_last_playback_revision &&
            audio.seek_revision == g_last_seek_revision) {
            animate_forward = true;
        }

        g_last_document_revision = window.revision;
        g_last_current_line = window.current_line_index;
        lyrics_view_apply_window_state(window, animate_forward);
    }

    if (audio_ok && audio.track_index == track_index) {
        g_last_playback_revision = audio.playback_revision;
        g_last_seek_revision = audio.seek_revision;
    }

    if (g_time != nullptr && audio_ok && audio.track_index == track_index) {
        const uint32_t second = static_cast<uint32_t>(position_ms / 1000ULL);
        if (second != g_last_time_second) {
            g_last_time_second = second;
            char current[24] = {};
            char total[24] = {};
            lyrics_view_format_time(position_ms, current, sizeof(current));
            lyrics_view_format_time(lyrics_view_total_ms(audio), total, sizeof(total));
            lv_label_set_text_fmt(g_time, "%s / %s", current, total);
        }
    }
}

} // namespace

void lyrics_view_create(lv_obj_t *screen)
{
    if (screen == nullptr || g_root != nullptr) {
        return;
    }

    g_root = lv_obj_create(screen);
    ui_common_lock_object(g_root);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_size(g_root, FAKEPOD_LCD_WIDTH, FAKEPOD_LCD_HEIGHT);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x07090D), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_shadow_width(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_root, lyrics_view_root_tap_cb, LV_EVENT_CLICKED, nullptr);

    g_title = lyrics_view_create_label(g_root, "歌词", lv_color_hex(0xFFFFFF), 390, 34);
    lv_obj_align(g_title, LV_ALIGN_TOP_MID, 0, 20);

    g_artist = lyrics_view_create_label(g_root, "", lv_color_hex(0x8E97A3), 390, 30);
    lv_obj_align(g_artist, LV_ALIGN_TOP_MID, 0, 54);

    for (size_t i = 0U; i < LYRICS_VIEW_WINDOW_LINES; ++i) {
        g_lines[i] = lyrics_view_create_label(g_root, "", lv_color_hex(0x6F7782), LYRICS_LINE_W, LYRICS_NEAR_H);
        lv_obj_set_pos(g_lines[i], LYRICS_LINE_X, 112 + static_cast<int32_t>(i) * 48);
    }

    g_status = lyrics_view_create_label(g_root, "正在加载歌词…", lv_color_hex(0xAEB5BF), 380, 74);
    lv_label_set_long_mode(g_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(g_status, LV_ALIGN_CENTER, 0, 22);

    g_time = lyrics_view_create_label(g_root, "0:00 / 0:00", lv_color_hex(0x737C88), 300, 30);
    lv_obj_align(g_time, LV_ALIGN_BOTTOM_MID, 0, -20);

    // P1.4.3：歌词专用底部轻量 Overlay。只覆盖底部，不压暗整页歌词。
    g_control_overlay = lv_obj_create(g_root);
    ui_common_lock_object(g_control_overlay);
    lv_obj_set_pos(g_control_overlay, 0, LYRICS_OVERLAY_Y);
    lv_obj_set_size(g_control_overlay, FAKEPOD_LCD_WIDTH, LYRICS_OVERLAY_H);
    lv_obj_set_style_radius(g_control_overlay, 18, 0);
    lv_obj_set_style_bg_color(g_control_overlay, lv_color_hex(0x080B10), 0);
    lv_obj_set_style_bg_opa(g_control_overlay, 218, 0);
    lv_obj_set_style_border_width(g_control_overlay, 0, 0);
    lv_obj_set_style_shadow_width(g_control_overlay, 0, 0);
    lv_obj_set_style_pad_all(g_control_overlay, 0, 0);
    lv_obj_remove_flag(g_control_overlay, LV_OBJ_FLAG_SCROLLABLE);

    g_overlay_progress = lv_slider_create(g_control_overlay);
    ui_common_lock_object(g_overlay_progress);
    lv_obj_set_size(g_overlay_progress, 330, 8);
    lv_obj_align(g_overlay_progress, LV_ALIGN_TOP_MID, 0, 14);
    lv_slider_set_range(g_overlay_progress, 0, LYRICS_PROGRESS_SCALE);
    lv_slider_set_value(g_overlay_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(g_overlay_progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_overlay_progress, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_overlay_progress, 45, LV_PART_MAIN);
    lv_obj_set_style_radius(g_overlay_progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_overlay_progress, lv_color_hex(LYRICS_ACCENT_RGB), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_overlay_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_width(g_overlay_progress, 16, LV_PART_KNOB);
    lv_obj_set_style_height(g_overlay_progress, 16, LV_PART_KNOB);
    lv_obj_set_style_radius(g_overlay_progress, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(g_overlay_progress, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_overlay_progress, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_add_event_cb(g_overlay_progress, lyrics_view_control_capture_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(g_overlay_progress, lyrics_view_overlay_progress_cb, LV_EVENT_ALL, nullptr);

    g_overlay_current_time = lyrics_view_create_label(
        g_control_overlay, "0:00", lv_color_hex(0xC8CED7), 100, 24);
    lv_obj_set_style_text_align(g_overlay_current_time, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(g_overlay_current_time, 65, 28);

    // R.12：时间行中间只保留音量百分比；底部统一成五按钮一行：
    // 模式 / 上一曲 / 播放 / 下一曲 / 音量。两侧 58px 大按钮同时扩大视觉和触摸区域。
    g_overlay_volume = lyrics_view_create_label(
        g_control_overlay, "80%", lv_color_hex(LYRICS_ACCENT_RGB), 54, 24);
    lv_obj_set_style_text_align(g_overlay_volume, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_overlay_volume, LV_ALIGN_TOP_MID, 0, 28);

    g_overlay_total_time = lyrics_view_create_label(
        g_control_overlay, "0:00", lv_color_hex(0xC8CED7), 100, 24);
    lv_obj_set_style_text_align(g_overlay_total_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g_overlay_total_time, 295, 28);

    g_overlay_mode_button = lyrics_view_create_draw_button(
        g_control_overlay, 58, lyrics_view_mode_icon_draw_cb);
    lv_obj_align(g_overlay_mode_button, LV_ALIGN_BOTTOM_MID, -164, -9);
    lv_obj_add_event_cb(
        g_overlay_mode_button, lyrics_view_overlay_mode_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *prev = lyrics_view_create_symbol_button(g_control_overlay, 52, LV_SYMBOL_PREV);
    lv_obj_align(prev, LV_ALIGN_BOTTOM_MID, -82, -12);
    lv_obj_add_event_cb(prev, lyrics_view_overlay_prev_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *play = lyrics_view_create_symbol_button(g_control_overlay, 62, LV_SYMBOL_PLAY);
    lv_obj_align(play, LV_ALIGN_BOTTOM_MID, 0, -7);
    lv_obj_set_style_bg_opa(play, 225, 0);
    g_overlay_play_symbol = lv_obj_get_child(play, 0);
    if (g_overlay_play_symbol != nullptr) {
        lv_obj_set_style_text_color(g_overlay_play_symbol, lv_color_hex(0x111111), 0);
    }
    lv_obj_add_event_cb(play, lyrics_view_overlay_play_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *next = lyrics_view_create_symbol_button(g_control_overlay, 52, LV_SYMBOL_NEXT);
    lv_obj_align(next, LV_ALIGN_BOTTOM_MID, 82, -12);
    lv_obj_add_event_cb(next, lyrics_view_overlay_next_cb, LV_EVENT_CLICKED, nullptr);

    g_overlay_volume_button = lyrics_view_create_draw_button(
        g_control_overlay, 58, lyrics_view_volume_icon_draw_cb);
    lv_obj_align(g_overlay_volume_button, LV_ALIGN_BOTTOM_MID, 164, -9);
    lv_obj_add_event_cb(
        g_overlay_volume_button, lyrics_view_overlay_volume_mode_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_add_flag(g_control_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    g_poll_timer = lv_timer_create(lyrics_view_timer_cb, LYRICS_POLL_MS, nullptr);
    if (g_poll_timer != nullptr) {
        // 页面初始隐藏；不要每120ms醒来只做一次 visible 判断。
        lv_timer_pause(g_poll_timer);
    }
    g_motion_timer = lv_timer_create(lyrics_view_motion_timer_cb, LYRICS_MOTION_MS, nullptr);
    if (g_motion_timer != nullptr) {
        lv_timer_pause(g_motion_timer);
    }
    g_overlay_timer = lv_timer_create(
        lyrics_view_overlay_timeout_cb, LYRICS_OVERLAY_TIMEOUT_MS, nullptr);
    if (g_overlay_timer != nullptr) {
        lv_timer_pause(g_overlay_timer);
    }
    ESP_LOGI(TAG,
        "P1.5.3.2R.12 歌词Overlay：底部五按钮=模式/上一曲/播放/下一曲/音量；音量图标58px并仍需点击后才启用纵向调节；纯Tap阈值=%dpx",
        static_cast<int>(LYRICS_OVERLAY_TAP_MAX_MOVE_PX));
}

void lyrics_view_open()
{
    if (g_root == nullptr) {
        return;
    }
    g_visible = true;
    if (g_poll_timer != nullptr) {
        lv_timer_reset(g_poll_timer);
        lv_timer_resume(g_poll_timer);
    }
    g_requested_track = UINT32_MAX;
    g_last_title_track = UINT32_MAX;
    g_last_document_revision = 0U;
    g_last_current_line = UINT32_MAX;
    g_last_time_second = UINT32_MAX;
    g_last_playback_revision = 0U;
    g_last_seek_revision = 0U;
    g_clock_track = UINT32_MAX;
    g_clock_observed_position_ms = UINT64_MAX;
    g_overlay_visible = false;
    g_overlay_volume_dragging = false;
    g_overlay_volume_adjust_armed = false;
    lyrics_view_overlay_cancel_progress();
    gesture_router_set_vertical_adjust_enabled(false);
    if (g_overlay_volume_button != nullptr) {
        lv_obj_set_style_bg_opa(g_overlay_volume_button, 38, 0);
        lv_obj_invalidate(g_overlay_volume_button);
    }
    if (g_overlay_mode_button != nullptr) {
        lv_obj_invalidate(g_overlay_mode_button);
    }
    if (g_control_overlay != nullptr) {
        lv_obj_add_flag(g_control_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_overlay_timer != nullptr) {
        lv_timer_pause(g_overlay_timer);
    }
    lyrics_view_clear_lines();
    if (g_status != nullptr) {
        lv_label_set_text(g_status, "正在加载歌词…");
        lv_obj_remove_flag(g_status, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_time != nullptr) {
        lv_label_set_text(g_time, "0:00 / 0:00");
    }
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    const uint32_t track = lyrics_view_current_track();
    lyrics_view_refresh_header(track);
    lyrics_view_request_if_needed(track);
    ESP_LOGI(TAG, "打开歌词页：track=%lu", static_cast<unsigned long>(track));
}

void lyrics_view_close()
{
    if (g_root == nullptr || !g_visible) {
        return;
    }
    g_visible = false;
    if (g_poll_timer != nullptr) {
        lv_timer_pause(g_poll_timer);
    }
    lyrics_view_overlay_hide();
    gesture_router_set_vertical_adjust_enabled(false);
    lyrics_view_cancel_motion(false);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "关闭歌词页，返回封面主页");
}

bool lyrics_view_is_visible()
{
    return g_visible;
}

bool lyrics_view_overlay_is_visible()
{
    return g_visible && g_overlay_visible;
}

bool lyrics_view_process_overlay_interaction()
{
    if (!g_visible || !g_overlay_visible) {
        return false;
    }
    return lyrics_view_overlay_volume_gesture_update();
}
