#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "player_transport.h"

// 初始化 Player 控制串行锁。必须在 UI 启动前调用。
esp_err_t player_control_init();

// 高频轻量 Player 服务。system_loop 每轮调用，用于处理自然 EOF 自动续播。
void player_control_update();

// 显式播放当前 Player List 选中的歌曲。
bool player_control_play_current();

// 播放/暂停键：只有 AudioTask 当前会话对应选中 Track 时才执行暂停/恢复；
// Seek 期间忽略播放/暂停操作，等定位完成后再根据最新状态操作；
// 若 Player 已切到另一首，则直接播放当前选中歌曲。
bool player_control_toggle_play_pause();

// 手动上一/下一曲。上一曲超过 3 秒重播当前首；否则按当前 Playlist Context 移动。
bool player_control_previous();
bool player_control_next();

// 播放模式：顺序/列表循环/单曲循环沿用 EOF 策略；随机模式同时接管
// 自然 EOF 和手动下一曲，并在当前 Playlist Context 内随机。
PlayerLoopMode player_control_get_loop_mode();
PlayerLoopMode player_control_cycle_loop_mode();
bool player_control_set_loop_mode(PlayerLoopMode mode);

// 用户音量：0~100，R.13 默认 50（约 -18dB）；30%=-26dB，100%=0dB。
// 静音状态独立于暂停；所有实际 DAC 寄存器操作仍由 AudioTask 执行。
bool player_control_set_volume(uint8_t percent);
bool player_control_volume_up(uint8_t step = 5U);
bool player_control_volume_down(uint8_t step = 5U);
bool player_control_toggle_mute();

// Stage 11.0：按当前 Track 的真实 Playback Clock 时间轴定位。
bool player_control_seek_ms(uint64_t target_ms);

// Stage 10.5：由 UI/列表页绑定新的播放上下文。position 是目标 Group 内位置。
// 这里只切 Player Context，不先单独 Stop；后续 Play 由 AudioTask 串行关闭旧 pipeline。
bool player_control_select_all_tracks(size_t position);
bool player_control_select_artist_group(size_t group_index, size_t position);
bool player_control_select_album_group(size_t group_index, size_t position);
bool player_control_select_decade_group(size_t group_index, size_t position);
