#include "video_seek_index.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "video_seek_index";

namespace VideoSeekIndex
{

namespace
{

// 侧车文件魔数 "FVIX" (FakePod Video Index)
static const char kMagic[4] = {'F', 'V', 'I', 'X'};
static const uint16_t kVersion = 1;

struct Header {
    char magic[4];
    uint16_t version;
    uint16_t granularity_ms;
    uint32_t entry_count;
};

Entry *s_entries = nullptr;
uint32_t s_count = 0;

} // namespace

esp_err_t load(const char *sidecar_path) {
    unload();
    if (sidecar_path == nullptr) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(sidecar_path, "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "侧车索引不存在: %s", sidecar_path);
        return ESP_ERR_NOT_FOUND;
    }

    Header hdr = {};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        ESP_LOGE(TAG, "读取索引头失败");
        return ESP_FAIL;
    }
    if (memcmp(hdr.magic, kMagic, 4) != 0) {
        fclose(f);
        ESP_LOGE(TAG, "索引魔数不匹配");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (hdr.version != kVersion) {
        fclose(f);
        ESP_LOGE(TAG, "索引版本不支持: %u", hdr.version);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (hdr.entry_count == 0 || hdr.entry_count > 1000000U) {
        fclose(f);
        ESP_LOGE(TAG, "索引条目数非法: %u", hdr.entry_count);
        return ESP_ERR_INVALID_SIZE;
    }

    s_entries = static_cast<Entry *>(heap_caps_calloc(
        hdr.entry_count, sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_entries == nullptr) {
        fclose(f);
        ESP_LOGE(TAG, "分配索引内存失败: %u entries", hdr.entry_count);
        return ESP_ERR_NO_MEM;
    }

    const size_t read = fread(s_entries, sizeof(Entry), hdr.entry_count, f);
    fclose(f);
    if (read != hdr.entry_count) {
        ESP_LOGE(TAG, "索引读取不完整: %u/%u", static_cast<unsigned>(read), hdr.entry_count);
        unload();
        return ESP_FAIL;
    }

    s_count = hdr.entry_count;
    ESP_LOGI(TAG, "加载侧车索引: %u 条目, 粒度 %ums, 文件=%s",
        s_count, hdr.granularity_ms, sidecar_path);
    return ESP_OK;
}

esp_err_t lookup(uint32_t target_ms, Entry *out_entry) {
    if (s_entries == nullptr || s_count == 0 || out_entry == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // 二分查找：最大 timestamp_ms <= target_ms
    uint32_t lo = 0, hi = s_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (s_entries[mid].timestamp_ms <= target_ms) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        *out_entry = s_entries[0];
    } else {
        *out_entry = s_entries[lo - 1];
    }
    return ESP_OK;
}

uint32_t count() { return s_count; }

uint32_t duration_ms() {
    if (s_entries == nullptr || s_count == 0) return 0;
    return s_entries[s_count - 1].timestamp_ms;
}

void unload() {
    if (s_entries != nullptr) {
        heap_caps_free(s_entries);
        s_entries = nullptr;
    }
    s_count = 0;
}

} // namespace VideoSeekIndex
