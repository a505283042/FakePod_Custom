#pragma once

#include "lvgl.h"

// P1.5.1：频谱页框架。当前只使用轻量假数据验证 460x460 方屏页面、横向导航与持续刷新开销。
// 不读取 PCM、不做 FFT、不访问 decoder/I2S；隐藏后动画 timer 完全暂停。
void spectrum_view_create(lv_obj_t *screen);
void spectrum_view_open();
void spectrum_view_close();
bool spectrum_view_is_visible();
