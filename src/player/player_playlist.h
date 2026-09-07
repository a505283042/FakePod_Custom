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

// 目录播放范围与曲库浏览列表是两个维度：
// PlayerListType 保留“全部/歌手/专辑/年代”的浏览上下文；
// PlayerFolderScope 决定上一首/下一首/随机/EOF 的实际候选范围；
// All 时继续沿用当前 PlayerListType，Level1/Level2 时改用目录队列。
enum class PlayerFolderScope : uint8_t
{
    All = 0,
    Level1 = 1,
    Level2 = 2,
};

static constexpr size_t PLAYER_FOLDER_PATH_MAX = 384U;

struct PlayerFolderQueueSnapshot
{
    bool ready = false;
    PlayerFolderScope preferred_scope = PlayerFolderScope::All;
    PlayerFolderScope effective_scope = PlayerFolderScope::All;
    uint32_t catalog_generation = 0;
    uint32_t context_id = 0;
    uint32_t position = 0;
    uint32_t track_count = 0;
    uint32_t track_index = UINT32_MAX;
    bool current_in_queue = false;
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

// 在不改变当前 Playlist Context 身份的前提下直接选择列表内 position。
// 供随机播放等 transport 策略使用；会先验证目标位置真实可解析。
bool player_playlist_select_position(size_t position);

// 在当前列表中首尾循环移动。
bool player_playlist_previous();
bool player_playlist_next();

// 复制当前列表的人类可读名称，例如“全部歌曲”“土屋アンナ”“2010年代”。
bool player_playlist_copy_label(char *buffer, size_t buffer_size);

const char *player_playlist_type_name(PlayerListType type);

// 设置/读取目录播放范围。切换范围不会停止或重新打开当前音频，只影响之后的传输候选。
// Level2 仅包含所选第二级目录直属音乐；所选目录失效时运行期安全回退总列表并等待重新选择。
bool player_playlist_set_folder_scope(PlayerFolderScope scope);
PlayerFolderScope player_playlist_get_folder_scope();
const char *player_playlist_folder_scope_name(PlayerFolderScope scope);

// R2：一级/二级范围使用用户显式选择的目录，不再从当前歌曲路径自动推导。
// 路径必须是 /sdcard/MUSIC 下对应层级的规范化目录，并以 '/' 结尾。
bool player_playlist_set_folder_selection(PlayerFolderScope scope, const char *folder_path);
bool player_playlist_copy_folder_selection(
    PlayerFolderScope scope, char *buffer, size_t buffer_size);
bool player_playlist_folder_selection_available(
    PlayerFolderScope scope, const char *folder_path, uint32_t *out_track_count = nullptr);

// 从已发布 Catalog 推导可选目录，不重新扫描 TF。Level1 parent_path 传 nullptr；
// Level2 parent_path 必须是一级目录。Level2 只枚举至少含一首“直属音乐”的二级目录。
size_t player_playlist_get_folder_option_count(
    PlayerFolderScope scope, const char *parent_path = nullptr);
bool player_playlist_copy_folder_option_at(
    PlayerFolderScope scope,
    const char *parent_path,
    size_t position,
    char *path_buffer,
    size_t path_buffer_size,
    uint32_t *out_track_count = nullptr);

// 获取当前实际传输队列。当前歌曲允许暂时位于新选择目录之外。
// preferred_scope 是用户设置，effective_scope 会在所选目录失效时安全回退为 All。
bool player_playlist_get_folder_queue_snapshot(PlayerFolderQueueSnapshot *out_snapshot);

// 解析/选择实际传输队列内位置。All 时保留当前分类列表身份；
// Level1/Level2 真正切歌后收敛为 AllTracks + 全局 Track index，避免持有失配 Group。
bool player_playlist_get_folder_queue_track_index_at_position(size_t position, size_t *out_track_index);
bool player_playlist_select_folder_queue_position(size_t position);

// 按目录播放范围首尾循环移动。
bool player_playlist_folder_previous();
bool player_playlist_folder_next();
