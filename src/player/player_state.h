#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "media_library.h"

// 初始化当前歌曲选择状态。
esp_err_t player_state_init();

// 判断选择状态是否可用。
bool player_state_is_ready();

// 获取当前歌曲索引，从 0 开始。
size_t player_state_get_index();

// 获取当前歌曲完整路径，没有歌曲时返回 nullptr。
const char *player_state_get_path();

// 获取当前歌曲格式。
MediaFormat player_state_get_format();

// 选择上一首，首尾循环；音乐库为空时返回 false。
bool player_state_previous();

// 选择下一首，首尾循环；音乐库为空时返回 false。
bool player_state_next();
