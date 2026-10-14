#pragma once

#include "lvgl.h"

// 禁止对象参与滚动、回弹、惯性和滚动链。
void ui_common_lock_object(lv_obj_t *obj);

// LVGL 任务内调用的轻量全局提示。显示在 top layer，约 1.8 秒后自动隐藏；
// 只用于即时用户反馈，不承担状态机或错误恢复职责。
void ui_common_show_notice(const char *title, const char *detail);

// NAS -> TF 下载期间的全屏阻塞提示。可从任意任务调用；内部通过 lv_async_call
// 切回 LVGL 线程更新界面。显示期间吞掉触摸，防止高占用下载阶段继续操作其它 UI。
void ui_common_set_nas_sync_blocking(bool visible);
