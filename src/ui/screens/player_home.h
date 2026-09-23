#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app_manager.h"
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
// 前台 APP 切换完成后同步磁带预读保留策略：设置页保留，电子书/视频/NSF 释放。
void player_home_apply_background_prefetch_policy(AppId foreground_app);

// R.39.5.4：其他 APP 请求系统 Launcher 时不再复制一套径向 UI。
// 若 Music 在后台，则先由 AppManager 恢复 Music Foreground，再自动展开原生 Launcher；
// BoundedSPI/Surface lease/动画/径向 hit-test 全部继续复用 Music 已验证实现。
esp_err_t player_home_request_launcher_foreground();

// 曲库选歌专用视觉交接：在 PlayerState 改变前短暂 pin 当前封面，
// 新 Track Surface 未 ready 时返回主页仍显示上一首封面；选歌失败必须 cancel。
bool player_home_prepare_track_transition_hold();
void player_home_cancel_track_transition_hold();

// R.30 全页面 LVGL 性能审计：仅暴露轻量只读状态，不改变页面行为。
bool player_home_overlay_is_visible();
bool player_home_launcher_is_visible();
bool player_home_launcher_is_animating();

// R.39.5.6：非 Music APP 的 Launcher 视觉不再复制实现。
// 以下接口直接复用 Music Launcher 的唯一视觉定义：I4 帧、palette、中心图标几何与径向命中。
// 调用方只负责提供自己的压暗背景容器。
static constexpr uint16_t PLAYER_HOME_LAUNCHER_PANEL_SIZE = 340U;
static constexpr uint16_t PLAYER_HOME_LAUNCHER_PANEL_X = 60U;
static constexpr uint16_t PLAYER_HOME_LAUNCHER_PANEL_Y = 60U;
static constexpr uint16_t PLAYER_HOME_LAUNCHER_I4_STRIDE = 170U;
static constexpr uint32_t PLAYER_HOME_LAUNCHER_I4_DATA_BYTES = 57800U;
static constexpr uint32_t PLAYER_HOME_LAUNCHER_I4_IMAGE_BYTES = 64U + PLAYER_HOME_LAUNCHER_I4_DATA_BYTES;
static constexpr uint16_t PLAYER_HOME_LAUNCHER_CENTER_IMAGE_SIZE = 120U;
static constexpr uint16_t PLAYER_HOME_LAUNCHER_CENTER_I2_STRIDE = 30U;
static constexpr uint32_t PLAYER_HOME_LAUNCHER_CENTER_IMAGE_BYTES =
    16U + static_cast<uint32_t>(PLAYER_HOME_LAUNCHER_CENTER_I2_STRIDE) *
    PLAYER_HOME_LAUNCHER_CENTER_IMAGE_SIZE;

uint8_t player_home_launcher_shared_frame_for_progress(int32_t progress);
int32_t player_home_launcher_shared_frame_progress(uint8_t frame_index);
esp_err_t player_home_launcher_shared_render_i4(
    AppId selected, uint8_t frame_index, uint8_t *buffer, size_t buffer_size, lv_image_dsc_t *out_dsc);
esp_err_t player_home_launcher_shared_render_center_i2(
    AppId selected, int32_t progress, uint8_t *buffer, size_t buffer_size, lv_image_dsc_t *out_dsc);
int8_t player_home_launcher_shared_hit_test(
    int32_t screen_x, int32_t screen_y, int32_t panel_x, int32_t panel_y);
AppId player_home_launcher_shared_app_for_index(uint8_t index);
// Music/Ebook 共用唯一 Launcher 选择语义：所有扇区都可选；只有“已注册且不是当前前台”才允许启动。
// 选择动作始终先更新 AppManager launcher_target，未实现 APP 因此仍可正常高亮/显示中心图标。
AppId player_home_launcher_shared_select_index(uint8_t index, bool *out_should_launch);
