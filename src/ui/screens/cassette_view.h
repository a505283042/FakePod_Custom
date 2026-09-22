#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

// Music 首页磁带视觉层。只消费已有 CoverSurface；不参与音频解码。
esp_err_t cassette_view_create(lv_obj_t *parent);
bool cassette_view_set_active(bool active);
// 从封面视图切到磁带时先隐藏准备；ready 后再由 player_home 一次提交，避免粉色中间帧。
bool cassette_view_prepare_deferred_active();
bool cassette_view_try_present_deferred();
bool cassette_view_is_present_ready();
void cassette_view_update();
// Music 原有播放控件 Overlay 显示时隐藏磁带专属曲目信息/Mini Lyrics。
void cassette_view_set_controls_visible(bool visible);
// true 表示 Controls 已切到当前歌曲的 PSRAM 预暗 RGB565 缓存，Home 不需要实时 Alpha Backdrop。
bool cassette_view_controls_cache_active();
// Launcher 菜单显示期间冻结磁带机械层；高速 Launcher 可同时隐藏整个 Cassette root。
void cassette_view_set_launcher_suspended(bool suspended);
// 进度条拖动/Seek期间冻结磁带机械层，避免UI动画与Seek提交争用。
void cassette_view_set_seek_frozen(bool frozen);
bool cassette_view_is_active();
