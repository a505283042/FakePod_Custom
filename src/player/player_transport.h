#pragma once

#include <stdint.h>

// 播放模式只影响“自然 EOF”后的动作；手动上一/下一曲始终尊重用户操作。
enum class PlayerLoopMode : uint8_t
{
    Sequential = 0,   // 顺序播放：列表末尾自然停止
    ListRepeat = 1,   // 列表循环：末尾回到第一首
    SingleRepeat = 2, // 单曲循环：自然 EOF 重播当前首
};

// 高频轻量更新：观察 AudioTask Snapshot 的 Finished 边沿并执行自动续播。
void player_transport_update();

// 当前选中歌曲的显式播放入口。不会先单独 Stop；新的 Play 命令由 AudioTask
// 串行收回旧 pipeline，避免“Stop + Play 又二次 shutdown”。
bool player_transport_play_current(const char *reason = nullptr);

// 手动 transport。上一曲在当前歌曲已播放超过 3 秒时重播当前首；否则切上一首。
// 下一曲始终切到当前 Playlist Context 的下一首。手动操作在列表首尾循环。
bool player_transport_previous();
bool player_transport_next();

// 对当前 Playlist 选中的 Track 发起 Seek；不会改变列表 position。
bool player_transport_seek_ms(uint64_t target_ms);

PlayerLoopMode player_transport_get_loop_mode();
bool player_transport_set_loop_mode(PlayerLoopMode mode);
PlayerLoopMode player_transport_cycle_loop_mode();
const char *player_transport_loop_mode_name(PlayerLoopMode mode);
