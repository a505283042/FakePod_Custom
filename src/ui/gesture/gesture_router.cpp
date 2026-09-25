#include "gesture_router.h"

#include <stdlib.h>

namespace
{
constexpr int16_t kAxisLockDistancePx = 18;
constexpr int16_t kHorizontalTriggerPx = 72;
constexpr int16_t kEdgeTriggerPx = 72;
constexpr int16_t kTopEdgePx = 42;
constexpr int16_t kBottomEdgePx = 417; // 460 - 43
// R.35.1：中央纵向切歌必须比边缘导航更“明确”，避免普通轻拖/歌词阅读误切歌。
// 起点严格排除顶部42px与底部43px；边缘手势仍拥有最高优先级。
constexpr int16_t kTrackVerticalTriggerPx = 86;
constexpr int16_t kTrackVerticalDominancePercent = 135;
constexpr uint32_t kTrackVerticalMinSpeedPxPerSec = 110U;
constexpr uint32_t kHorizontalMinSpeedPxPerSec = 45U;
constexpr uint32_t kEdgeMinSpeedPxPerSec = 40U;
constexpr int16_t kVerticalAdjustTriggerPx = 12;
constexpr int16_t kVerticalAdjustDominancePercent = 135;

enum class GestureAxis : uint8_t
{
    None = 0,
    Horizontal,
    Vertical,
};

struct GestureState
{
    bool pressed = false;
    bool press_origin_valid = false;
    bool control_capture = false;
    bool suppress_click = false;
    bool vertical_adjust_enabled = false;
    bool vertical_adjust_edges_reserved = false;
    bool vertical_adjust_active = false;
    bool vertical_adjust_released = false;
    uint32_t vertical_adjust_sequence = 0U;
    int16_t start_x = 0;
    int16_t start_y = 0;
    int16_t last_x = 0;
    int16_t last_y = 0;
    int16_t max_abs_dx = 0;
    int16_t max_abs_dy = 0;
    uint32_t start_tick_ms = 0U;
    GestureAxis axis = GestureAxis::None;
    UiGestureAction pending = UiGestureAction::None;
};

GestureState g_state = {};

static int32_t gesture_abs(int32_t value)
{
    return value < 0 ? -value : value;
}

static bool gesture_speed_ok(int32_t distance_px, uint32_t elapsed_ms, uint32_t min_speed_px_per_sec)
{
    if (distance_px <= 0) {
        return false;
    }
    if (elapsed_ms == 0U) {
        return true;
    }
    return static_cast<uint64_t>(distance_px) * 1000ULL >=
        static_cast<uint64_t>(min_speed_px_per_sec) * elapsed_ms;
}

static bool gesture_try_activate_vertical_adjust(int32_t dx, int32_t dy)
{
    if (!g_state.vertical_adjust_enabled) {
        return false;
    }
    if (g_state.vertical_adjust_edges_reserved &&
        (g_state.start_y <= kTopEdgePx || g_state.start_y >= kBottomEdgePx)) {
        return false;
    }
    if (g_state.vertical_adjust_active) {
        return true;
    }

    const int32_t ax = gesture_abs(dx);
    const int32_t ay = gesture_abs(dy);
    if (ay < kVerticalAdjustTriggerPx ||
        ay * 100 < ax * kVerticalAdjustDominancePercent) {
        return false;
    }

    // Overlay 纵向拖动优先于控件/页面手势。这里在 LVGL 产生 RELEASED/CLICKED 之前
    // 就设置 suppress_click，因此即使起点落在按钮或 Slider 上，松手也不会误触。
    g_state.vertical_adjust_active = true;
    g_state.vertical_adjust_released = false;
    g_state.suppress_click = true;
    g_state.pending = UiGestureAction::None;
    g_state.axis = GestureAxis::Vertical;
    return true;
}

static void gesture_update_axis_lock(int32_t dx, int32_t dy)
{
    if (g_state.axis != GestureAxis::None) {
        return;
    }

    const int32_t ax = gesture_abs(dx);
    const int32_t ay = gesture_abs(dy);
    if (ax < kAxisLockDistancePx && ay < kAxisLockDistancePx) {
        return;
    }

    // 一旦某方向明显占优就锁定，之后不会从横向切换成纵向或反过来。
    if (ax * 100 >= ay * 120) {
        g_state.axis = GestureAxis::Horizontal;
    } else if (ay * 100 >= ax * 120) {
        g_state.axis = GestureAxis::Vertical;
    }
}

static UiGestureAction gesture_classify(uint32_t tick_ms)
{
    const int32_t dx = static_cast<int32_t>(g_state.last_x) - g_state.start_x;
    const int32_t dy = static_cast<int32_t>(g_state.last_y) - g_state.start_y;
    const int32_t ax = gesture_abs(dx);
    const int32_t ay = gesture_abs(dy);
    const uint32_t elapsed_ms = tick_ms - g_state.start_tick_ms;

    if (g_state.axis == GestureAxis::Vertical) {
        // 顶部/底部边缘永久保留给全局导航；即使反方向拖动，也不下放成切歌。
        if (g_state.start_y <= kTopEdgePx) {
            if (dy >= kEdgeTriggerPx && ay * 100 >= ax * 125 &&
                gesture_speed_ok(ay, elapsed_ms, kEdgeMinSpeedPxPerSec)) {
                return UiGestureAction::PullDownFromTop;
            }
            return UiGestureAction::None;
        }
        if (g_state.start_y >= kBottomEdgePx) {
            if (dy <= -kEdgeTriggerPx && ay * 100 >= ax * 125 &&
                gesture_speed_ok(ay, elapsed_ms, kEdgeMinSpeedPxPerSec)) {
                return UiGestureAction::PullUpFromBottom;
            }
            return UiGestureAction::None;
        }

        // R.35.1：中央区域明确纵向 Flick 才发布切歌动作。动作只在 RELEASE
        // 分类一次，因此一次完整触摸生命周期最多提交一次上一曲/下一曲。
        if (ay >= kTrackVerticalTriggerPx &&
            ay * 100 >= ax * kTrackVerticalDominancePercent &&
            gesture_speed_ok(ay, elapsed_ms, kTrackVerticalMinSpeedPxPerSec)) {
            return dy < 0 ? UiGestureAction::SwipeUpTrack : UiGestureAction::SwipeDownTrack;
        }
        return UiGestureAction::None;
    }

    if (g_state.axis == GestureAxis::Horizontal && ax >= kHorizontalTriggerPx &&
        ax * 100 >= ay * 135 &&
        gesture_speed_ok(ax, elapsed_ms, kHorizontalMinSpeedPxPerSec)) {
        return dx < 0 ? UiGestureAction::SwipeLeft : UiGestureAction::SwipeRight;
    }

    return UiGestureAction::None;
}
} // namespace

void gesture_router_reset()
{
    g_state = {};
}

void gesture_router_feed_pointer(bool pressed, int16_t x, int16_t y, uint32_t tick_ms)
{
    if (pressed) {
        if (!g_state.pressed) {
            // R44.3：新一轮物理触摸开始后，上一轮尚未被 UI timer 消费的动作已经过期。
            // 宁可丢弃旧动作，也不能让它在新手指已经按下后跨触摸生命周期执行。
            g_state.pending = UiGestureAction::None;
            g_state.pressed = true;
            g_state.press_origin_valid = true;
            g_state.suppress_click = false;
            g_state.start_x = x;
            g_state.start_y = y;
            g_state.last_x = x;
            g_state.last_y = y;
            g_state.max_abs_dx = 0;
            g_state.max_abs_dy = 0;
            g_state.start_tick_ms = tick_ms;
            g_state.axis = GestureAxis::None;
            g_state.vertical_adjust_active = false;
            g_state.vertical_adjust_released = false;
            ++g_state.vertical_adjust_sequence;
            if (g_state.vertical_adjust_sequence == 0U) {
                ++g_state.vertical_adjust_sequence;
            }
            return;
        }

        g_state.last_x = x;
        g_state.last_y = y;
        const int32_t dx = static_cast<int32_t>(g_state.last_x) - g_state.start_x;
        const int32_t dy = static_cast<int32_t>(g_state.last_y) - g_state.start_y;
        const int16_t ax = static_cast<int16_t>(gesture_abs(dx));
        const int16_t ay = static_cast<int16_t>(gesture_abs(dy));
        if (ax > g_state.max_abs_dx) g_state.max_abs_dx = ax;
        if (ay > g_state.max_abs_dy) g_state.max_abs_dy = ay;
        if (gesture_try_activate_vertical_adjust(dx, dy)) {
            g_state.suppress_click = true;
            return;
        }
        if (!g_state.control_capture) {
            gesture_update_axis_lock(dx, dy);
        }
        return;
    }

    if (!g_state.pressed) {
        return;
    }

    g_state.last_x = x;
    g_state.last_y = y;
    const int32_t dx = static_cast<int32_t>(g_state.last_x) - g_state.start_x;
    const int32_t dy = static_cast<int32_t>(g_state.last_y) - g_state.start_y;
    const int16_t ax = static_cast<int16_t>(gesture_abs(dx));
    const int16_t ay = static_cast<int16_t>(gesture_abs(dy));
    if (ax > g_state.max_abs_dx) g_state.max_abs_dx = ax;
    if (ay > g_state.max_abs_dy) g_state.max_abs_dy = ay;

    // 极快的滑动可能只有 PRESSED + RELEASED 两个采样，因此 RELEASE 时也必须再判一次。
    if (gesture_try_activate_vertical_adjust(dx, dy)) {
        g_state.vertical_adjust_active = false;
        g_state.vertical_adjust_released = true;
        g_state.suppress_click = true;
        g_state.pending = UiGestureAction::None;
        g_state.pressed = false;
        g_state.axis = GestureAxis::None;
        return;
    }

    if (!g_state.control_capture) {
        gesture_update_axis_lock(dx, dy);
        const UiGestureAction action = gesture_classify(tick_ms);
        if (action != UiGestureAction::None) {
            g_state.pending = action;
            g_state.suppress_click = true;
        }
    }

    g_state.pressed = false;
    g_state.axis = GestureAxis::None;
}

void gesture_router_set_control_capture(bool captured)
{
    g_state.control_capture = captured;
    if (captured) {
        // 控件一旦接管，不保留已经积累的方向锁；防止 Slider 开始拖动后
        // 被之前几像素的手抖误判成页面手势。
        g_state.axis = GestureAxis::None;
    }
}

void gesture_router_set_vertical_adjust_enabled(bool enabled)
{
    g_state.vertical_adjust_enabled = enabled;
    if (!enabled) {
        g_state.vertical_adjust_active = false;
        g_state.vertical_adjust_released = false;
    }
}

void gesture_router_set_vertical_adjust_edges_reserved(bool reserved)
{
    g_state.vertical_adjust_edges_reserved = reserved;
}

bool gesture_router_get_vertical_adjust(UiVerticalAdjustSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr || !g_state.vertical_adjust_enabled ||
        (!g_state.vertical_adjust_active && !g_state.vertical_adjust_released)) {
        return false;
    }
    out_snapshot->active = g_state.vertical_adjust_active;
    out_snapshot->released = g_state.vertical_adjust_released;
    out_snapshot->delta_y = static_cast<int16_t>(
        static_cast<int32_t>(g_state.last_y) - g_state.start_y);
    out_snapshot->sequence = g_state.vertical_adjust_sequence;
    return true;
}

bool gesture_router_vertical_adjust_is_engaged()
{
    return g_state.vertical_adjust_enabled &&
        (g_state.vertical_adjust_active || g_state.vertical_adjust_released);
}

void gesture_router_ack_vertical_adjust_release(uint32_t sequence)
{
    if (g_state.vertical_adjust_released && g_state.vertical_adjust_sequence == sequence) {
        g_state.vertical_adjust_released = false;
    }
}

bool gesture_router_should_suppress_click()
{
    return g_state.suppress_click;
}

bool gesture_router_press_was_tap(int16_t max_move_px)
{
    if (!g_state.press_origin_valid || max_move_px < 0) {
        return false;
    }
    return g_state.max_abs_dx <= max_move_px &&
        g_state.max_abs_dy <= max_move_px;
}

bool gesture_router_press_started_in_rect(
    int16_t left, int16_t top, int16_t right, int16_t bottom)
{
    if (!g_state.press_origin_valid || left > right || top > bottom) {
        return false;
    }
    return g_state.start_x >= left && g_state.start_x <= right &&
        g_state.start_y >= top && g_state.start_y <= bottom;
}

bool gesture_router_take_action(UiGestureAction *out_action)
{
    if (out_action == nullptr || g_state.pending == UiGestureAction::None) {
        return false;
    }
    *out_action = g_state.pending;
    g_state.pending = UiGestureAction::None;
    return true;
}

const char *gesture_router_action_name(UiGestureAction action)
{
    switch (action) {
        case UiGestureAction::SwipeLeft: return "左滑";
        case UiGestureAction::SwipeRight: return "右滑";
        case UiGestureAction::SwipeUpTrack: return "上滑切下一曲";
        case UiGestureAction::SwipeDownTrack: return "下滑切上一曲";
        case UiGestureAction::PullDownFromTop: return "顶部下拉";
        case UiGestureAction::PullUpFromBottom: return "底部上滑";
        default: return "无";
    }
}
