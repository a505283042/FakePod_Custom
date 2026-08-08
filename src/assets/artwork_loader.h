#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "media_types.h"

// Stage 12.1：异步封面加载只缓存压缩后的 JPEG/PNG 字节，不做图片解码。
// UI 在 Stage 12.2 通过 acquire/release 短期固定缓存条目，避免异步 LRU 淘汰悬空指针。
enum class ArtworkLoadState : uint8_t
{
    Stopped = 0,
    Idle,
    Loading,
    Ready,
    NoArtwork,
    Failed,
};

struct ArtworkLoaderSnapshot
{
    bool ready = false;
    ArtworkLoadState state = ArtworkLoadState::Stopped;
    uint32_t state_revision = 0;
    uint32_t request_id = 0;
    uint32_t catalog_generation = 0;
    uint32_t track_index = UINT32_MAX;
    uint32_t data_size = 0;
    MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
    uint16_t width = 0;
    uint16_t height = 0;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool cache_hit = false;
    uint32_t superseded_count = 0;
};

struct ArtworkCacheStats
{
    uint32_t entry_count = 0;
    uint32_t pinned_count = 0;
    size_t bytes = 0;
    size_t budget_bytes = 0;
};

struct ArtworkCacheLease
{
    const uint8_t *data = nullptr;
    size_t size = 0;
    MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t catalog_generation = 0;
    uint32_t track_index = UINT32_MAX;

    // 内部租约标识。调用方不得修改；release 后整结构清零。
    uint32_t slot_revision = 0;
    uint8_t slot_index = 0xFFU;
};

// 启动独立 ArtworkTask。服务不依赖 LVGL，也不读取/修改 AudioTask 状态。
esp_err_t artwork_loader_start();
bool artwork_loader_is_ready();

// 请求加载指定 Track 的首选 ArtworkRef。请求会复制 locator/path，异步任务不持有 Catalog 裸指针。
// 多次快速请求采用 latest-wins：队列中最多保留一个待执行请求，正在读取的旧请求会在 8KB 分块边界取消。
bool artwork_loader_request_track(uint32_t track_index, uint32_t *out_request_id = nullptr);

bool artwork_loader_get_snapshot(ArtworkLoaderSnapshot *out_snapshot);
bool artwork_loader_get_cache_stats(ArtworkCacheStats *out_stats);

// 获取当前 Catalog generation 下指定 Track 的压缩图片缓存。
// 成功后必须调用 artwork_loader_release_cached()；租约存活期间对应 LRU 条目不会被淘汰。
bool artwork_loader_acquire_cached(uint32_t track_index, ArtworkCacheLease *out_lease);
void artwork_loader_release_cached(ArtworkCacheLease *lease);
