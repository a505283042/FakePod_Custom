#pragma once
#include <stdint.h>
namespace fakepod_flac {
// Only a known, short remaining tail can require a cached final-frame flush.
inline bool pending_tail(uint64_t total, uint64_t delivered, uint32_t max_block) {
    return total > delivered && max_block != 0 && total - delivered <= max_block;
}
}
