#pragma once

#include <stdint.h>

#include "lvgl.h"

// P1.3.5.4：460x460 方屏曲库覆盖层。
// 顶层左右滑切歌曲/歌手/专辑/年代；列表由 CST820 原始坐标直驱，抬手后追加轻量惯性并显示右侧位置条。
// 搜索页复用同一虚拟列表；底部 2x5 键盘按字母组连续输入，多位 query 匹配运行时 initials SearchKey。
void library_view_create(lv_obj_t *screen);
void library_view_open();
bool library_view_is_visible();

// APP 切换专用：仅隐藏曲库并停止惯性，不调用 HomeResume，避免 Music 已进入后台时反向唤醒主页。
void library_view_suspend_for_app_switch();
// PreserveBackground 返回 Music 时原样恢复浏览模式、搜索条件与滚动位置。
void library_view_resume_after_app_switch();

// 由 ui_manager 的 CST820 原始指针路径旁路观察：
// 普通曲库负责横向分类 + 纵向直驱列表；搜索模式只允许结果区纵向滚动，键盘区域交给 LVGL Button。
void library_view_feed_pointer(bool pressed, int16_t x, int16_t y, uint32_t tick_ms);

// R.30 性能审计只读状态。
bool library_view_inertia_is_active();
bool library_view_search_is_active();
