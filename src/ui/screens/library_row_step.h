#pragma once
#include <stdint.h>
namespace fakepod_library {
inline int32_t align_scroll(int32_t y, int32_t maximum, int32_t step) {
    if (y <= 0) return 0;
    if (y >= maximum) return maximum;
    return (y / step) * step;
}
inline int32_t consume_drag(int32_t &remainder, int32_t delta, int32_t step) {
    remainder += delta;
    const int32_t rows = remainder / step;
    remainder -= rows * step;
    return rows * step;
}
}
