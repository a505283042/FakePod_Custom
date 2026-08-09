#pragma once

#include <stdint.h>

// P1.3：播放器首页统一手势仲裁。这里只识别动作，不直接操作 LVGL 页面，
// 页面层通过 take_action() 在自己的 LVGL timer 中消费结果。
enum class UiGestureAction : uint8_t
{
    None = 0,
    SwipeLeft,
    SwipeRight,
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
bool gesture_router_get_vertical_adjust(UiVerticalAdjustSnapshot *out_snapshot);
bool gesture_router_vertical_adjust_is_engaged();
void gesture_router_ack_vertical_adjust_release(uint32_t sequence);

// 页面手势一旦成立，本次 RELEASE 后 LVGL 可能仍产生 CLICKED；页面/按钮回调
// 用这个标记丢弃该次 click，避免“滑一下顺便点了一次”。新一轮按下会自动清除。
bool gesture_router_should_suppress_click();

// latest-wins 单槽动作。调用方应在 LVGL task/timer 上下文消费。
bool gesture_router_take_action(UiGestureAction *out_action);

const char *gesture_router_action_name(UiGestureAction action);
