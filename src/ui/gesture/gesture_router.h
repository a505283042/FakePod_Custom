#pragma once

#include <stdint.h>

// P1.3：播放器首页统一手势仲裁。这里只识别动作，不直接操作 LVGL 页面，
// 页面层通过 take_action() 在自己的 LVGL timer 中消费结果。
enum class UiGestureAction : uint8_t
{
    None = 0,
    SwipeLeft,
    SwipeRight,
    // R.35.1：非顶部/底部边缘起手的明确纵向 Flick。
    // 页面层决定是否将其映射为上一曲/下一曲；Router 本身不直接操作 Player。
    SwipeUpTrack,
    SwipeDownTrack,
    PullDownFromTop,
    PullUpFromBottom,
};

void gesture_router_reset();

// 由 CST820 -> LVGL 输入桥接层喂入原始指针状态。
void gesture_router_feed_pointer(bool pressed, int16_t x, int16_t y, uint32_t tick_ms);

// 控件优先：Slider / Button 在 PRESSED~RELEASED 生命周期内占有本次触摸，
// GestureRouter 不会把拖动控件误判为页面手势。
void gesture_router_set_control_capture(bool captured);

// Overlay 纵向调节通道：启用后，任意控件/背景上的明显纵向拖动都优先被识别为
// “连续调节手势”。它只发布位移，不直接修改音量；页面层负责预览，并在 RELEASE 后提交一次。
struct UiVerticalAdjustSnapshot
{
    bool active = false;
    bool released = false;
    int16_t delta_y = 0;
    uint32_t sequence = 0U;
};

void gesture_router_set_vertical_adjust_enabled(bool enabled);
// 设置页等滚动页面可保留顶部/底部边缘给全局导航，避免连续纵向拖动抢走Launcher手势。
void gesture_router_set_vertical_adjust_edges_reserved(bool reserved);
bool gesture_router_get_vertical_adjust(UiVerticalAdjustSnapshot *out_snapshot);
bool gesture_router_vertical_adjust_is_engaged();
void gesture_router_ack_vertical_adjust_release(uint32_t sequence);

// 页面手势一旦成立，本次 RELEASE 后 LVGL 可能仍产生 CLICKED；页面/按钮回调
// 用这个标记丢弃该次 click，避免“滑一下顺便点了一次”。新一轮按下会自动清除。
bool gesture_router_should_suppress_click();

// P1.5.3.2R.11：严格区分“点击”和“短距离滑动”。
// max_move_px 表示从按下起点开始，整个触摸生命周期允许的最大横/纵偏移；
// 即使手指最后又回到起点，只要中途移动超过阈值，也不会被当成 Tap。
bool gesture_router_press_was_tap(int16_t max_move_px);

// 返回最近一轮触摸按下时的原始坐标是否位于给定闭区间内。
// RELEASE 后仍保留该起点，供 LVGL 随后产生的 CLICKED 回调判断“触摸从哪里开始”。
// 这样顶部/底部/左右边缘可以永久留给页面手势，而不会因为抬手落到中央被误判成 Overlay Tap。
bool gesture_router_press_started_in_rect(
    int16_t left, int16_t top, int16_t right, int16_t bottom);

// latest-wins 单槽动作。调用方应在 LVGL task/timer 上下文消费。
bool gesture_router_take_action(UiGestureAction *out_action);

const char *gesture_router_action_name(UiGestureAction action);
