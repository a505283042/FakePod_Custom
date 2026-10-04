#pragma once

#include <stdint.h>

#include "esp_err.h"

// R46.0.70：只负责 NAS 端预生成 Catalog 的下载、TF 缓存与校验。
// 不扫描远端目录、不常驻第二份 Catalog、不参与音频播放。
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

// 初始化只读取 /sdcard/System/nas_catalog.conf 是否存在以及本地 tiny meta；不联网、不建任务。
esp_err_t nas_catalog_service_init();

// 用户显式触发。只允许 Wi-Fi 已连接且 Music AudioTask 当前不持有播放源时同步。
// Worker 为一次性 5KB Internal 栈；索引下载缓存与校验使用 TF + 临时 PSRAM，结束即释放。
esp_err_t nas_catalog_service_request_sync();

bool nas_catalog_service_get_snapshot(NasCatalogSnapshot *out_snapshot);
const char *nas_catalog_service_state_name(NasCatalogState state);
