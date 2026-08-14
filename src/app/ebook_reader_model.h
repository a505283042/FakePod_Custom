#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace EbookReader
{

static constexpr const char *kRootDirectory = "/sdcard/BOOKS";
static constexpr size_t kMaxEntries = 48;
static constexpr size_t kEntryNameBytes = 256;
static constexpr size_t kPathBytes = 512;
static constexpr size_t kPageReadBytes = 2048;

struct Entry
{
    bool is_directory = false;
    uint64_t size_bytes = 0;
    char name[kEntryNameBytes] = {};
};

struct DirectorySnapshot
{
    Entry *entries = nullptr; // PSRAM
    size_t count = 0;
    bool truncated = false;
};

using GlyphWidthFn = uint16_t (*)(uint32_t codepoint, uint32_t next_codepoint);

struct PageLayout
{
    uint16_t text_width_px = 0;
    uint8_t max_lines = 0;
    GlyphWidthFn glyph_width = nullptr;
};

struct TextPage
{
    char *text = nullptr; // PSRAM, 已按页插入换行，UTF-8 NUL terminated
    size_t size = 0;
    uint64_t start_offset = 0;
    uint64_t next_offset = 0;
    uint64_t file_size = 0;
    bool at_start = true;
    bool at_end = false;
};

// 只枚举目录和 UTF-8 TXT 候选；目录优先、名称按字节序排序。
// 所有 FATFS 操作都走 StorageSdLockGuard，且不会长时间持有全局 SD 锁。
esp_err_t scan_directory(const char *path, DirectorySnapshot *out_snapshot);
void release_directory(DirectorySnapshot *snapshot);

// 从稳定文件字节偏移读取单页。每页只读一个小窗口，并按实际字体像素宽度/可见行数切页；
// next_offset 只推进本页真正显示过的 UTF-8 字节及对应行结束符。offset=0 时识别 UTF-8 BOM；UTF-16/非法 UTF-8 会拒绝。
esp_err_t load_text_page(
    const char *path, uint64_t offset, const PageLayout &layout, TextPage *out_page);
void release_text_page(TextPage *page);

bool is_txt_name(const char *name);
bool path_is_inside_root(const char *path);
esp_err_t join_child_path(const char *directory, const char *name, char *out, size_t out_size);
esp_err_t parent_path(const char *path, char *out, size_t out_size);

} // namespace EbookReader
