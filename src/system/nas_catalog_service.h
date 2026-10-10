#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// R46.0.70：只负责 NAS 端预生成 Catalog 的下载、TF 缓存与校验。
// 不扫描远端目录；R46.0.78 仅额外提供 HTTP 播放端点/URL 构造，不持有音频连接。
enum class NasCatalogState : uint8_t
{
    Unconfigured = 0,
    Idle,
    Syncing,
    UpToDate,
    Updated,
    Failed,
};

struct NasCatalogSnapshot
{
    bool ready = false;
    bool configured = false;
    bool cached = false;
    bool syncing = false;
    NasCatalogState state = NasCatalogState::Unconfigured;
    esp_err_t last_error = ESP_OK;
    uint32_t track_count = 0U;
    uint64_t revision = 0ULL;
};


// R46.0.117：配置了 track_url 时继续使用静态 short-ID；
// 显式配置 music_url 且 track_url 留空时，改用 V2 UTF-8 相对路径访问 WebDAV/HTTP。
// 索引格式、音频缓冲以及播放队列均保持原状。
struct NasPlaybackEndpoint
{
    char track_base_url[256] = {};
    char music_base_url[256] = {};
    char username[64] = {};
    char password[96] = {};
};

// 初始化只读取 /sdcard/System/nas_catalog.conf 是否存在以及本地 tiny meta；不联网、不建任务。
esp_err_t nas_catalog_service_init();

// 用户显式触发。只允许 Wi-Fi 已连接且 Music AudioTask 当前不持有播放源时同步。
// Worker 为一次性 5KB Internal 栈；索引下载缓存与校验使用 TF + 临时 PSRAM，结束即释放。
esp_err_t nas_catalog_service_request_sync();

bool nas_catalog_service_get_snapshot(NasCatalogSnapshot *out_snapshot);
const char *nas_catalog_service_state_name(NasCatalogState state);

// 从 TF 配置解析播放端点；不联网。HTTP-only，与 Catalog Foundation 保持一致。
esp_err_t nas_catalog_service_get_playback_endpoint(NasPlaybackEndpoint *out_endpoint);

// 有 track_url 时保持 R46.0.96 short-ID URL；显式配置 music_url 且 track_url 为空时，
// 按 V2 中的 UTF-8 相对路径编码生成原始文件 URL（WebDAV GET，无硬链接）。
esp_err_t nas_catalog_service_build_track_url(
    const NasPlaybackEndpoint *endpoint,
    const char *relative_path,
    char *out_url,
    size_t out_url_size);
