#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace EbookBookmarkStore
{

// 每本书保留一个显式书签。记录只保存路径哈希与稳定文件字节偏移，
// 不持有路径字符串，避免占用很小的 NVS 分区。
esp_err_t load(const char *path, uint64_t file_size, uint64_t *out_offset, bool *out_found);
esp_err_t save(const char *path, uint64_t offset);
esp_err_t clear(const char *path);

} // namespace EbookBookmarkStore
