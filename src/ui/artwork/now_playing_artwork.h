#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

// 首页封面展示器。P1.2.5 正常路径消费后台预处理完成的 460x460 RGB565 surface；
// 压缩 JPEG/PNG -> LVGL decoder 仅保留为兼容回退。
esp_err_t now_playing_artwork_create(lv_obj_t *parent, int32_t size_px, lv_obj_t **out_container);

void now_playing_artwork_update();
void now_playing_artwork_refresh_context();

// Overlay 明暗兼容接口。P1.2.5 不再保存 dimmed surface，因此返回 false，
// 调用方使用固定 alpha 黑层；由于底图已是最终 RGB565，不会重新解码/缩放。
bool now_playing_artwork_set_dimmed(bool dimmed);
bool now_playing_artwork_has_fast_surface();
