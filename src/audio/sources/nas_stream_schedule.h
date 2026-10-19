#pragma once
#include <stddef.h>
#include <stdint.h>
namespace fakepod_nas {
// Startup/low-water refills retain original priority. No extra allocation/task.
inline bool should_cooperate(bool primed, size_t buffered, size_t capacity, int64_t elapsed_us)
{
    return primed && capacity != 0 && buffered >= capacity - capacity / 4 && elapsed_us >= 2000;
}
}
