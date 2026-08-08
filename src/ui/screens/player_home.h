#pragma once

#include "lvgl.h"

// 创建播放器首页。Stage 11.2 进度条支持拖动预览，松手后通过 Player/AudioTask 发起 Seek。
void player_home_create(lv_obj_t *screen);

// Stage 10.6：曲库选择歌曲/列表后强制刷新首页标题、列表位置与播放状态。
void player_home_refresh();
