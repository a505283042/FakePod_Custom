#pragma once
#include <stddef.h>
namespace fakepod_nas {
inline size_t rebuffer_target(size_t capacity) {
    const size_t target = capacity - capacity / 3;
    return target < 256U * 1024U ? target : 256U * 1024U;
}
inline bool wait_for_rebuffer(size_t buffered, size_t capacity, bool eof, bool io_error) {
    return !eof && !io_error && capacity != 0 && buffered < rebuffer_target(capacity);
}
}
