#include "audio_source.h"

static bool audio_source_has_ops(const AudioSource *source)
{
    return source != nullptr && source->ops != nullptr && source->context != nullptr;
}

bool audio_source_is_open(const AudioSource *source)
{
    return audio_source_has_ops(source);
}

bool audio_source_has_capability(const AudioSource *source, AudioSourceCapability capability)
{
    return audio_source_has_ops(source) &&
        (source->capabilities & static_cast<uint32_t>(capability)) != 0U;
}

esp_err_t audio_source_read(
    AudioSource *source,
    void *buffer,
    size_t bytes,
    size_t *out_bytes)
{
    if (out_bytes != nullptr) {
        *out_bytes = 0;
    }
    if (!audio_source_has_ops(source) || source->ops->read == nullptr ||
        buffer == nullptr || out_bytes == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_source_has_capability(source, AUDIO_SOURCE_CAP_READ)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (bytes == 0) {
        return ESP_OK;
    }

    const esp_err_t ret = source->ops->read(source->context, buffer, bytes, out_bytes);
    ++source->stats.read_calls;
    source->stats.bytes_read += *out_bytes;
    return ret;
}

esp_err_t audio_source_seek(
    AudioSource *source,
    int64_t offset,
    AudioSourceSeekOrigin origin)
{
    if (!audio_source_has_ops(source) || source->ops->seek == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_source_has_capability(source, AUDIO_SOURCE_CAP_SEEK)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_err_t ret = source->ops->seek(source->context, offset, origin);
    ++source->stats.seek_calls;
    return ret;
}

esp_err_t audio_source_tell(AudioSource *source, uint64_t *out_position)
{
    if (out_position != nullptr) {
        *out_position = 0;
    }
    if (!audio_source_has_ops(source) || source->ops->tell == nullptr || out_position == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_source_has_capability(source, AUDIO_SOURCE_CAP_TELL)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_err_t ret = source->ops->tell(source->context, out_position);
    ++source->stats.tell_calls;
    return ret;
}

esp_err_t audio_source_size(AudioSource *source, uint64_t *out_size)
{
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (!audio_source_has_ops(source) || source->ops->size == nullptr || out_size == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_source_has_capability(source, AUDIO_SOURCE_CAP_SIZE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return source->ops->size(source->context, out_size);
}

bool audio_source_eof(const AudioSource *source)
{
    if (!audio_source_has_ops(source) || source->ops->eof == nullptr ||
        !audio_source_has_capability(source, AUDIO_SOURCE_CAP_EOF)) {
        return false;
    }
    return source->ops->eof(source->context);
}

const char *audio_source_name(const AudioSource *source)
{
    if (!audio_source_has_ops(source) || source->ops->name == nullptr) {
        return "NONE";
    }
    const char *name = source->ops->name(source->context);
    return name != nullptr ? name : "UNKNOWN";
}

const AudioSourceStats *audio_source_stats(const AudioSource *source)
{
    return source != nullptr ? &source->stats : nullptr;
}

esp_err_t audio_source_close(AudioSource *source)
{
    if (source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    if (audio_source_has_ops(source) && source->ops->close != nullptr) {
        ret = source->ops->close(source->context);
    }
    *source = {};
    return ret;
}
