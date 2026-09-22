#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

// Music 首页磁带视觉层。只消费已有 CoverSurface；不参与音频解码。
esp_err_t cassette_view_create(lv_obj_t *parent);
bool cassette_view_set_active(bool active);
// Launcher 高速菜单只临时隐藏磁带场景，保留下一首预缓存；真正退出仍使用 set_active(false)。
bool cassette_view_set_launcher_scene_hidden(bool hidden);
// 曲库点歌前短暂 pin 当前磁带封面；返回主页时若新歌 Surface 未就绪，继续显示旧封面。
bool cassette_view_prepare_track_transition_hold();
void cassette_view_cancel_track_transition_hold();
// 从封面视图切到磁带时先隐藏准备；ready 后再由 player_home 一次提交，避免粉色中间帧。
bool cassette_view_prepare_deferred_active();
bool cassette_view_try_present_deferred();
bool cassette_view_is_present_ready();
void cassette_view_update();
// Music 原有播放控件 Overlay 显示时隐藏磁带专属曲目信息/Mini Lyrics。
void cassette_view_set_controls_visible(bool visible);
// true 表示 Controls 当前已有一张稳定的 PSRAM 预暗背景正在显示；即使目标曲已变化也先保持旧图，Home 不需要实时 Alpha Backdrop。
bool cassette_view_controls_cache_active();
// Launcher 菜单显示期间冻结磁带机械层；高速 Launcher 可同时隐藏整个 Cassette root。
void cassette_view_set_launcher_suspended(bool suspended);
// 进度条拖动/Seek期间冻结磁带机械层，避免UI动画与Seek提交争用。
void cassette_view_set_seek_frozen(bool frozen);
bool cassette_view_is_active();
