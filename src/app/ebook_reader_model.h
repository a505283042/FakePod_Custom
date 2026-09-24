#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "text_encoding.h"

namespace EbookReader
{

static constexpr const char *kRootDirectory = "/sdcard/txt";
static constexpr size_t kEntryNameBytes = 256;
static constexpr size_t kPathBytes = 512;
static constexpr size_t kPageReadBytes = 2048;
// 持久 PageIndex cache 的分页算法世代。任何会改变 start/next 页界的 parser 规则都必须递增。
static constexpr uint32_t kPaginationAlgorithmRevision = 2U;

enum DirectoryEntryFlags : uint32_t
{
    kDirectoryEntryNone = 0U,
    kDirectoryEntryDirectory = 1U << 0U,
};

// Ebook Directory Index V1：与 Music Catalog 的 Row + StringPool 思路一致。
// Row 只保存定长 POD 字段，文件名统一存入 PSRAM StringPool，避免每项固定占用 256B。
struct DirectoryEntryIndex
{
    uint64_t size_bytes = 0;
    uint32_t name_off = 0;
    uint32_t flags = kDirectoryEntryNone;
};
static_assert(sizeof(DirectoryEntryIndex) == 16, "DirectoryEntryIndex must remain compact");

struct DirectorySnapshot
{
    DirectoryEntryIndex *entries = nullptr; // PSRAM, count 个有效 Row
    char *string_pool = nullptr; // PSRAM, NUL-terminated strings；offset 0 保留为空串
    size_t count = 0;
    size_t pool_size = 0;
};

// Browser cooperative loader 使用的持久目录扫描会话。
// DIR / 路径 / scratch / 动态 EntryIndex / StringPool 都在 begin/end 之间复用；
// 每次 step 只处理少量 FAT 条目。数组按需在 PSRAM 倍增，不再有 48 项硬上限。
struct DirectoryScanSession
{
    void *dir = nullptr; // implementation-owned DIR*
    DirectoryEntryIndex *entries = nullptr; // PSRAM dynamic array
    char *string_pool = nullptr; // PSRAM dynamic StringPool
    char *path = nullptr; // PSRAM, kPathBytes
    char *scratch = nullptr; // PSRAM, kEntryNameBytes + kPathBytes
    size_t count = 0;
    size_t entry_capacity = 0;
    size_t pool_size = 0;
    size_t pool_capacity = 0;
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

// PageIndex Builder V2 使用的持久扫描会话。FILE 与两个 scratch 只在 begin/end 各创建/释放一次，
// 每页扫描复用同一套分页核心，不生成持久正文对象。
struct PageScanSession
{
    void *file = nullptr; // implementation-owned FILE*
    uint8_t *raw = nullptr; // PSRAM, kPageReadBytes
    uint8_t *decoded = nullptr; // 非 UTF-8 时使用，PSRAM UTF-8 window
    uint16_t *source_map = nullptr; // 非 UTF-8 时使用，UTF-8 byte boundary -> 源文件相对 offset
    char *page_text = nullptr; // PSRAM, parser scratch
    uint64_t file_size = 0;
    TextEncoding::Encoding encoding = TextEncoding::Encoding::Utf8;
};

struct PageScanResult
{
    uint64_t start_offset = 0;
    uint64_t next_offset = 0;
    bool at_end = false;
};

// 只枚举目录和 TXT 候选；目录优先、名称按字节序排序。
// Directory Index V1 使用动态 PSRAM EntryIndex + StringPool，不再有 48 项硬截断；
// 内存不足时明确返回 ESP_ERR_NO_MEM，而不是静默丢弃目录项。
// 所有 FATFS 操作都走 StorageSdLockGuard，且不会长时间持有全局 SD 锁。
esp_err_t scan_directory(const char *path, DirectorySnapshot *out_snapshot);

// 非阻塞 Browser 扫描 API：begin 只打开目录/建立小型 PSRAM builder；step 每次处理至多
// max_raw_entries 个 FAT 枚举项；finish 原地排序并把 EntryIndex + StringPool ownership
// 交给 DirectorySnapshot。cancel 可在 APP 切出/返回/重新进入目录时随时安全释放。
esp_err_t begin_directory_scan(const char *path, DirectoryScanSession *session);
esp_err_t scan_directory_step(
    DirectoryScanSession *session, size_t max_raw_entries, bool *out_done);
esp_err_t finish_directory_scan(DirectoryScanSession *session, DirectorySnapshot *out_snapshot);
void cancel_directory_scan(DirectoryScanSession *session);

void release_directory(DirectorySnapshot *snapshot);
const DirectoryEntryIndex *directory_entry_at(const DirectorySnapshot *snapshot, size_t index);
const char *directory_entry_name(const DirectorySnapshot *snapshot, size_t index);
bool directory_entry_is_directory(const DirectoryEntryIndex *entry);

// 从稳定文件字节偏移读取单页。每页只读一个小窗口，并按实际字体像素宽度/可见行数切页。
// 小说重排采用保守策略：短对白/诗句/效果字保留原换行，高置信度网页机械折行软合并，明确网站广告仅隐藏显示。
// 连续多个物理空行折叠为最多一行空行；所有显示变换仍精确消费原 TXT 字节，next_offset 永远保持源文件 offset。
// Reader 始终输出 UTF-8；分页 offset 保持为原始 TXT 文件字节 offset。
// 支持 UTF-8/UTF-8 BOM、带 BOM 的 UTF-16LE/BE 与 GBK/CP936。
esp_err_t detect_text_encoding(const char *path, TextEncoding::Encoding *out_encoding);
esp_err_t load_text_page(
    const char *path, uint64_t offset, TextEncoding::Encoding encoding,
    const PageLayout &layout, TextPage *out_page);

// 长书索引扫描专用：一次打开文件并复用 PSRAM scratch。scan_page 每次仍按 1KB 分块
// 获取 StorageSdLockGuard，并与 load_text_page 共享完全相同的分页 parser。
esp_err_t begin_page_scan(
    const char *path, TextEncoding::Encoding encoding, PageScanSession *session);
esp_err_t scan_page(
    PageScanSession *session, uint64_t offset, const PageLayout &layout, PageScanResult *out_result);
void end_page_scan(PageScanSession *session);

void release_text_page(TextPage *page);

bool is_txt_name(const char *name);
bool path_is_inside_root(const char *path);
esp_err_t join_child_path(const char *directory, const char *name, char *out, size_t out_size);
esp_err_t parent_path(const char *path, char *out, size_t out_size);

} // namespace EbookReader
