#pragma once

#include <stdint.h>

#include "esp_err.h"

// P1.5R.1.2：触摸采样与 LVGL 渲染解耦，并增加变化序号供业务层去重。
// TouchInputTask 只负责 CST820 I2C 采样和轻量事件发布，绝不调用任何 LVGL API。
struct UiTouchSnapshot
{
    bool pressed;
    int16_t x;
    int16_t y;
    uint32_t tick_ms;
    // DOWN/UP 或按下期间坐标发生变化时递增；LVGL 用它避免重复分发同一快照。
    uint32_t sequence;
};

// DOWN/UP 边沿必须排队，避免 LVGL 忙时一次快速点按完全落在两个 indev poll 之间。
struct UiTouchEdgeEvent
{
    bool pressed;
    int16_t x;
    int16_t y;
    uint32_t tick_ms;
    uint32_t sequence;
};

esp_err_t ui_touch_input_start();
bool ui_touch_input_is_ready();

// 取得最近一次采样快照。MOVE 不排队，只覆盖这里，防止手指移动把队列塞满。
bool ui_touch_input_get_snapshot(UiTouchSnapshot *out_snapshot);

// 非阻塞取得一个 DOWN/UP 边沿事件。
bool ui_touch_input_take_edge(UiTouchEdgeEvent *out_event);

// 是否还有待消费的 DOWN/UP 事件，供 LVGL indev 的 continue_reading 使用。
bool ui_touch_input_has_pending_edge();

// 触摸正在进行，或最近 activity_window_ms 内刚有过触摸活动。
// 频谱等装饰动画用它主动让路，不能用于业务状态判断。
bool ui_touch_input_recent_activity(uint32_t activity_window_ms);
