#pragma once

#include <stddef.h>

// 播放/暂停键：根据 AudioTask 当前快照决定播放、暂停或恢复。
bool player_control_toggle_play_pause();

// 停止当前音频后，在当前播放列表中切换上一首/下一首。
bool player_control_previous();
bool player_control_next();

// Stage 10.5：由 UI/列表页绑定新的播放上下文。position 是目标 Group 内位置。
// 为保证 AudioTask 与 Player 选择一致，切换上下文前会同步停止当前播放。
bool player_control_select_all_tracks(size_t position);
bool player_control_select_artist_group(size_t group_index, size_t position);
bool player_control_select_album_group(size_t group_index, size_t position);
bool player_control_select_decade_group(size_t group_index, size_t position);
