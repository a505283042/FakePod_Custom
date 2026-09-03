#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

// Music 首页磁带视觉层。只消费已有 CoverSurface；不参与音频解码。
esp_err_t cassette_view_create(lv_obj_t *parent);
bool cassette_view_set_active(bool active);
void cassette_view_update();
// Music 原有播放控件 Overlay 显示时隐藏磁带专属曲目信息/Mini Lyrics。
void cassette_view_set_controls_visible(bool visible);
bool cassette_view_is_active();
