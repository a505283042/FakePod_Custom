#pragma once

#include <stddef.h>

#include "esp_heap_caps.h"

// LVGL 9.2.2 内置 TLSF 的 backing pool。容量由 LV_MEM_SIZE 决定，当前工程固定为 128KB。
// 这里只允许 PSRAM；如果外部 RAM 无法提供对应连续块，则让 LVGL 的现有分配断言暴露故障，
// 不允许静默回退并重新吞掉 INTERNAL SRAM。
static inline void * fakepod_lvgl_psram_pool_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
