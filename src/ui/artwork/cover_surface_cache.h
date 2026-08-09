#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// UI Reset P1.2.6：最终显示封面缓存；任务固定 Core1，并支持 system_loop 下一曲预热编排。
// ArtworkLoader 继续负责 SD/内嵌图片读取并缓存压缩 JPEG/PNG；本服务在低优先级任务中
// 把当前封面预处理成屏幕尺寸 RGB565。Overlay 压暗交给 LVGL 在最终 RGB565 上做一次 alpha 合成。
enum class CoverSurfaceState : uint8_t
{
    Stopped = 0,
    Idle,
    Preparing,
    Ready,
    Failed,
};

struct CoverSurfaceSnapshot
{
    bool ready = false;
    CoverSurfaceState state = CoverSurfaceState::Stopped;
    uint32_t state_revision = 0;
    uint32_t request_id = 0;
    uint32_t catalog_generation = 0;
    uint32_t track_index = UINT32_MAX;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t source_width = 0;
    uint16_t source_height = 0;
    uint32_t prepare_ms = 0;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool cache_hit = false;
};

struct CoverSurfaceLease
{
    const uint8_t *normal_rgb565 = nullptr;
    size_t data_size = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t catalog_generation = 0;
    uint32_t track_index = UINT32_MAX;

    uint32_t slot_revision = 0;
    uint8_t slot_index = 0xFFU;
};

esp_err_t cover_surface_cache_start();
bool cover_surface_cache_is_ready();

// 只投递内存预处理任务，不访问 SD。调用前压缩封面应已由 ArtworkLoader 缓存。
bool cover_surface_cache_request_track(uint32_t track_index, uint32_t *out_request_id = nullptr);

bool cover_surface_cache_get_snapshot(CoverSurfaceSnapshot *out_snapshot);
bool cover_surface_cache_acquire(uint32_t track_index, CoverSurfaceLease *out_lease);
void cover_surface_cache_release(CoverSurfaceLease *lease);
