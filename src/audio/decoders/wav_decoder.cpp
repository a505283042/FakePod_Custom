#include "wav_decoder.h"

#include <string.h>
#include "esp_log.h"
#include "app_diag_config.h"

static const char *TAG = "WAV";
static constexpr size_t WAV_READ_FRAMES_MAX = 256;

static uint16_t wav_read_le16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

static uint32_t wav_read_le32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

static bool wav_read_exact(AudioSource *source, void *buffer, size_t size)
{
    if (source == nullptr || buffer == nullptr) {
        return false;
    }
    size_t got = 0;
    return audio_source_read(source, buffer, size, &got) == ESP_OK && got == size;
}

static esp_err_t wav_skip_bytes(AudioSource *source, uint32_t bytes)
{
    if (source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return audio_source_seek(source, static_cast<int64_t>(bytes), AudioSourceSeekOrigin::Current);
}

static esp_err_t wav_validate_data_bounds(WavDecoder *decoder, uint64_t data_offset)
{
    if (decoder == nullptr || decoder->source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t file_size = 0;
    esp_err_t ret = audio_source_size(decoder->source, &file_size);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint64_t declared_end = data_offset + decoder->data_size_bytes;
    if (declared_end > file_size) {
        ESP_LOGE(TAG, "WAV data chunk 超出实际文件：data偏移=%llu 声明=%lu 文件=%llu",
            static_cast<unsigned long long>(data_offset),
            static_cast<unsigned long>(decoder->data_size_bytes),
            static_cast<unsigned long long>(file_size));
        return ESP_ERR_INVALID_SIZE;
    }

    return audio_source_seek(decoder->source, static_cast<int64_t>(data_offset), AudioSourceSeekOrigin::Begin);
}

esp_err_t wav_decoder_open(WavDecoder *decoder, AudioSource *source)
{
    if (decoder == nullptr || !audio_source_is_open(source)) {
        return ESP_ERR_INVALID_ARG;
    }

    wav_decoder_close(decoder);
    decoder->source = source;

    if (!audio_source_has_capability(source, AUDIO_SOURCE_CAP_READ) ||
        !audio_source_has_capability(source, AUDIO_SOURCE_CAP_SEEK) ||
        !audio_source_has_capability(source, AUDIO_SOURCE_CAP_TELL) ||
        !audio_source_has_capability(source, AUDIO_SOURCE_CAP_SIZE)) {
        wav_decoder_close(decoder);
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t seek_ret = audio_source_seek(source, 0, AudioSourceSeekOrigin::Begin);
    if (seek_ret != ESP_OK) {
        wav_decoder_close(decoder);
        return seek_ret;
    }

    uint8_t riff_header[12] = {};
    if (!wav_read_exact(source, riff_header, sizeof(riff_header))) {
        ESP_LOGE(TAG, "WAV 文件头不足 12 字节");
        wav_decoder_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }

    if (memcmp(riff_header, "RIFF", 4) != 0 || memcmp(riff_header + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "不是标准 RIFF/WAVE 文件");
        wav_decoder_close(decoder);
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool fmt_found = false;
    bool data_found = false;
    uint64_t data_offset = UINT64_MAX;

    while (!data_found) {
        uint8_t chunk_header[8] = {};
        if (!wav_read_exact(source, chunk_header, sizeof(chunk_header))) {
            ESP_LOGE(TAG, "WAV 未找到完整 data chunk");
            wav_decoder_close(decoder);
            return ESP_ERR_INVALID_RESPONSE;
        }

        const uint32_t chunk_size = wav_read_le32(chunk_header + 4);
        const bool odd_padding = (chunk_size & 1U) != 0;

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                ESP_LOGE(TAG, "WAV fmt chunk 太短：%lu", static_cast<unsigned long>(chunk_size));
                wav_decoder_close(decoder);
                return ESP_ERR_INVALID_SIZE;
            }

            uint8_t fmt[16] = {};
            if (!wav_read_exact(source, fmt, sizeof(fmt))) {
                wav_decoder_close(decoder);
                return ESP_ERR_INVALID_SIZE;
            }

            const uint16_t audio_format = wav_read_le16(fmt + 0);
            decoder->channels = wav_read_le16(fmt + 2);
            decoder->sample_rate_hz = wav_read_le32(fmt + 4);
            decoder->byte_rate = wav_read_le32(fmt + 8);
            decoder->block_align = wav_read_le16(fmt + 12);
            decoder->bits_per_sample = wav_read_le16(fmt + 14);

            if (audio_format != 1) {
                ESP_LOGE(TAG, "暂不支持该 WAV 编码：format=0x%04X（Stage 9.2 仅支持 PCM=1）", audio_format);
                wav_decoder_close(decoder);
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (decoder->channels != 2 || decoder->bits_per_sample != 16) {
                ESP_LOGE(TAG, "暂不支持该 WAV 参数：声道=%u 位深=%u（要求双声道16bit）",
                    static_cast<unsigned>(decoder->channels),
                    static_cast<unsigned>(decoder->bits_per_sample));
                wav_decoder_close(decoder);
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (decoder->sample_rate_hz != 44100 && decoder->sample_rate_hz != 48000) {
                ESP_LOGE(TAG, "暂不支持该 WAV 采样率：%luHz（要求44100或48000）",
                    static_cast<unsigned long>(decoder->sample_rate_hz));
                wav_decoder_close(decoder);
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (decoder->block_align != 4 ||
                decoder->byte_rate != decoder->sample_rate_hz * decoder->block_align) {
                ESP_LOGE(TAG, "WAV fmt 参数不一致：block_align=%u byte_rate=%lu",
                    static_cast<unsigned>(decoder->block_align),
                    static_cast<unsigned long>(decoder->byte_rate));
                wav_decoder_close(decoder);
                return ESP_ERR_INVALID_RESPONSE;
            }

            const uint64_t fmt_skip = static_cast<uint64_t>(chunk_size - 16) + (odd_padding ? 1U : 0U);
            if (fmt_skip > static_cast<uint64_t>(UINT32_MAX)) {
                wav_decoder_close(decoder);
                return ESP_ERR_INVALID_SIZE;
            }
            esp_err_t skip_ret = wav_skip_bytes(source, static_cast<uint32_t>(fmt_skip));
            if (skip_ret != ESP_OK) {
                wav_decoder_close(decoder);
                return skip_ret;
            }
            fmt_found = true;
            continue;
        }

        if (memcmp(chunk_header, "data", 4) == 0) {
            if (!fmt_found) {
                ESP_LOGE(TAG, "WAV data chunk 出现在 fmt chunk 之前，Stage 9.2 拒绝该异常布局");
                wav_decoder_close(decoder);
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (chunk_size == 0 || (chunk_size % decoder->block_align) != 0) {
                ESP_LOGE(TAG, "WAV data 长度无效：%lu，block_align=%u",
                    static_cast<unsigned long>(chunk_size),
                    static_cast<unsigned>(decoder->block_align));
                wav_decoder_close(decoder);
                return ESP_ERR_INVALID_SIZE;
            }

            if (audio_source_tell(source, &data_offset) != ESP_OK) {
                wav_decoder_close(decoder);
                return ESP_FAIL;
            }

            decoder->data_offset_bytes = data_offset;
            decoder->data_size_bytes = chunk_size;
            decoder->data_remaining_bytes = chunk_size;
            decoder->total_frames = chunk_size / decoder->block_align;
            decoder->frames_read = 0;
            data_found = true;
            break;
        }

        const uint64_t padded = static_cast<uint64_t>(chunk_size) + (odd_padding ? 1U : 0U);
        if (padded > static_cast<uint64_t>(UINT32_MAX)) {
            wav_decoder_close(decoder);
            return ESP_ERR_INVALID_SIZE;
        }
        esp_err_t skip_ret = wav_skip_bytes(source, static_cast<uint32_t>(padded));
        if (skip_ret != ESP_OK) {
            ESP_LOGE(TAG, "跳过 WAV chunk %.4s 失败", reinterpret_cast<const char *>(chunk_header));
            wav_decoder_close(decoder);
            return skip_ret;
        }
    }

    esp_err_t bounds_ret = wav_validate_data_bounds(decoder, data_offset);
    if (bounds_ret != ESP_OK) {
        wav_decoder_close(decoder);
        return bounds_ret;
    }

#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG, "WAV解析成功：%luHz / %ubit / %u声道，PCM=%lu字节，总帧=%llu",
        static_cast<unsigned long>(decoder->sample_rate_hz),
        static_cast<unsigned>(decoder->bits_per_sample),
        static_cast<unsigned>(decoder->channels),
        static_cast<unsigned long>(decoder->data_size_bytes),
        static_cast<unsigned long long>(decoder->total_frames));
#endif
    return ESP_OK;
}

esp_err_t wav_decoder_read_pcm32(
    WavDecoder *decoder,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames)
{
    if (out_frames != nullptr) {
        *out_frames = 0;
    }
    if (decoder == nullptr || decoder->source == nullptr ||
        out_interleaved_stereo == nullptr || out_frames == nullptr || max_frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (decoder->data_remaining_bytes == 0) {
        return ESP_OK;
    }

    size_t frames_to_read = max_frames;
    if (frames_to_read > WAV_READ_FRAMES_MAX) {
        frames_to_read = WAV_READ_FRAMES_MAX;
    }

    const size_t remaining_frames = decoder->data_remaining_bytes / decoder->block_align;
    if (frames_to_read > remaining_frames) {
        frames_to_read = remaining_frames;
    }

    uint8_t raw[WAV_READ_FRAMES_MAX * 4] = {};
    const size_t bytes_to_read = frames_to_read * decoder->block_align;
    size_t bytes_read = 0;
    const esp_err_t read_ret = audio_source_read(decoder->source, raw, bytes_to_read, &bytes_read);
    if (read_ret != ESP_OK) {
        ESP_LOGE(TAG, "读取 WAV PCM 数据失败：%s", esp_err_to_name(read_ret));
        return read_ret;
    }
    if (bytes_read == 0) {
        ESP_LOGE(TAG, "WAV PCM 提前结束：声明剩余=%lu字节",
            static_cast<unsigned long>(decoder->data_remaining_bytes));
        return ESP_ERR_INVALID_SIZE;
    }

    if ((bytes_read % decoder->block_align) != 0) {
        ESP_LOGE(TAG, "WAV PCM 读取到非完整帧：%u字节", static_cast<unsigned>(bytes_read));
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t frames_read = bytes_read / decoder->block_align;
    for (size_t i = 0; i < frames_read; ++i) {
        const uint8_t *frame = raw + i * 4;
        const int16_t left = static_cast<int16_t>(wav_read_le16(frame));
        const int16_t right = static_cast<int16_t>(wav_read_le16(frame + 2));
        out_interleaved_stereo[i * 2] = static_cast<int32_t>(left) * 65536;
        out_interleaved_stereo[i * 2 + 1] = static_cast<int32_t>(right) * 65536;
    }

    decoder->data_remaining_bytes -= static_cast<uint32_t>(bytes_read);
    decoder->frames_read += frames_read;
    *out_frames = frames_read;
    return ESP_OK;
}

esp_err_t wav_decoder_seek_frame(WavDecoder *decoder, uint64_t target_frame, uint64_t *out_frame)
{
    if (out_frame != nullptr) {
        *out_frame = 0;
    }
    if (decoder == nullptr || decoder->source == nullptr || decoder->block_align == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_source_has_capability(decoder->source, AUDIO_SOURCE_CAP_SEEK)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (target_frame > decoder->total_frames) {
        target_frame = decoder->total_frames;
    }
    const uint64_t byte_offset = decoder->data_offset_bytes +
        target_frame * static_cast<uint64_t>(decoder->block_align);
    if (byte_offset > static_cast<uint64_t>(INT64_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t ret = audio_source_seek(
        decoder->source,
        static_cast<int64_t>(byte_offset),
        AudioSourceSeekOrigin::Begin);
    if (ret != ESP_OK) {
        return ret;
    }

    decoder->frames_read = target_frame;
    const uint64_t remaining_frames = decoder->total_frames - target_frame;
    const uint64_t remaining_bytes = remaining_frames * decoder->block_align;
    decoder->data_remaining_bytes = remaining_bytes > UINT32_MAX
        ? UINT32_MAX
        : static_cast<uint32_t>(remaining_bytes);
    if (out_frame != nullptr) {
        *out_frame = target_frame;
    }
#if APP_DIAG_AUDIO_SEEK
    ESP_LOGI(TAG, "SEEK_TRACE: WAV exact frame=%llu/%llu byte=%llu",
        static_cast<unsigned long long>(target_frame),
        static_cast<unsigned long long>(decoder->total_frames),
        static_cast<unsigned long long>(byte_offset));
#endif
    return ESP_OK;
}

void wav_decoder_close(WavDecoder *decoder)
{
    if (decoder == nullptr) {
        return;
    }
    // Source 生命周期由 PcmDecoder 所有，这里只清 Codec 状态。
    *decoder = WavDecoder{};
}

bool wav_decoder_is_open(const WavDecoder *decoder)
{
    return decoder != nullptr && audio_source_is_open(decoder->source);
}

bool wav_decoder_is_eof(const WavDecoder *decoder)
{
    return decoder != nullptr && audio_source_is_open(decoder->source) && decoder->data_remaining_bytes == 0;
}
