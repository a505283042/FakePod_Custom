#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace VideoBrowser
{

static constexpr const char *kRootDirectory = "/sdcard/video";
static constexpr size_t kEntryNameBytes = 256;
static constexpr size_t kPathBytes = 512;

enum EntryFlags : uint32_t
{
    kEntryNone = 0U,
    kEntryDirectory = 1U << 0U,
};

// Video Browser 与 Music/Ebook 一致采用紧凑 Row + PSRAM StringPool。
// 屏幕只绑定少量虚拟 Row，目录大小不再决定 LVGL 对象数量。
struct EntryIndex
{
    uint64_t size_bytes = 0;
    uint32_t name_off = 0;
    uint32_t flags = kEntryNone;
};
static_assert(sizeof(EntryIndex) == 16, "Video EntryIndex must remain compact");

struct DirectorySnapshot
{
    EntryIndex *entries = nullptr; // PSRAM
    char *string_pool = nullptr;   // PSRAM, offset 0 保留空串
    size_t count = 0;
    size_t pool_size = 0;
};

struct DirectoryScanSession
{
    void *dir = nullptr;           // implementation-owned DIR*
    EntryIndex *entries = nullptr; // PSRAM dynamic array
    char *string_pool = nullptr;   // PSRAM dynamic StringPool
    char *path = nullptr;          // PSRAM, kPathBytes
    char *scratch = nullptr;       // PSRAM, kEntryNameBytes + kPathBytes
    size_t count = 0;
    size_t entry_capacity = 0;
    size_t pool_size = 0;
    size_t pool_capacity = 0;
};

bool path_is_inside_root(const char *path);
esp_err_t join_child_path(const char *parent, const char *name, char *out, size_t out_size);
esp_err_t parent_path(const char *path, char *out, size_t out_size);

esp_err_t begin_directory_scan(const char *path, DirectoryScanSession *session);
esp_err_t scan_directory_step(DirectoryScanSession *session, size_t max_raw_entries, bool *out_done);
esp_err_t finish_directory_scan(DirectoryScanSession *session, DirectorySnapshot *out_snapshot);
void cancel_directory_scan(DirectoryScanSession *session);

void release_directory(DirectorySnapshot *snapshot);
const EntryIndex *entry_at(const DirectorySnapshot *snapshot, size_t index);
const char *entry_name(const DirectorySnapshot *snapshot, size_t index);
bool entry_is_directory(const EntryIndex *entry);

} // namespace VideoBrowser
