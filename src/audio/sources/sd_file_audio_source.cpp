#include "sd_file_audio_source.h"

#include <limits.h>
#include "esp_log.h"

static const char *TAG = "音频源";

static esp_err_t sd_file_read(void *context, void *buffer, size_t bytes, size_t *out_bytes)
{
    SdFileAudioSource *source = static_cast<SdFileAudioSource *>(context);
    if (out_bytes != nullptr) {
        *out_bytes = 0;
    }
    if (source == nullptr || source->file == nullptr || buffer == nullptr || out_bytes == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_bytes = fread(buffer, 1, bytes, source->file);
    if (*out_bytes < bytes && ferror(source->file)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t sd_file_seek(void *context, int64_t offset, AudioSourceSeekOrigin origin)
{
    SdFileAudioSource *source = static_cast<SdFileAudioSource *>(context);
    if (source == nullptr || source->file == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (offset < static_cast<int64_t>(LONG_MIN) || offset > static_cast<int64_t>(LONG_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int whence = SEEK_SET;
    switch (origin) {
        case AudioSourceSeekOrigin::Begin: whence = SEEK_SET; break;
        case AudioSourceSeekOrigin::Current: whence = SEEK_CUR; break;
        case AudioSourceSeekOrigin::End: whence = SEEK_END; break;
        default: return ESP_ERR_INVALID_ARG;
    }

    if (fseek(source->file, static_cast<long>(offset), whence) != 0) {
        return ESP_FAIL;
    }
    // fseek 会清 EOF；同时清掉旧 I/O error，确保后续 seek/retry 从干净状态继续。
    clearerr(source->file);
    return ESP_OK;
}

static esp_err_t sd_file_tell(void *context, uint64_t *out_position)
{
    SdFileAudioSource *source = static_cast<SdFileAudioSource *>(context);
    if (source == nullptr || source->file == nullptr || out_position == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const long position = ftell(source->file);
    if (position < 0) {
        return ESP_FAIL;
    }
    *out_position = static_cast<uint64_t>(position);
    return ESP_OK;
}

static esp_err_t sd_file_size(void *context, uint64_t *out_size)
{
    SdFileAudioSource *source = static_cast<SdFileAudioSource *>(context);
    if (source == nullptr || source->file == nullptr || out_size == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_size = source->size_bytes;
    return ESP_OK;
}

static bool sd_file_eof(void *context)
{
    SdFileAudioSource *source = static_cast<SdFileAudioSource *>(context);
    return source != nullptr && source->file != nullptr && feof(source->file) != 0;
}

static esp_err_t sd_file_close(void *context)
{
    SdFileAudioSource *source = static_cast<SdFileAudioSource *>(context);
    if (source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (source->file != nullptr) {
        fclose(source->file);
    }
    *source = {};
    return ESP_OK;
}

static const char *sd_file_name(void *)
{
    return "SD_FILE";
}

static const AudioSourceOps SD_FILE_OPS = {
    sd_file_read,
    sd_file_seek,
    sd_file_tell,
    sd_file_size,
    sd_file_eof,
    sd_file_close,
    sd_file_name,
};

esp_err_t sd_file_audio_source_open(
    AudioSource *out_source,
    SdFileAudioSource *storage,
    const char *path)
{
    if (out_source == nullptr || storage == nullptr || path == nullptr || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    audio_source_close(out_source);
    *storage = {};

    storage->file = fopen(path, "rb");
    if (storage->file == nullptr) {
        ESP_LOGE(TAG, "打开 SD 音频源失败：%s", path);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(storage->file, 0, SEEK_END) != 0) {
        fclose(storage->file);
        *storage = {};
        return ESP_FAIL;
    }
    const long end = ftell(storage->file);
    if (end <= 0 || fseek(storage->file, 0, SEEK_SET) != 0) {
        fclose(storage->file);
        *storage = {};
        return ESP_ERR_INVALID_SIZE;
    }
    clearerr(storage->file);
    storage->size_bytes = static_cast<uint64_t>(end);

    out_source->ops = &SD_FILE_OPS;
    out_source->context = storage;
    out_source->capabilities =
        AUDIO_SOURCE_CAP_READ |
        AUDIO_SOURCE_CAP_SEEK |
        AUDIO_SOURCE_CAP_TELL |
        AUDIO_SOURCE_CAP_SIZE |
        AUDIO_SOURCE_CAP_EOF |
        AUDIO_SOURCE_CAP_RANDOM_ACCESS;
    out_source->stats = {};

    ESP_LOGI(TAG,
        "SOURCE_TRACE: OPEN type=SD_FILE size=%llu caps=READ|SEEK|TELL|SIZE|EOF|RANDOM path=%s",
        static_cast<unsigned long long>(storage->size_bytes),
        path);
    return ESP_OK;
}
