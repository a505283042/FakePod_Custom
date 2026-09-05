#pragma once

#include <stddef.h>
#include <stdint.h>

enum class FallbackCoverImageKind : uint8_t
{
    Artwork = 0,
    Cassette,
};

struct FallbackCoverImageLease
{
    const uint8_t *rgb565 = nullptr;
    size_t data_size = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint32_t revision = 0U;
    uint8_t slot_index = 0xFFU;
};

// 无封面替补图来自 TF 卡 /sdcard/System 下的固定 460x460 Baseline JPG。
// 首次使用时读入并解码为 RGB565 PSRAM；同一图片后续复用，避免磁带机械刷新重复解码 JPG。
bool fallback_cover_image_acquire(FallbackCoverImageKind kind, FallbackCoverImageLease *out_lease);
void fallback_cover_image_release(FallbackCoverImageLease *lease);

// 真实封面恢复后调用，释放当前没有 UI lease 固定的替补 RGB565，避免长期占用 PSRAM。
void fallback_cover_image_discard_unpinned();
