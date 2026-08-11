#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

// 首页封面展示器。R.20 正常路径消费后台预处理完成的 normal + dimmed 460x460 RGB565 Surface；
// 压缩 JPEG/PNG -> LVGL decoder 仅保留为兼容回退。
esp_err_t now_playing_artwork_create(lv_obj_t *parent, int32_t size_px, lv_obj_t **out_container);

void now_playing_artwork_update();
void now_playing_artwork_refresh_context();

// P1.5R.1.2.2：主页被歌词/频谱/曲库完整覆盖时释放 UI 持有的封面 lease，
// 避免两槽 Surface cache 被“隐藏的旧封面 + 下一曲”占满，从而淘汰当前曲。
// 再次激活时会强制按当前 Player context 重新 acquire/cache bind。
void now_playing_artwork_set_active(bool active);

// Overlay 明暗接口。R.20 若命中最终 Surface，则直接切 normal/dimmed 并返回 true；
// 只有压缩图回退等非快速路径才返回 false，让调用方继续使用 alpha 黑层兜底。
bool now_playing_artwork_set_dimmed(bool dimmed);
bool now_playing_artwork_has_fast_surface();

// P1.5.3.2R.23：主页允许时，跨 Track 的最终 RGB565 Surface 可直接提交到 CO5300，
// 再无效化少量 Overlay 控件补画；Launcher 等复杂顶层 UI 显示时应暂时禁用。
void now_playing_artwork_set_direct_present_allowed(bool allowed);
bool now_playing_artwork_take_direct_present_event();
