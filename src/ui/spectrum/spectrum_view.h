#pragma once

#include "lvgl.h"

// P1.5.3.3：默认 Neon Ridge 流光山脊，40根低亮度竖向stems，并提高山脊/横向频谱纵向动态范围；Tap 可切换横向渐变频谱。
// 两种样式共用真实16-band FFT Snapshot，并都保留歌名/歌手、均衡两行当前歌词和时间。
// 页面级仍只允许右滑返回主页；UI不读取PCM、不做FFT、不访问decoder/I2S；隐藏后timer完全暂停。
void spectrum_view_create(lv_obj_t *screen);
void spectrum_view_open();
void spectrum_view_close();
bool spectrum_view_is_visible();
