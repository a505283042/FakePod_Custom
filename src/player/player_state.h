#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "media_library.h"
#include "player_playlist.h"

// 初始化当前播放列表/歌曲选择状态。默认绑定“全部歌曲”。
esp_err_t player_state_init();

// 判断选择状态是否可用。
bool player_state_is_ready();

// Catalog generation 热替换后重建 Playlist Context。preferred_path 仍存在时恢复到该歌曲；
// 已删除时按旧全局索引回退到相邻有效歌曲，空库保持空列表，不自动播放。
bool player_state_rebind_after_catalog_reload(const char *preferred_path, size_t fallback_track_index);

// 获取当前歌曲全局 Track 索引，从 0 开始。
size_t player_state_get_index();

// 获取当前歌曲完整路径，没有歌曲或列表 stale 时返回 nullptr。
const char *player_state_get_path();

// 获取当前歌曲格式。
MediaFormat player_state_get_format();

// 获取原始曲库浏览列表快照，用于分类浏览/恢复。
bool player_state_get_list_snapshot(PlayerListSnapshot *out_snapshot);
// 位置/数量面向当前实际播放队列：总列表沿用分类上下文，一级/二级返回目录队列。
size_t player_state_get_list_position();
size_t player_state_get_list_count();
PlayerListType player_state_get_list_type();
bool player_state_copy_list_label(char *buffer, size_t buffer_size);

// 目录播放范围独立于上面的曲库浏览上下文；传输层的上一首/下一首/随机/EOF 使用此队列。
bool player_state_set_folder_scope(PlayerFolderScope scope);
PlayerFolderScope player_state_get_folder_scope();
bool player_state_set_folder_selection(PlayerFolderScope scope, const char *folder_path);
bool player_state_copy_folder_selection(
    PlayerFolderScope scope, char *buffer, size_t buffer_size);
bool player_state_get_folder_queue_snapshot(PlayerFolderQueueSnapshot *out_snapshot);
bool player_state_select_folder_queue_position(size_t position);

// 切换播放上下文。position 是目标列表内的位置，不是全局 Track index。
bool player_state_select_all_tracks(size_t position);
bool player_state_select_artist_group(size_t group_index, size_t position);
bool player_state_select_album_group(size_t group_index, size_t position);
bool player_state_select_decade_group(size_t group_index, size_t position);

// 在当前列表内直接选择 position，不改变当前 Playlist Context 身份。
bool player_state_select_position(size_t position);

// 在当前目录播放范围内选择上一首/下一首，首尾循环；列表为空或 stale 时返回 false。
bool player_state_previous();
bool player_state_next();
