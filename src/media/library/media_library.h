#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "media_types.h"

// 扫描 TF 卡音乐目录并建立运行时 MusicCatalogV2，同时维护 /sdcard/System/library 下的 V2 Index/Manifest。
// 未变化文件通过 FAST(size+mtime) Manifest 复用旧技术信息；V1 仅作为首次升级迁移源。
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

// 获取扫描/持久化索引中的技术参数。未深度解析的格式也会返回 true，但 flags 不含 MEDIA_TECH_PARSED。
bool media_library_get_technical_info(size_t index, MediaTechnicalInfo *out_info);
