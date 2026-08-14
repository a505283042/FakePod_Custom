#pragma once

#include "esp_err.h"
#include "lvgl.h"

// 创建播放器首页。UI Reset P1.1：默认纯全屏封面；单击显示整屏暗色控制 Overlay。
// Overlay 使用一次性显隐而非全屏 alpha 动画，复用 Stage 11.2 Seek 与既有 Player/AudioTask 控制链。
void player_home_create(lv_obj_t *screen);

// P1.5.3.2R.36.2：歌词/频谱/曲库等全屏页面隐藏后使用统一恢复事务。
// 先恢复主页 timer/Artwork lease 与 LVGL source，再尝试一次无 barrier 的当前封面物理提交，
// 最后强制 invalidation，确保 BoundedSPI 快路径已熔断时也能由 LVGL 完整重绘。
void player_home_resume_from_fullscreen_view(const char *reason);

// APP.1 Music Lifecycle Adapter：Music 转后台时暂停所有主页前台活动并释放 Artwork UI lease；
// 回到前台时统一恢复 Home。AudioTask/PlayerState 不属于这里，不会被暂停或销毁。
esp_err_t player_home_app_leave_background();
esp_err_t player_home_app_enter_foreground();
bool player_home_app_is_foreground();

// 曲库选歌专用视觉交接：在 PlayerState 改变前短暂 pin 当前封面，
// 新 Track Surface 未 ready 时返回主页仍显示上一首封面；选歌失败必须 cancel。
bool player_home_prepare_track_transition_hold();
void player_home_cancel_track_transition_hold();

// R.30 全页面 LVGL 性能审计：仅暴露轻量只读状态，不改变页面行为。
bool player_home_overlay_is_visible();
bool player_home_launcher_is_visible();
bool player_home_launcher_is_animating();
