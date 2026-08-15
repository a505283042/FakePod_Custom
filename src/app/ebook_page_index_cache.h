#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace EbookPageIndexCache
{

// V1 采用“连续页起点 + 稀疏提交”模型：内存 PageIndex 仍然是完整自然页序列，
// 只在累计新增若干页后把新增尾段 append 到 TF cache。这样第二次打开深书签可以
// 直接恢复完整前缀索引，同时上一页/下一页不会遇到 sparse hole。
static constexpr size_t kCommitStridePages = 32U;

struct LoadResult
{
    uint64_t *starts = nullptr; // PSRAM，由调用方接管或 release_load_result()
    size_t count = 0;
    size_t capacity = 0;
};

// view_slot: 0=Fullscreen, 1=Labeled。
// layout_signature 必须包含分页算法/字体/正文宽度/可见行数等会改变页界的因素。
esp_err_t load(
    const char *book_path,
    uint8_t view_slot,
    uint64_t expected_file_size,
    uint32_t layout_signature,
    LoadResult *out_result);

// 把 starts[persisted_count..count) 追加提交到缓存；若缓存不存在/身份不匹配，
// 自动用当前完整前缀重建。成功后 out_persisted_count=count。
esp_err_t commit_prefix(
    const char *book_path,
    uint8_t view_slot,
    uint64_t expected_file_size,
    uint32_t layout_signature,
    const uint64_t *starts,
    size_t count,
    size_t persisted_count,
    size_t *out_persisted_count);

void release_load_result(LoadResult *result);

} // namespace EbookPageIndexCache
