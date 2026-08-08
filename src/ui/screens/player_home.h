#pragma once

#include "lvgl.h"

// 创建 Stage 6 播放器首页骨架。
void player_home_create(lv_obj_t *screen);

// Stage 10.6：曲库选择歌曲/列表后强制刷新首页标题、列表位置与播放状态。
void player_home_refresh();
