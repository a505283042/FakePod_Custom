#include "video_seek_index.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "video_media_io.h"

static const char *TAG = "video_seek_index";

namespace VideoSeekIndex
{

namespace
{

static const char kMagic[4] = {'F', 'V', 'I', 'X'};
static constexpr uint16_t kVersionV1 = 1U;
static constexpr uint16_t kVersionV2 = 2U;
static constexpr uint32_t kMaxEntryCount = 100000U;
static constexpr uint32_t kFingerprintSampleBytes = 512U;
static constexpr uint32_t kFingerprintReadBytes = kFingerprintSampleBytes;

struct HeaderV1 {
    char magic[4];
    uint16_t version;
    uint16_t granularity_ms;
    uint32_t entry_count;
};

struct HeaderV2Extra {
    uint32_t source_size;
    uint32_t source_crc32;
};

struct EntryV1 {
    uint32_t timestamp_ms;
    uint32_t file_offset;
};

static_assert(sizeof(HeaderV1) == 12U, "FVIX V1 header layout changed");
static_assert(sizeof(HeaderV2Extra) == 8U, "FVIX V2 header extension layout changed");
static_assert(sizeof(EntryV1) == 8U, "FVIX V1 entry layout changed");
static_assert(sizeof(Entry) == 16U, "FVIX V2 entry layout changed");

Entry *s_entries = nullptr;
uint32_t s_count = 0U;
uint16_t s_version = 0U;
uint16_t s_granularity_ms = 0U;

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    for (size_t i = 0U; i < size; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

// 与 Windows 侧车生成器使用完全相同的四段 512 字节抽样规则。
static esp_err_t calculate_source_crc32(VideoMediaIo::Context *io, uint32_t *out_crc)
{
    if (io == nullptr || io->file == nullptr || out_crc == nullptr || io->size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t offsets[4] = {
        0U,
        io->size / 3U,
        static_cast<uint32_t>((static_cast<uint64_t>(io->size) * 2ULL) / 3ULL),
        io->size > kFingerprintSampleBytes ? io->size - kFingerprintSampleBytes : 0U,
    };

    uint32_t crc = 0xFFFFFFFFU;
    uint8_t buffer[kFingerprintReadBytes] = {};
    for (uint8_t sample = 0U; sample < 4U; ++sample) {
        const uint32_t offset = offsets[sample];
        if (offset >= io->size) return ESP_ERR_INVALID_SIZE;
        const uint32_t sample_size =
            (io->size - offset) < kFingerprintSampleBytes
                ? (io->size - offset)
                : kFingerprintSampleBytes;

        uint32_t consumed = 0U;
        while (consumed < sample_size) {
            const uint32_t remaining = sample_size - consumed;
            const uint32_t request = remaining > sizeof(buffer)
                ? static_cast<uint32_t>(sizeof(buffer))
                : remaining;
            if (VideoMediaIo::seek_cb(offset + consumed, io) < 0) {
                return ESP_FAIL;
            }
            const int got = VideoMediaIo::read_cb(buffer, request, io);
            if (got != static_cast<int>(request)) {
                return ESP_FAIL;
            }
            crc = crc32_update(crc, buffer, request);
            consumed += request;
        }
    }

    *out_crc = crc ^ 0xFFFFFFFFU;
    return ESP_OK;
}

static bool validate_entry(const Entry &entry, const Entry *previous,
    uint32_t source_size, uint32_t source_fps_x100, uint32_t index)
{
    if (entry.file_offset > source_size || source_size - entry.file_offset < 8U) {
        return false;
    }
    if (entry.audio_bytes_before > source_size) return false;
    if (source_fps_x100 == 0U) return false;
    const uint64_t expected_timestamp =
        static_cast<uint64_t>(entry.video_frame_index) * 100000ULL / source_fps_x100;
    if (expected_timestamp > UINT32_MAX ||
        entry.timestamp_ms != static_cast<uint32_t>(expected_timestamp)) {
        return false;
    }
    if (index == 0U) return true;
    if (previous == nullptr) return false;
    return entry.timestamp_ms > previous->timestamp_ms &&
        entry.file_offset > previous->file_offset &&
        entry.video_frame_index > previous->video_frame_index &&
        entry.audio_bytes_before >= previous->audio_bytes_before;
}

} // namespace

esp_err_t load(const char *sidecar_path, VideoMediaIo::Context *source_io,
    uint32_t source_fps_x100)
{
    unload();
    if (sidecar_path == nullptr || source_io == nullptr || source_io->file == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(sidecar_path, "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "侧车索引不存在: %s", sidecar_path);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    const long sidecar_size_long = ftell(f);
    if (sidecar_size_long < static_cast<long>(sizeof(HeaderV1)) || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t sidecar_size = static_cast<uint64_t>(sidecar_size_long);

    HeaderV1 hdr = {};
    if (fread(&hdr, sizeof(hdr), 1U, f) != 1U) {
        fclose(f);
        ESP_LOGE(TAG, "读取索引头失败");
        return ESP_FAIL;
    }
    if (memcmp(hdr.magic, kMagic, sizeof(kMagic)) != 0) {
        fclose(f);
        ESP_LOGE(TAG, "索引魔数不匹配");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (hdr.version != kVersionV1 && hdr.version != kVersionV2) {
        fclose(f);
        ESP_LOGE(TAG, "索引版本不支持: %u", static_cast<unsigned>(hdr.version));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (hdr.granularity_ms == 0U || hdr.granularity_ms > 60000U ||
        hdr.entry_count == 0U || hdr.entry_count > kMaxEntryCount) {
        fclose(f);
        ESP_LOGE(TAG, "索引参数非法: granularity=%u entries=%u",
            static_cast<unsigned>(hdr.granularity_ms),
            static_cast<unsigned>(hdr.entry_count));
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t disk_entry_size = hdr.version == kVersionV2
        ? static_cast<uint32_t>(sizeof(Entry))
        : static_cast<uint32_t>(sizeof(EntryV1));
    const uint32_t disk_header_size = hdr.version == kVersionV2
        ? static_cast<uint32_t>(sizeof(HeaderV1) + sizeof(HeaderV2Extra))
        : static_cast<uint32_t>(sizeof(HeaderV1));
    const uint64_t expected_size = static_cast<uint64_t>(disk_header_size) +
        static_cast<uint64_t>(hdr.entry_count) * disk_entry_size;
    if (sidecar_size != expected_size) {
        fclose(f);
        ESP_LOGE(TAG, "索引文件长度不匹配: actual=%llu expected=%llu",
            static_cast<unsigned long long>(sidecar_size),
            static_cast<unsigned long long>(expected_size));
        return ESP_ERR_INVALID_SIZE;
    }

    if (hdr.version == kVersionV2) {
        HeaderV2Extra extra = {};
        if (fread(&extra, sizeof(extra), 1U, f) != 1U) {
            fclose(f);
            return ESP_FAIL;
        }
        if (extra.source_size != source_io->size) {
            fclose(f);
            ESP_LOGE(TAG, "侧车索引来源文件长度不一致: index=%lu avi=%lu",
                static_cast<unsigned long>(extra.source_size),
                static_cast<unsigned long>(source_io->size));
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint32_t actual_crc32 = 0U;
        const esp_err_t crc_ret = calculate_source_crc32(source_io, &actual_crc32);
        if (crc_ret != ESP_OK) {
            fclose(f);
            ESP_LOGE(TAG, "计算 AVI 抽样指纹失败: %s", esp_err_to_name(crc_ret));
            return crc_ret;
        }
        if (actual_crc32 != extra.source_crc32) {
            fclose(f);
            ESP_LOGE(TAG, "侧车索引来源指纹不一致: index=%08lx avi=%08lx",
                static_cast<unsigned long>(extra.source_crc32),
                static_cast<unsigned long>(actual_crc32));
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    s_entries = static_cast<Entry *>(heap_caps_calloc(
        hdr.entry_count, sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_entries == nullptr) {
        fclose(f);
        ESP_LOGE(TAG, "分配索引内存失败: %u entries", static_cast<unsigned>(hdr.entry_count));
        return ESP_ERR_NO_MEM;
    }

    bool entries_valid = true;
    for (uint32_t i = 0U; i < hdr.entry_count; ++i) {
        Entry current = {};
        if (hdr.version == kVersionV2) {
            if (fread(&current, sizeof(current), 1U, f) != 1U) {
                entries_valid = false;
                break;
            }
        } else {
            EntryV1 legacy = {};
            if (fread(&legacy, sizeof(legacy), 1U, f) != 1U) {
                entries_valid = false;
                break;
            }
            current.timestamp_ms = legacy.timestamp_ms;
            current.file_offset = legacy.file_offset;
            current.video_frame_index = 0U;
            current.audio_bytes_before = 0U;
        }

        if (current.file_offset > source_io->size || source_io->size - current.file_offset < 8U) {
            entries_valid = false;
            break;
        }
        if (hdr.version == kVersionV2 &&
            !validate_entry(current, i > 0U ? &s_entries[i - 1U] : nullptr,
                source_io->size, source_fps_x100, i)) {
            entries_valid = false;
            break;
        }
        if (hdr.version == kVersionV1 && i > 0U &&
            (current.timestamp_ms <= s_entries[i - 1U].timestamp_ms ||
             current.file_offset <= s_entries[i - 1U].file_offset)) {
            entries_valid = false;
            break;
        }
        s_entries[i] = current;
    }

    fclose(f);
    if (!entries_valid) {
        ESP_LOGE(TAG, "索引条目无效或顺序错误");
        unload();
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_count = hdr.entry_count;
    s_version = hdr.version;
    s_granularity_ms = hdr.granularity_ms;
    if (s_version == kVersionV1) {
        ESP_LOGW(TAG, "加载旧版 FVIX V1：未验证来源指纹，音频累计字节数仍使用平均码率估算");
    }
    ESP_LOGI(TAG, "加载侧车索引: V%u, %u 条目, 粒度 %ums, 内存=%uB, 文件=%s",
        static_cast<unsigned>(s_version),
        static_cast<unsigned>(s_count),
        static_cast<unsigned>(s_granularity_ms),
        static_cast<unsigned>(s_count * sizeof(Entry)),
        sidecar_path);
    return ESP_OK;
}

esp_err_t lookup(uint32_t target_ms, Entry *out_entry)
{
    if (s_entries == nullptr || s_count == 0U || out_entry == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t lo = 0U;
    uint32_t hi = s_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2U;
        if (s_entries[mid].timestamp_ms <= target_ms) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    *out_entry = lo == 0U ? s_entries[0] : s_entries[lo - 1U];
    return ESP_OK;
}

uint32_t count() { return s_count; }

uint32_t duration_ms()
{
    if (s_entries == nullptr || s_count == 0U) return 0U;
    return s_entries[s_count - 1U].timestamp_ms;
}

bool has_audio_byte_anchor() { return s_version >= kVersionV2; }

bool has_source_fingerprint() { return s_version >= kVersionV2; }

void unload()
{
    if (s_entries != nullptr) {
        heap_caps_free(s_entries);
        s_entries = nullptr;
    }
    s_count = 0U;
    s_version = 0U;
    s_granularity_ms = 0U;
}

} // namespace VideoSeekIndex
