#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 最终封面以 normal native RGB565 为共享基底：封面视图按需保留 dimmed，磁带视图只保留 normal。
// 2 个槽只承担切歌瞬间“旧 lease + 新 Surface”的安全交换，不再维护第三张 wire-order Surface。
// BoundedSPI Cover Present 直接消费 native Surface，并在传输层按需转换 wire-order。
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
    bool dimmed_pinned = false;
};

esp_err_t cover_surface_cache_start();
bool cover_surface_cache_is_ready();

// 只投递内存预处理任务，不访问 SD。调用前压缩封面应已由 ArtworkLoader 缓存。
bool cover_surface_cache_request_track(uint32_t track_index, uint32_t *out_request_id = nullptr);

bool cover_surface_cache_get_snapshot(CoverSurfaceSnapshot *out_snapshot);
bool cover_surface_cache_acquire(uint32_t track_index, CoverSurfaceLease *out_lease);
// 磁带视图只依赖 normal；该 lease 不固定 dimmed，允许磁带稳态回收压暗 Surface。
bool cover_surface_cache_acquire_normal(uint32_t track_index, CoverSurfaceLease *out_lease);
void cover_surface_cache_release(CoverSurfaceLease *lease);

// 封面模式需要 normal+dimmed；磁带模式只保留 normal。切回封面或进入 Launcher 时，
// dimmed 直接从现有 normal 重建，不重新访问 SD，也不重新解码 JPEG/PNG。
void cover_surface_cache_set_dimmed_retained(bool retained);
bool cover_surface_cache_restore_dimmed(uint32_t track_index);

// R.36：新当前曲已经安全绑定后，清理所有其它未 pin Surface。稳态只留下当前曲。
void cover_surface_cache_retain_track(uint32_t track_index);
