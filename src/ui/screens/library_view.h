#pragma once

#include "lvgl.h"

// Stage 10.6：创建曲库覆盖层。默认隐藏，由播放器首页“曲库”入口打开。
void library_view_create(lv_obj_t *screen);
void library_view_open();
bool library_view_is_visible();
