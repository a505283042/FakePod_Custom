#pragma once
#include <stdint.h>
namespace fakepod_perf {
inline unsigned busy_percent(uint32_t idle_delta, uint64_t elapsed_us) {
    if (!elapsed_us) return 0U;
    const uint64_t idle = idle_delta < elapsed_us ? idle_delta : elapsed_us;
    return static_cast<unsigned>(((elapsed_us - idle) * 100ULL + elapsed_us / 2ULL) / elapsed_us);
}
inline unsigned fps_tenths(uint32_t frames, uint64_t elapsed_us) {
    return elapsed_us ? static_cast<unsigned>((uint64_t(frames) * 10000000ULL + elapsed_us / 2ULL) / elapsed_us) : 0U;
}
}
