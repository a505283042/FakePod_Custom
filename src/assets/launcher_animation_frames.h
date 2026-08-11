#pragma once

#include <stddef.h>
#include <stdint.h>

struct LauncherAnimationFrameAsset {
    const uint8_t *data;
    uint32_t size;
    uint16_t progress;
};

static constexpr uint16_t kLauncherAnimationAssetWidth = 340;
static constexpr uint16_t kLauncherAnimationAssetHeight = 340;
static constexpr uint16_t kLauncherAnimationAssetStride = 170;
static constexpr uint32_t kLauncherAnimationAssetPixelBytes = 57800;
static constexpr uint8_t kLauncherAnimationAssetFrameCount = 12;

extern const LauncherAnimationFrameAsset g_launcher_animation_frames[kLauncherAnimationAssetFrameCount];
