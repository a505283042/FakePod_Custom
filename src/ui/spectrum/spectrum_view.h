#pragma once

#include "lvgl.h"

// P1.5.2R.4.4：消费真实16-band FFT Snapshot，单对象绘制24根渐变主柱 + Peak降落点 + 短暗倒影，20 FPS。
// 频谱下方固定预留两行高度，只显示当前同步歌词；同一句过长时按实际 glyph 像素宽度寻找更均衡的两行断点。
// 触摸不暂停频谱；Tap 暂为空操作并预留样式切换；页面级只允许右滑返回主页。
// UI不读取PCM、不做FFT、不访问decoder/I2S；隐藏后动画timer完全暂停。
void spectrum_view_create(lv_obj_t *screen);
void spectrum_view_open();
void spectrum_view_close();
bool spectrum_view_is_visible();
