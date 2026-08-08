#pragma once

// 播放/暂停键：根据 AudioTask 当前快照决定播放、暂停或恢复。
bool player_control_toggle_play_pause();

// 停止当前音频后切换上一首/下一首。
bool player_control_previous();
bool player_control_next();
