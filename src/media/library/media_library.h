#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// 当前支持进入音乐库的文件格式。
enum class MediaFormat : uint8_t
{
    Unknown,
    MP3,
    FLAC,
    WAV,
    NSF,
    NSFE
};

// 扫描 TF 卡音乐目录并建立内存索引。
// 当前只建立路径索引，不读取标题、歌手、封面等元数据。
esp_err_t media_library_scan();

// 判断音乐库扫描是否完成。
bool media_library_is_ready();

// 获取歌曲总数。
size_t media_library_get_count();

// 获取指定歌曲的完整路径，索引越界时返回 nullptr。
const char *media_library_get_path(size_t index);

// 获取指定歌曲格式，索引越界时返回 Unknown。
MediaFormat media_library_get_format(size_t index);

// 获取指定歌曲的文件名（包含扩展名），索引越界时返回 nullptr。
const char *media_library_get_filename(size_t index);

// 复制适合界面显示的歌曲名，默认去掉最后一个扩展名。
// 缓冲区不足时会截断，并始终保证以 \0 结尾。
bool media_library_copy_display_name(size_t index, char *buffer, size_t buffer_size);

// 获取格式名称，主要用于日志和调试。
const char *media_library_format_name(MediaFormat format);
