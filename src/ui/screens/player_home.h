#pragma once

#include "lvgl.h"

// 创建播放器首页。UI Reset P1.1：默认纯全屏封面；单击显示整屏暗色控制 Overlay。
// Overlay 使用一次性显隐而非全屏 alpha 动画，复用 Stage 11.2 Seek 与既有 Player/AudioTask 控制链。
void player_home_create(lv_obj_t *screen);

// Stage 10.6：曲库选择歌曲/列表后强制刷新首页标题、列表位置与播放状态。
void player_home_refresh();

// R.30 全页面 LVGL 性能审计：仅暴露轻量只读状态，不改变页面行为。
bool player_home_overlay_is_visible();
bool player_home_launcher_is_visible();
bool player_home_launcher_is_animating();
