#include "media_index_store.h"
#include "storage_io.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "system_paths.h"

static const char *TAG = "音乐索引";
static constexpr uint16_t MEDIA_INDEX_VERSION = 1;
static constexpr uint16_t MEDIA_MANIFEST_VERSION = 1;
static constexpr uint32_t MEDIA_SIGNATURE_MODE_FAST = 1;
static constexpr size_t MAX_INDEX_RECORDS = 100000;
static constexpr size_t MAX_PATH_POOL_BYTES = 32 * 1024 * 1024;

#pragma pack(push, 1)
struct MediaIndexFileHeader
{
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint16_t record_size;
    uint16_t reserved0;
    uint32_t record_count;
    uint32_t path_pool_size;
    uint32_t payload_crc32;
    uint32_t signature_mode;
};

struct MediaIndexDiskRecord
{
    uint32_t path_offset;
    uint8_t format;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t reserved0;
    uint32_t technical_flags;
    uint64_t file_size_bytes;
    int64_t modified_time;
    uint32_t sample_rate_hz;
    uint32_t bitrate_kbps;
    uint32_t duration_ms;
    uint64_t total_frames;
    uint64_t audio_data_offset;
    uint64_t metadata_end_offset;
    uint64_t artwork_offset;
    uint32_t artwork_size;
    uint32_t max_frame_size;
    uint16_t max_block_size;
    uint16_t samples_per_frame;
};

struct MediaManifestFileHeader
{
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint16_t record_size;
    uint16_t reserved0;
    uint32_t record_count;
    uint32_t path_pool_size;
    uint32_t payload_crc32;
    uint32_t index_payload_crc32;
    uint32_t signature_mode;
};

struct MediaManifestDiskRecord
{
    uint32_t path_offset;
    uint8_t format;
    uint8_t reserved[3];
    uint64_t file_size_bytes;
    int64_t modified_time;
};
#pragma pack(pop)

static_assert(sizeof(MediaIndexFileHeader) == 32, "MediaIndexFileHeader layout changed");
static_assert(sizeof(MediaIndexDiskRecord) == 84, "MediaIndexDiskRecord layout changed");
static_assert(sizeof(MediaManifestFileHeader) == 36, "MediaManifestFileHeader layout changed");
static_assert(sizeof(MediaManifestDiskRecord) == 24, "MediaManifestDiskRecord layout changed");

static uint32_t crc32_update(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

static uint32_t crc32_begin()
{
    return 0xFFFFFFFFU;
}

static uint32_t crc32_end(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFU;
}

static void *index_psram_alloc(size_t bytes)
{
    if (bytes == 0) {
        return nullptr;
    }
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool ensure_system_directory()
{
    struct stat info = {};
    if (stat(SystemPaths::kSystemDirectory, &info) == 0) {
        return S_ISDIR(info.st_mode);
    }
    if (mkdir(SystemPaths::kSystemDirectory, 0775) == 0 || errno == EEXIST) {
        return true;
    }
    ESP_LOGE(TAG, "创建系统目录失败：%s errno=%d", SystemPaths::kSystemDirectory, errno);
    return false;
}

static MediaIndexDiskRecord to_disk_record(const MediaIndexRecord &source)
{
    MediaIndexDiskRecord record = {};
    record.path_offset = source.path_offset;
    record.format = static_cast<uint8_t>(source.format);
    record.channels = source.technical.channels;
    record.bits_per_sample = source.technical.bits_per_sample;
    record.technical_flags = source.technical.flags;
    record.file_size_bytes = source.file_size_bytes;
    record.modified_time = source.modified_time;
    record.sample_rate_hz = source.technical.sample_rate_hz;
    record.bitrate_kbps = source.technical.bitrate_kbps;
    record.duration_ms = source.technical.duration_ms;
    record.total_frames = source.technical.total_frames;
    record.audio_data_offset = source.technical.audio_data_offset;
    record.metadata_end_offset = source.technical.metadata_end_offset;
    record.artwork_offset = source.technical.artwork_offset;
    record.artwork_size = source.technical.artwork_size;
    record.max_frame_size = source.technical.max_frame_size;
    record.max_block_size = source.technical.max_block_size;
    record.samples_per_frame = source.technical.samples_per_frame;
    return record;
}

static MediaIndexRecord from_disk_record(const MediaIndexDiskRecord &source)
{
    MediaIndexRecord record = {};
    record.path_offset = source.path_offset;
    record.format = static_cast<MediaFormat>(source.format);
    record.file_size_bytes = source.file_size_bytes;
    record.modified_time = source.modified_time;
    record.technical.flags = source.technical_flags;
    record.technical.sample_rate_hz = source.sample_rate_hz;
    record.technical.bitrate_kbps = source.bitrate_kbps;
    record.technical.duration_ms = source.duration_ms;
    record.technical.total_frames = source.total_frames;
    record.technical.audio_data_offset = source.audio_data_offset;
    record.technical.metadata_end_offset = source.metadata_end_offset;
    record.technical.artwork_offset = source.artwork_offset;
    record.technical.artwork_size = source.artwork_size;
    record.technical.max_frame_size = source.max_frame_size;
    record.technical.max_block_size = source.max_block_size;
    record.technical.samples_per_frame = source.samples_per_frame;
    record.technical.channels = source.channels;
    record.technical.bits_per_sample = source.bits_per_sample;
    return record;
}

static uint32_t calculate_index_payload_crc(
    const MediaIndexRecord *records,
    size_t record_count,
    const char *path_pool,
    size_t path_pool_size
)
{
    uint32_t crc = crc32_begin();
    for (size_t i = 0; i < record_count; ++i) {
        const MediaIndexDiskRecord disk = to_disk_record(records[i]);
        crc = crc32_update(crc, &disk, sizeof(disk));
    }
    if (path_pool_size > 0) {
        crc = crc32_update(crc, path_pool, path_pool_size);
    }
    return crc32_end(crc);
}

static uint32_t calculate_manifest_payload_crc(
    const MediaIndexRecord *records,
    size_t record_count,
    const char *path_pool,
    size_t path_pool_size
)
{
    uint32_t crc = crc32_begin();
    for (size_t i = 0; i < record_count; ++i) {
        MediaManifestDiskRecord disk = {};
        disk.path_offset = records[i].path_offset;
        disk.format = static_cast<uint8_t>(records[i].format);
        disk.file_size_bytes = records[i].file_size_bytes;
        disk.modified_time = records[i].modified_time;
        crc = crc32_update(crc, &disk, sizeof(disk));
    }
    if (path_pool_size > 0) {
        crc = crc32_update(crc, path_pool, path_pool_size);
    }
    return crc32_end(crc);
}

static bool flush_file(FILE *file)
{
    if (file == nullptr || fflush(file) != 0) {
        return false;
    }
    const int descriptor = fileno(file);
    return descriptor < 0 || fsync(descriptor) == 0;
}

static esp_err_t write_index_file(
    const char *path,
    const MediaIndexRecord *records,
    size_t record_count,
    const char *path_pool,
    size_t path_pool_size,
    uint32_t payload_crc
)
{
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        return ESP_FAIL;
    }

    MediaIndexFileHeader header = {};
    memcpy(header.magic, "FPIDX01", 7);
    header.version = MEDIA_INDEX_VERSION;
    header.header_size = sizeof(header);
    header.record_size = sizeof(MediaIndexDiskRecord);
    header.record_count = static_cast<uint32_t>(record_count);
    header.path_pool_size = static_cast<uint32_t>(path_pool_size);
    header.payload_crc32 = payload_crc;
    header.signature_mode = MEDIA_SIGNATURE_MODE_FAST;

    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    for (size_t i = 0; ok && i < record_count; ++i) {
        const MediaIndexDiskRecord disk = to_disk_record(records[i]);
        ok = fwrite(&disk, 1, sizeof(disk), file) == sizeof(disk);
    }
    if (ok && path_pool_size > 0) {
        ok = fwrite(path_pool, 1, path_pool_size, file) == path_pool_size;
    }
    if (ok) {
        ok = flush_file(file);
    }
    fclose(file);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_manifest_file(
    const char *path,
    const MediaIndexRecord *records,
    size_t record_count,
    const char *path_pool,
    size_t path_pool_size,
    uint32_t payload_crc,
    uint32_t index_payload_crc
)
{
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        return ESP_FAIL;
    }

    MediaManifestFileHeader header = {};
    memcpy(header.magic, "FPMNF01", 7);
    header.version = MEDIA_MANIFEST_VERSION;
    header.header_size = sizeof(header);
    header.record_size = sizeof(MediaManifestDiskRecord);
    header.record_count = static_cast<uint32_t>(record_count);
    header.path_pool_size = static_cast<uint32_t>(path_pool_size);
    header.payload_crc32 = payload_crc;
    header.index_payload_crc32 = index_payload_crc;
    header.signature_mode = MEDIA_SIGNATURE_MODE_FAST;

    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    for (size_t i = 0; ok && i < record_count; ++i) {
        MediaManifestDiskRecord disk = {};
        disk.path_offset = records[i].path_offset;
        disk.format = static_cast<uint8_t>(records[i].format);
        disk.file_size_bytes = records[i].file_size_bytes;
        disk.modified_time = records[i].modified_time;
        ok = fwrite(&disk, 1, sizeof(disk), file) == sizeof(disk);
    }
    if (ok && path_pool_size > 0) {
        ok = fwrite(path_pool, 1, path_pool_size, file) == path_pool_size;
    }
    if (ok) {
        ok = flush_file(file);
    }
    fclose(file);
    return ok ? ESP_OK : ESP_FAIL;
}

static bool path_pool_record_valid(const char *pool, size_t pool_size, uint32_t offset)
{
    if (pool == nullptr || offset >= pool_size) {
        return false;
    }
    return memchr(pool + offset, '\0', pool_size - offset) != nullptr;
}

static esp_err_t load_index_file(const char *path, MediaIndexSnapshot *snapshot)
{
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    MediaIndexFileHeader header = {};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header.magic, "FPIDX01", 7) != 0 ||
        header.version != MEDIA_INDEX_VERSION ||
        header.header_size != sizeof(MediaIndexFileHeader) ||
        header.record_size != sizeof(MediaIndexDiskRecord) ||
        header.record_count > MAX_INDEX_RECORDS ||
        header.path_pool_size > MAX_PATH_POOL_BYTES ||
        header.signature_mode != MEDIA_SIGNATURE_MODE_FAST) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    MediaIndexRecord *records = nullptr;
    char *path_pool = nullptr;
    if (header.record_count > 0) {
        records = static_cast<MediaIndexRecord *>(
            index_psram_alloc(static_cast<size_t>(header.record_count) * sizeof(MediaIndexRecord))
        );
        if (records == nullptr) {
            fclose(file);
            return ESP_ERR_NO_MEM;
        }
    }
    if (header.path_pool_size > 0) {
        path_pool = static_cast<char *>(index_psram_alloc(header.path_pool_size));
        if (path_pool == nullptr) {
            heap_caps_free(records);
            fclose(file);
            return ESP_ERR_NO_MEM;
        }
    }

    uint32_t crc = crc32_begin();
    bool ok = true;
    for (uint32_t i = 0; i < header.record_count; ++i) {
        MediaIndexDiskRecord disk = {};
        if (fread(&disk, 1, sizeof(disk), file) != sizeof(disk)) {
            ok = false;
            break;
        }
        crc = crc32_update(crc, &disk, sizeof(disk));
        records[i] = from_disk_record(disk);
    }
    if (ok && header.path_pool_size > 0) {
        ok = fread(path_pool, 1, header.path_pool_size, file) == header.path_pool_size;
        if (ok) {
            crc = crc32_update(crc, path_pool, header.path_pool_size);
        }
    }
    fclose(file);

    crc = crc32_end(crc);
    if (!ok || crc != header.payload_crc32) {
        heap_caps_free(records);
        heap_caps_free(path_pool);
        return ESP_ERR_INVALID_CRC;
    }

    for (uint32_t i = 0; i < header.record_count; ++i) {
        if (!path_pool_record_valid(path_pool, header.path_pool_size, records[i].path_offset)) {
            heap_caps_free(records);
            heap_caps_free(path_pool);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    snapshot->records = records;
    snapshot->record_count = header.record_count;
    snapshot->path_pool = path_pool;
    snapshot->path_pool_size = header.path_pool_size;
    snapshot->payload_crc32 = header.payload_crc32;
    return ESP_OK;
}

void media_index_store_release(MediaIndexSnapshot *snapshot)
{
    if (snapshot == nullptr) {
        return;
    }
    heap_caps_free(snapshot->records);
    heap_caps_free(snapshot->path_pool);
    *snapshot = {};
}

esp_err_t media_index_store_load(MediaIndexSnapshot *snapshot)
{
    if (snapshot == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    media_index_store_release(snapshot);

    StorageSdLockGuard sd_lock;
    if (!sd_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = load_index_file(SystemPaths::kMusicIndex, snapshot);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "已加载旧索引：%u 首，CRC=0x%08lX",
            static_cast<unsigned>(snapshot->record_count),
            static_cast<unsigned long>(snapshot->payload_crc32));
        return ESP_OK;
    }

    MediaIndexSnapshot backup = {};
    const esp_err_t backup_ret = load_index_file(SystemPaths::kMusicIndexBackup, &backup);
    if (backup_ret == ESP_OK) {
        backup.loaded_from_backup = true;
        *snapshot = backup;
        ESP_LOGW(TAG, "主索引不可用(%s)，已从 .bak 恢复：%u 首",
            esp_err_to_name(ret),
            static_cast<unsigned>(snapshot->record_count));
        return ESP_OK;
    }

    if (ret != ESP_ERR_NOT_FOUND || backup_ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "没有可复用的有效旧索引：final=%s backup=%s",
            esp_err_to_name(ret),
            esp_err_to_name(backup_ret));
    }
    return ESP_ERR_NOT_FOUND;
}

const MediaIndexRecord *media_index_store_find(
    const MediaIndexSnapshot *snapshot,
    const char *path
)
{
    if (snapshot == nullptr || path == nullptr || snapshot->record_count == 0 ||
        snapshot->records == nullptr || snapshot->path_pool == nullptr) {
        return nullptr;
    }

    size_t low = 0;
    size_t high = snapshot->record_count;
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        const MediaIndexRecord &candidate = snapshot->records[mid];
        if (!path_pool_record_valid(snapshot->path_pool, snapshot->path_pool_size, candidate.path_offset)) {
            return nullptr;
        }
        const char *candidate_path = snapshot->path_pool + candidate.path_offset;
        const int comparison = strcasecmp(path, candidate_path);
        if (comparison == 0) {
            return &candidate;
        }
        if (comparison < 0) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return nullptr;
}

static esp_err_t validate_manifest_file(const char *path, uint32_t expected_index_crc)
{
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    MediaManifestFileHeader header = {};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header.magic, "FPMNF01", 7) != 0 ||
        header.version != MEDIA_MANIFEST_VERSION ||
        header.header_size != sizeof(MediaManifestFileHeader) ||
        header.record_size != sizeof(MediaManifestDiskRecord) ||
        header.record_count > MAX_INDEX_RECORDS ||
        header.path_pool_size > MAX_PATH_POOL_BYTES ||
        header.index_payload_crc32 != expected_index_crc ||
        header.signature_mode != MEDIA_SIGNATURE_MODE_FAST) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t crc = crc32_begin();
    MediaManifestDiskRecord record = {};
    for (uint32_t i = 0; i < header.record_count; ++i) {
        if (fread(&record, 1, sizeof(record), file) != sizeof(record)) {
            fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
        crc = crc32_update(crc, &record, sizeof(record));
    }

    uint8_t buffer[256] = {};
    uint32_t remaining = header.path_pool_size;
    while (remaining > 0) {
        const size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        if (fread(buffer, 1, chunk, file) != chunk) {
            fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }
        crc = crc32_update(crc, buffer, chunk);
        remaining -= static_cast<uint32_t>(chunk);
    }
    fclose(file);
    return crc32_end(crc) == header.payload_crc32 ? ESP_OK : ESP_ERR_INVALID_CRC;
}

static esp_err_t rotate_atomic_file(const char *temp_path, const char *final_path, const char *backup_path)
{
    remove(backup_path);
    if (rename(final_path, backup_path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "旧文件转 bak 失败：%s errno=%d", final_path, errno);
    }
    if (rename(temp_path, final_path) != 0) {
        ESP_LOGE(TAG, "tmp 提升 final 失败：%s -> %s errno=%d", temp_path, final_path, errno);
        // 尽量恢复旧版本；失败时仍保留 bak 供下一次启动读取。
        rename(backup_path, final_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t media_index_store_commit(
    const MediaIndexRecord *records,
    size_t record_count,
    const char *path_pool,
    size_t path_pool_size,
    const MediaIndexSnapshot *previous_snapshot
)
{
    if ((record_count > 0 && records == nullptr) ||
        (path_pool_size > 0 && path_pool == nullptr) ||
        record_count > UINT32_MAX || path_pool_size > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t index_crc = calculate_index_payload_crc(
        records, record_count, path_pool, path_pool_size
    );
    const uint32_t manifest_crc = calculate_manifest_payload_crc(
        records, record_count, path_pool, path_pool_size
    );

    // CRC/去重计算不占用 SD 锁；只有真正文件系统事务进入全局串行通道。
    StorageSdLockGuard sd_lock;
    if (!sd_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    if (!ensure_system_directory()) {
        return ESP_FAIL;
    }

    // 当前内容与已经完整校验过的 final 索引一致时，不重复写 TF 卡。
    // 若上一轮是从 .bak 恢复，则仍重新提交一次，把 final 修复回来。
    if (
        previous_snapshot != nullptr &&
        !previous_snapshot->loaded_from_backup &&
        previous_snapshot->payload_crc32 == index_crc &&
        validate_manifest_file(SystemPaths::kMusicManifest, index_crc) == ESP_OK
    ) {
        ESP_LOGI(TAG, "索引内容未变化，跳过写盘：CRC=0x%08lX",
            static_cast<unsigned long>(index_crc));
        return ESP_OK;
    }

    remove(SystemPaths::kMusicIndexTemp);
    remove(SystemPaths::kMusicManifestTemp);

    esp_err_t ret = write_index_file(
        SystemPaths::kMusicIndexTemp,
        records,
        record_count,
        path_pool,
        path_pool_size,
        index_crc
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "写入索引 tmp 失败");
        return ret;
    }

    ret = write_manifest_file(
        SystemPaths::kMusicManifestTemp,
        records,
        record_count,
        path_pool,
        path_pool_size,
        manifest_crc,
        index_crc
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "写入 manifest tmp 失败");
        remove(SystemPaths::kMusicIndexTemp);
        return ret;
    }

    MediaIndexSnapshot verify_index = {};
    ret = load_index_file(SystemPaths::kMusicIndexTemp, &verify_index);
    media_index_store_release(&verify_index);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "索引 tmp 校验失败：%s", esp_err_to_name(ret));
        remove(SystemPaths::kMusicIndexTemp);
        remove(SystemPaths::kMusicManifestTemp);
        return ret;
    }
    ret = validate_manifest_file(SystemPaths::kMusicManifestTemp, index_crc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "manifest tmp 校验失败：%s", esp_err_to_name(ret));
        remove(SystemPaths::kMusicIndexTemp);
        remove(SystemPaths::kMusicManifestTemp);
        return ret;
    }

    ret = rotate_atomic_file(
        SystemPaths::kMusicIndexTemp,
        SystemPaths::kMusicIndex,
        SystemPaths::kMusicIndexBackup
    );
    if (ret != ESP_OK) {
        remove(SystemPaths::kMusicManifestTemp);
        return ret;
    }
    ret = rotate_atomic_file(
        SystemPaths::kMusicManifestTemp,
        SystemPaths::kMusicManifest,
        SystemPaths::kMusicManifestBackup
    );
    if (ret != ESP_OK) {
        // pair 提交失败时回滚刚刚提升的 index，避免留下新旧不匹配的一对文件。
        remove(SystemPaths::kMusicIndex);
        rename(SystemPaths::kMusicIndexBackup, SystemPaths::kMusicIndex);
        return ret;
    }

    // final 落盘后再次读取校验。任何一边失败都回滚到上一对 bak。
    MediaIndexSnapshot final_index = {};
    const esp_err_t final_index_ret = load_index_file(SystemPaths::kMusicIndex, &final_index);
    media_index_store_release(&final_index);
    const esp_err_t final_manifest_ret = validate_manifest_file(
        SystemPaths::kMusicManifest,
        index_crc
    );
    if (final_index_ret != ESP_OK || final_manifest_ret != ESP_OK) {
        ESP_LOGE(TAG, "final 二次校验失败：index=%s manifest=%s，尝试回滚 bak",
            esp_err_to_name(final_index_ret),
            esp_err_to_name(final_manifest_ret));
        remove(SystemPaths::kMusicIndex);
        remove(SystemPaths::kMusicManifest);
        rename(SystemPaths::kMusicIndexBackup, SystemPaths::kMusicIndex);
        rename(SystemPaths::kMusicManifestBackup, SystemPaths::kMusicManifest);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "索引事务提交完成：歌曲=%u 路径池=%uB index_crc=0x%08lX manifest_crc=0x%08lX",
        static_cast<unsigned>(record_count),
        static_cast<unsigned>(path_pool_size),
        static_cast<unsigned long>(index_crc),
        static_cast<unsigned long>(manifest_crc));
    return ESP_OK;
}
