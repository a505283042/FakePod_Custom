#pragma once

#include "lvgl.h"

// P1.5.3.2R.6：SegmentedColumns 调整为第一/默认样式，HorizontalMirror 第二，NeonRidge 第三。
// 三种样式共用真实16-band FFT Snapshot；Tap 三态循环。
// SegmentedColumns 每列至少1格，保留暗倒影，Peak改为与主柱同宽的3px窄落点；歌词仍保持短句单行、超长才均衡两行。
// 页面级仍只允许右滑返回主页；UI不读取PCM、不做FFT、不访问decoder/I2S；隐藏后timer完全暂停。
void spectrum_view_create(lv_obj_t *screen);
void spectrum_view_open();
void spectrum_view_close();
bool spectrum_view_is_visible();

// R.30 性能审计：返回当前自绘样式名。
const char *spectrum_view_current_style_name();
