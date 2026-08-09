#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Player 只保存列表身份与位置，不跨 Catalog generation 缓存 Group/Track 裸指针。
enum class PlayerListType : uint8_t
{
    AllTracks = 0,
    Artist = 1,
    Album = 2,
    Decade = 3,
};

struct PlayerListSnapshot
{
    bool ready = false;
    PlayerListType type = PlayerListType::AllTracks;
    uint32_t catalog_generation = 0;
    uint32_t group_index = UINT32_MAX;
    uint32_t group_id = UINT32_MAX;
    uint32_t position = 0;
    uint32_t track_count = 0;
    uint32_t track_index = UINT32_MAX;
    uint16_t decade_start = 0;
    bool decade_unknown = false;
};

// 初始化为“全部歌曲”列表，位置从 0 开始。
esp_err_t player_playlist_init();
bool player_playlist_is_ready();

// 绑定不同运行时 Group。position 是该列表内位置，不是全局 Track index。
bool player_playlist_bind_all(size_t position);
bool player_playlist_bind_artist(size_t group_index, size_t position);
bool player_playlist_bind_album(size_t group_index, size_t position);
bool player_playlist_bind_decade(size_t group_index, size_t position);

// 获取当前列表快照；若 Catalog generation 已变化则返回 false，防止使用 stale group。
bool player_playlist_get_snapshot(PlayerListSnapshot *out_snapshot);

// 获取当前全局 Track index。
bool player_playlist_get_track_index(size_t *out_track_index);

// 解析当前列表中任意位置对应的全局 Track index，不修改当前播放位置。
// 用于资源预热等只读场景；position 是列表内位置。
bool player_playlist_get_track_index_at_position(size_t position, size_t *out_track_index);

// 在当前列表中首尾循环移动。
bool player_playlist_previous();
bool player_playlist_next();

// 复制当前列表的人类可读名称，例如“全部歌曲”“土屋アンナ”“2010年代”。
bool player_playlist_copy_label(char *buffer, size_t buffer_size);

const char *player_playlist_type_name(PlayerListType type);
