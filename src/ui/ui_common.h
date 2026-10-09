#pragma once

#include "lvgl.h"

// 禁止对象参与滚动、回弹、惯性和滚动链。
void ui_common_lock_object(lv_obj_t *obj);

// LVGL 任务内调用的轻量全局提示。显示在 top layer，约 1.8 秒后自动隐藏；
// 只用于即时用户反馈，不承担状态机或错误恢复职责。
void ui_common_show_notice(const char *title, const char *detail);
