#pragma once

#include <stdint.h>

#include "esp_err.h"

namespace VideoSeekIndex
{

// 侧车索引条目：时间戳(ms) -> 文件偏移
struct Entry {
    uint32_t timestamp_ms;
    uint32_t file_offset;
};

// 加载侧车索引文件。sidecar_path 通常为 video_path + ".fvix"。
// 索引存储在 PSRAM。
esp_err_t load(const char *sidecar_path);

// 查找不超过 target_ms 的最近条目。
// 成功返回 ESP_OK，entry 填充为找到的条目；
// 若 target_ms 早于第一个条目，返回第一个条目。
esp_err_t lookup(uint32_t target_ms, Entry *out_entry);

// 返回条目数量。
uint32_t count();

// 返回总时长（最后一个条目的时间戳）。
uint32_t duration_ms();

// 释放索引。
void unload();

} // namespace VideoSeekIndex
