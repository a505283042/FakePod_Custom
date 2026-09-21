#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "media_types.h"
#include "media_catalog_v2.h"

struct MediaLibraryChangeSummary
{
    uint32_t previous_count = 0U;
    uint32_t current_count = 0U;
    uint32_t added_count = 0U;
    uint32_t removed_count = 0U;
    uint32_t updated_count = 0U;
    bool had_previous_catalog = false;
    bool changed = false;
};

enum class MediaLibraryScanEvent : uint8_t
{
    InitialBuild = 0,
    ChangesDetected,
};

using MediaLibraryScanEventCallback = void (*)(
    MediaLibraryScanEvent event,
    uint32_t current_count,
    void *context
);

// 启动期扫描 TF 卡音乐目录并建立运行时 MusicCatalogV2，同时维护 /sdcard/System/library 下的 V2 Index/Manifest。
// 未变化文件通过 FAST(size+mtime) Manifest 复用旧技术信息；V1 仅作为首次升级迁移源。
// 启动扫描每次启动周期只调用一次；USB运行时归还后的刷新走 quiesced hot reload。
// 只有已经存在正式 V2 Catalog 时才统计新增/删除/更新，首次建库不会被误报成“新增全部歌曲”。
esp_err_t media_library_scan(
    MediaLibraryChangeSummary *out_changes = nullptr,
    MediaLibraryScanEventCallback on_scan_event = nullptr,
    void *callback_context = nullptr
);

// USB/维护模式专用事务热刷新。调用前必须已停止 Audio/Artwork/Lyrics 并保持普通TF访问封锁。
// 成功后旧 Catalog 所有权移动到 out_retired；调用方完成 Player/UI generation 重绑定后必须释放它。
// 失败时当前运行时 Catalog 不变。
esp_err_t media_library_hot_reload_quiesced(
    MusicCatalogV2 *out_retired,
    MediaLibraryChangeSummary *out_changes = nullptr,
    MediaLibraryScanEventCallback on_scan_event = nullptr,
    void *callback_context = nullptr
);

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

// 获取指定歌曲的首选封面 locator；没有有效封面时返回 false。
// 返回的 ref/path 只在对应 Catalog generation 未变化期间有效。
bool media_library_get_artwork_view(size_t index, MediaArtworkViewV2 *out_view);
