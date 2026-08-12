#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// R.36：最终封面只保留当前曲 normal + dimmed 两张 native RGB565。CoverTask 固定 Core1；
// 2 个槽只承担切歌瞬间“旧 lease + 新 Surface”的安全交换，绑定完成后立即清理旧槽，
// 不再长期保存下一曲，也不再维护第三张 wire-order Surface。DirectPresent 使用 native 在线 swap。
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
    const uint8_t *dimmed_rgb565 = nullptr;
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

// R.36：新当前曲已经安全绑定后，清理所有其它未 pin Surface。稳态只留下当前曲。
void cover_surface_cache_retain_track(uint32_t track_index);
