#pragma once

#include <stdint.h>
#include "media_catalog_v2.h"
#include "nas_library_source.h"
#include "player_state.h"

// Shared Local/NAS namespace for the EXISTING ArtworkLoader -> CoverSurface -> UI pipeline.
// High bit separates a remote track number from the same local track number.
// NAS catalog generation already changes with its index CRC on successful reload.
static inline uint32_t artwork_source_active_generation()
{
    if (player_state_get_source() == PlayerMediaSource::Nas) {
        const uint32_t generation = nas_library_source_generation();
        return generation == 0U ? 0U : (0x80000000U | (generation & 0x7FFFFFFFU));
    }
    const uint32_t generation = media_catalog_v2_generation();
    return generation == 0U ? 0U : (generation & 0x7FFFFFFFU);
}

static inline bool artwork_source_is_nas()
{
    return player_state_get_source() == PlayerMediaSource::Nas;
}
