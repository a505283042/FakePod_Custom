#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// AudioSource 是 Codec 与底层 I/O 之间的统一“数据插座”。
// Decoder 只依赖 read/seek/tell/size/eof，不关心字节来自 TF、HTTP Range 还是未来的网络流。
enum AudioSourceCapability : uint32_t
{
    AUDIO_SOURCE_CAP_READ = 1U << 0,
    AUDIO_SOURCE_CAP_SEEK = 1U << 1,
    AUDIO_SOURCE_CAP_TELL = 1U << 2,
    AUDIO_SOURCE_CAP_SIZE = 1U << 3,
    AUDIO_SOURCE_CAP_EOF = 1U << 4,
    AUDIO_SOURCE_CAP_RANDOM_ACCESS = 1U << 5,
    AUDIO_SOURCE_CAP_STREAMING = 1U << 6,
};

enum class AudioSourceSeekOrigin : uint8_t
{
    Begin = 0,
    Current,
    End,
};

struct AudioSourceStats
{
    uint64_t bytes_read = 0;
    uint32_t read_calls = 0;
    uint32_t seek_calls = 0;
    uint32_t tell_calls = 0;
};

struct AudioSource;

struct AudioSourceOps
{
    esp_err_t (*read)(void *context, void *buffer, size_t bytes, size_t *out_bytes) = nullptr;
    esp_err_t (*seek)(void *context, int64_t offset, AudioSourceSeekOrigin origin) = nullptr;
    esp_err_t (*tell)(void *context, uint64_t *out_position) = nullptr;
    esp_err_t (*size)(void *context, uint64_t *out_size) = nullptr;
    bool (*eof)(void *context) = nullptr;
    esp_err_t (*close)(void *context) = nullptr;
    const char *(*name)(void *context) = nullptr;
};

struct AudioSource
{
    const AudioSourceOps *ops = nullptr;
    void *context = nullptr;
    uint32_t capabilities = 0;
    AudioSourceStats stats = {};
};

bool audio_source_is_open(const AudioSource *source);
bool audio_source_has_capability(const AudioSource *source, AudioSourceCapability capability);

esp_err_t audio_source_read(
    AudioSource *source,
    void *buffer,
    size_t bytes,
    size_t *out_bytes
);
esp_err_t audio_source_seek(
    AudioSource *source,
    int64_t offset,
    AudioSourceSeekOrigin origin
);
esp_err_t audio_source_tell(AudioSource *source, uint64_t *out_position);
esp_err_t audio_source_size(AudioSource *source, uint64_t *out_size);
bool audio_source_eof(const AudioSource *source);
const char *audio_source_name(const AudioSource *source);
const AudioSourceStats *audio_source_stats(const AudioSource *source);

// 关闭底层 Source 并清空绑定关系。Codec 不调用此函数；Source 生命周期由 PcmDecoder 所有。
esp_err_t audio_source_close(AudioSource *source);
