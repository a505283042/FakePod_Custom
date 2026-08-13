#pragma once

// SRAM.1：保留 LVGL 9.2.2 内置 TLSF 分配器，只把 64KB backing pool 从 INTERNAL .bss 迁到 PSRAM。
// Boot 在建立 LVGL 前已经把 PSRAM 作为 Fatal 前置条件验证，因此这里不回退 INTERNAL SRAM。
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_MEM_SIZE (64U * 1024U)
#define LV_MEM_POOL_INCLUDE "esp_heap_caps.h"
#define LV_MEM_POOL_ALLOC(size) \
    heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#endif  // LV_CONF_H
