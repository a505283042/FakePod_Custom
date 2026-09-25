#include "opus_decoder.h"

#include <string.h>

#include "esp_audio_dec.h"
#include "esp_audio_types.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "../audio_rate_profile.h"

static const char *TAG = "OPUS";

// RFC 7845 对 family 0/1 建议 demuxer 能处理到 61,440B 的音频 packet。
// 这里使用 64KB PSRAM；OpusTags 无论多大都流式跳过，不进入该缓冲。
static constexpr size_t OPUS_PACKET_BUFFER_BYTES = 64U * 1024U;
// 解码 PCM 缓冲按 OpusHead 声道数 + 首音频 packet 的真实 frame duration 精确计算。
// 仍保留按 needed_size 扩容的兜底，避免把乐鑫 decoder 的输出需求写死。
static constexpr size_t OPUS_MAX_DECODED_BUFFER_BYTES = 128U * 1024U;

static esp_err_t opus_audio_error_to_esp(esp_audio_err_t error)
{
    switch (error) {
        case ESP_AUDIO_ERR_OK:
            return ESP_OK;
        case ESP_AUDIO_ERR_MEM_LACK:
            return ESP_ERR_NO_MEM;
        case ESP_AUDIO_ERR_INVALID_PARAMETER:
            return ESP_ERR_INVALID_ARG;
        case ESP_AUDIO_ERR_NOT_SUPPORT:
            return ESP_ERR_NOT_SUPPORTED;
        case ESP_AUDIO_ERR_BUFF_NOT_ENOUGH:
            return ESP_ERR_INVALID_SIZE;
        case ESP_AUDIO_ERR_DATA_LACK:
            return ESP_ERR_INVALID_SIZE;
        default:
            return ESP_FAIL;
    }
}

static uint8_t *opus_alloc_buffer(size_t bytes)
{
    return bytes > 0U
        ? static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
        : nullptr;
}

static void opus_free_buffer(void *buffer)
{
    heap_caps_free(buffer);
}

static uint16_t opus_read_le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t opus_read_le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
        (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24);
}

// 按乐鑫 decoder 的真实输出需求预留一帧 PCM。
// 后续仍保留 ESP_AUDIO_ERR_BUFF_NOT_ENOUGH -> needed_size 扩容作为兜底。
static esp_err_t opus_calculate_decoded_buffer_bytes(
    uint16_t channels,
    uint16_t frame_duration_q4_ms,
    size_t *out_bytes)
{
    if (out_bytes != nullptr) {
        *out_bytes = 0U;
    }
    if ((channels != 1U && channels != 2U) ||
        frame_duration_q4_ms == 0U ||
        out_bytes == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // frame_duration_q4_ms 的单位是 1/4ms。
    // 48kHz 下：samples = 48000 * q4_ms / 4000。
    const uint64_t sample_numerator = 48000ULL * frame_duration_q4_ms;
    if ((sample_numerator % 4000ULL) != 0ULL) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t samples_per_channel = sample_numerator / 4000ULL;
    const uint64_t required =
        samples_per_channel * channels * static_cast<uint64_t>(sizeof(int16_t));
    if (required == 0ULL || required > OPUS_MAX_DECODED_BUFFER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_bytes = static_cast<size_t>(required);
    return ESP_OK;
}

// RFC 6716 TOC 的 config 决定“单个 Opus frame”的时长。
// 返回值单位为 1/4ms，既能精确表达 2.5/5ms，也不依赖浮点。
static uint16_t opus_toc_frame_duration_q4_ms(uint8_t toc)
{
    const uint8_t config = static_cast<uint8_t>(toc >> 3U);
    if (config < 12U) {
        static constexpr uint16_t kSilkDurationsQ4[] = {40U, 80U, 160U, 240U};
        return kSilkDurationsQ4[config & 0x03U];
    }
    if (config < 16U) {
        return (config & 0x01U) != 0U ? 80U : 40U;
    }
    static constexpr uint16_t kCeltDurationsQ4[] = {10U, 20U, 40U, 80U};
    return kCeltDurationsQ4[config & 0x03U];
}

static esp_err_t opus_validate_single_frame_packet(
    const OpusDecoder *decoder,
    const uint8_t *packet,
    size_t packet_size,
    uint16_t *out_frame_duration_q4_ms)
{
    if (out_frame_duration_q4_ms != nullptr) {
        *out_frame_duration_q4_ms = 0U;
    }
    if (decoder == nullptr || packet == nullptr || packet_size == 0U ||
        out_frame_duration_q4_ms == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t toc = packet[0];
    const uint8_t frame_count_code = static_cast<uint8_t>(toc & 0x03U);
    if (frame_count_code != 0U) {
        // esp_opus_dec_decode() 的直接接口按单个 encoded frame 工作。
        // Ogg packet 内含多帧时必须继续拆 Opus 内部 framing；本轮先安全拒绝，不能误喂底层库。
        ESP_LOGE(TAG, "暂不支持多帧 Opus packet：TOC=0x%02X c=%u",
            static_cast<unsigned>(toc),
            static_cast<unsigned>(frame_count_code));
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint16_t frame_duration_q4_ms = opus_toc_frame_duration_q4_ms(toc);
    // 当前锁定的 esp_audio_codec 2.6.0 需要显式匹配 frame_duration。
    // 先支持音乐文件最常见的 20ms 单帧 packet；其它时长安全失败，避免再次触发库内 BREAK。
    if (frame_duration_q4_ms != 80U) {
        ESP_LOGE(TAG, "暂不支持该 Opus 单帧时长：%u.%02ums TOC=0x%02X",
            static_cast<unsigned>(frame_duration_q4_ms / 4U),
            static_cast<unsigned>((frame_duration_q4_ms % 4U) * 25U),
            static_cast<unsigned>(toc));
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out_frame_duration_q4_ms = frame_duration_q4_ms;
    return ESP_OK;
}

static esp_err_t opus_source_read_exact(AudioSource *source, void *buffer, size_t bytes)
{
    if (source == nullptr || buffer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t done = 0U;
    while (done < bytes) {
        size_t read_bytes = 0U;
        const esp_err_t ret = audio_source_read(
            source,
            static_cast<uint8_t *>(buffer) + done,
            bytes - done,
            &read_bytes);
        done += read_bytes;
        if (ret != ESP_OK) {
            return ret;
        }
        if (read_bytes == 0U) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

static esp_err_t opus_source_skip(AudioSource *source, size_t bytes)
{
    if (source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes == 0U) {
        return ESP_OK;
    }

    if (audio_source_has_capability(source, AUDIO_SOURCE_CAP_SEEK)) {
        return audio_source_seek(
            source,
            static_cast<int64_t>(bytes),
            AudioSourceSeekOrigin::Current);
    }

    uint8_t scratch[64] = {};
    size_t remaining = bytes;
    while (remaining > 0U) {
        const size_t chunk = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
        const esp_err_t ret = opus_source_read_exact(source, scratch, chunk);
        if (ret != ESP_OK) {
            return ret;
        }
        remaining -= chunk;
    }
    return ESP_OK;
}

static esp_err_t opus_ogg_load_page(OpusDecoder *decoder, bool continuation_required, bool first_page)
{
    if (decoder == nullptr || decoder->source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t header[27] = {};
    esp_err_t ret = opus_source_read_exact(decoder->source, header, sizeof(header));
    if (ret != ESP_OK) {
        return ret;
    }
    if (memcmp(header, "OggS", 4U) != 0 || header[4] != 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const bool continued = (header[5] & 0x01U) != 0U;
    if (continued != continuation_required) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (first_page && (header[5] & 0x02U) == 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint32_t serial = opus_read_le32(header + 14U);
    if (!decoder->ogg_serial_valid) {
        decoder->ogg_serial = serial;
        decoder->ogg_serial_valid = true;
    } else if (decoder->ogg_serial != serial) {
        // 基础播放只接受单 logical stream，避免 chained Ogg 的 metadata/时钟混淆。
        return ESP_ERR_NOT_SUPPORTED;
    }

    decoder->ogg_segment_count = header[26];
    decoder->ogg_segment_index = 0U;
    decoder->ogg_segment_remaining = 0U;
    decoder->ogg_segment_ends_packet = false;
    if (decoder->ogg_segment_count > 0U) {
        ret = opus_source_read_exact(
            decoder->source,
            decoder->ogg_lacing,
            decoder->ogg_segment_count);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t opus_ogg_prepare_segment(OpusDecoder *decoder)
{
    if (decoder == nullptr || !decoder->ogg_packet_active) {
        return ESP_ERR_INVALID_STATE;
    }

    while (decoder->ogg_segment_remaining == 0U && !decoder->ogg_packet_ended) {
        if (decoder->ogg_segment_index >= decoder->ogg_segment_count) {
            const esp_err_t ret = opus_ogg_load_page(decoder, true, false);
            if (ret != ESP_OK) {
                return ret;
            }
        }

        const uint8_t length = decoder->ogg_lacing[decoder->ogg_segment_index++];
        decoder->ogg_segment_remaining = length;
        decoder->ogg_segment_ends_packet = length < 255U;
        if (length == 0U && decoder->ogg_segment_ends_packet) {
            decoder->ogg_packet_ended = true;
        }
    }
    return ESP_OK;
}

static esp_err_t opus_ogg_packet_transfer(
    OpusDecoder *decoder,
    uint8_t *out,
    size_t bytes,
    bool skip)
{
    if (decoder == nullptr || !decoder->ogg_packet_active) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t done = 0U;
    while (done < bytes) {
        esp_err_t ret = opus_ogg_prepare_segment(decoder);
        if (ret != ESP_OK) {
            return ret;
        }
        if (decoder->ogg_packet_ended) {
            return ESP_ERR_INVALID_SIZE;
        }

        const size_t remaining = bytes - done;
        const size_t chunk = remaining < decoder->ogg_segment_remaining
            ? remaining
            : decoder->ogg_segment_remaining;
        ret = skip
            ? opus_source_skip(decoder->source, chunk)
            : opus_source_read_exact(decoder->source, out + done, chunk);
        if (ret != ESP_OK) {
            return ret;
        }

        done += chunk;
        decoder->ogg_segment_remaining = static_cast<uint16_t>(
            decoder->ogg_segment_remaining - chunk);
        if (decoder->ogg_segment_remaining == 0U && decoder->ogg_segment_ends_packet) {
            decoder->ogg_packet_ended = true;
        }
    }
    return ESP_OK;
}

static esp_err_t opus_ogg_finish_packet(OpusDecoder *decoder)
{
    if (decoder == nullptr || !decoder->ogg_packet_active) {
        return ESP_ERR_INVALID_ARG;
    }

    while (!decoder->ogg_packet_ended) {
        esp_err_t ret = opus_ogg_prepare_segment(decoder);
        if (ret != ESP_OK) {
            return ret;
        }
        if (decoder->ogg_packet_ended) {
            break;
        }

        // OpusTags 可能因为内嵌封面达到数百 KB。把同一 Ogg page 内连续的 lacing
        // 合并成一次 seek/read-discard，避免按 255B 做上千次 fseek。
        size_t skip_bytes = decoder->ogg_segment_remaining;
        const bool current_ends_packet = decoder->ogg_segment_ends_packet;
        decoder->ogg_segment_remaining = 0U;
        if (current_ends_packet) {
            decoder->ogg_packet_ended = true;
        } else {
            while (decoder->ogg_segment_index < decoder->ogg_segment_count) {
                const uint8_t length = decoder->ogg_lacing[decoder->ogg_segment_index++];
                skip_bytes += length;
                if (length < 255U) {
                    decoder->ogg_packet_ended = true;
                    break;
                }
            }
        }

        ret = opus_source_skip(decoder->source, skip_bytes);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t opus_ogg_begin_next_packet(OpusDecoder *decoder)
{
    if (decoder == nullptr || !decoder->ogg_packet_active || !decoder->ogg_packet_ended) {
        return ESP_ERR_INVALID_STATE;
    }

    if (decoder->ogg_segment_index >= decoder->ogg_segment_count) {
        const esp_err_t ret = opus_ogg_load_page(decoder, false, false);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    decoder->ogg_packet_ended = false;
    decoder->ogg_segment_remaining = 0U;
    decoder->ogg_segment_ends_packet = false;
    return ESP_OK;
}

static esp_err_t opus_ogg_read_current_packet(
    OpusDecoder *decoder,
    uint8_t *buffer,
    size_t capacity,
    size_t *out_size)
{
    if (out_size != nullptr) {
        *out_size = 0U;
    }
    if (decoder == nullptr || buffer == nullptr || capacity == 0U || out_size == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t written = 0U;
    while (!decoder->ogg_packet_ended) {
        esp_err_t ret = opus_ogg_prepare_segment(decoder);
        if (ret != ESP_OK) {
            return ret;
        }
        if (decoder->ogg_packet_ended) {
            break;
        }

        const size_t segment_bytes = decoder->ogg_segment_remaining;
        if (segment_bytes > capacity - written) {
            ESP_LOGE(TAG, "Opus 音频 packet 过大：已读=%uB 当前段=%uB 上限=%uB",
                static_cast<unsigned>(written),
                static_cast<unsigned>(segment_bytes),
                static_cast<unsigned>(capacity));
            return ESP_ERR_INVALID_SIZE;
        }
        ret = opus_ogg_packet_transfer(
            decoder,
            buffer + written,
            segment_bytes,
            false);
        if (ret != ESP_OK) {
            return ret;
        }
        written += segment_bytes;
    }

    *out_size = written;
    return written > 0U ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t opus_parse_stream_headers(OpusDecoder *decoder)
{
    if (decoder == nullptr || decoder->source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_source_seek(decoder->source, 0, AudioSourceSeekOrigin::Begin);
    if (ret != ESP_OK) {
        return ret;
    }

    decoder->ogg_packet_active = true;
    decoder->ogg_packet_ended = false;
    ret = opus_ogg_load_page(decoder, false, true);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t head[19] = {};
    ret = opus_ogg_packet_transfer(decoder, head, sizeof(head), false);
    if (ret != ESP_OK) {
        return ret;
    }
    if (memcmp(head, "OpusHead", 8U) != 0 || (head[8] & 0xF0U) != 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint8_t channels = head[9];
    if ((channels != 1U && channels != 2U) || head[18] != 0U) {
        ESP_LOGE(TAG, "暂不支持该 Ogg Opus 声道映射：channels=%u mapping=%u",
            static_cast<unsigned>(channels), static_cast<unsigned>(head[18]));
        return ESP_ERR_NOT_SUPPORTED;
    }
    decoder->channels = channels;
    decoder->pre_skip = opus_read_le16(head + 10U);
    decoder->pre_skip_remaining = decoder->pre_skip;
    ret = opus_ogg_finish_packet(decoder);
    if (ret != ESP_OK) {
        return ret;
    }

    // 第二个 packet 必须是 OpusTags。只读取 8 字节签名，其余内容（包括大尺寸 Base64 封面）直接跳过，
    // 绝不能交给 Opus 解码器。
    ret = opus_ogg_begin_next_packet(decoder);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t tags_magic[8] = {};
    ret = opus_ogg_packet_transfer(decoder, tags_magic, sizeof(tags_magic), false);
    if (ret != ESP_OK || memcmp(tags_magic, "OpusTags", 8U) != 0) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }
    ret = opus_ogg_finish_packet(decoder);
    if (ret != ESP_OK) {
        return ret;
    }

    // 进入第一个真正的音频 packet。
    return opus_ogg_begin_next_packet(decoder);
}

static esp_err_t opus_resize_decoded_buffer(OpusDecoder *decoder, size_t requested)
{
    if (decoder == nullptr || requested == 0U || requested > OPUS_MAX_DECODED_BUFFER_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (requested <= decoder->decoded_capacity) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    uint8_t *replacement = nullptr;
    if (decoder->workspace != nullptr) {
        ret = audio_decode_workspace_reserve_decoded(
            decoder->workspace, requested, &replacement);
    } else {
        replacement = opus_alloc_buffer(requested);
        ret = replacement != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
        if (ret == ESP_OK) {
            opus_free_buffer(decoder->decoded_buffer);
        }
    }
    if (ret != ESP_OK || replacement == nullptr) {
        return ret != ESP_OK ? ret : ESP_ERR_NO_MEM;
    }
    decoder->decoded_buffer = replacement;
    decoder->decoded_capacity = requested;
    return ESP_OK;
}

static esp_err_t opus_decode_next_output(OpusDecoder *decoder)
{
    if (decoder == nullptr || decoder->opus_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    decoder->decoded_offset = 0U;
    decoder->decoded_size = 0U;

    while (!decoder->eof) {
        size_t packet_size = decoder->input_size;
        esp_err_t ret = ESP_OK;
        if (!decoder->input_packet_ready) {
            ret = opus_ogg_read_current_packet(
                decoder,
                decoder->input_buffer,
                decoder->input_capacity,
                &packet_size);
            if (ret != ESP_OK) {
                // 顺序读到文件尾时视为正常 EOS；其它结构错误保持显式失败。
                if (ret == ESP_ERR_INVALID_SIZE && audio_source_eof(decoder->source)) {
                    decoder->eof = true;
                    return ESP_OK;
                }
                return ret;
            }
            decoder->input_size = packet_size;

            uint16_t frame_duration_q4_ms = 0U;
            ret = opus_validate_single_frame_packet(
                decoder,
                decoder->input_buffer,
                packet_size,
                &frame_duration_q4_ms);
            if (ret != ESP_OK) {
                return ret;
            }
            if (frame_duration_q4_ms != decoder->frame_duration_q4_ms) {
                ESP_LOGE(TAG, "Opus 流中途改变帧时长：当前=%u/4ms 新=%u/4ms",
                    static_cast<unsigned>(decoder->frame_duration_q4_ms),
                    static_cast<unsigned>(frame_duration_q4_ms));
                return ESP_ERR_NOT_SUPPORTED;
            }
        }
        decoder->input_packet_ready = true;

        if (!decoder->runtime_info_verified) {
            ESP_LOGI(TAG, "Opus首音频packet：%uB TOC=0x%02X frame=20ms channels=%u pre_skip=%u",
                static_cast<unsigned>(packet_size),
                static_cast<unsigned>(decoder->input_buffer[0]),
                static_cast<unsigned>(decoder->channels),
                static_cast<unsigned>(decoder->pre_skip));
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = decoder->input_buffer;
        raw.len = static_cast<uint32_t>(packet_size);

        esp_audio_dec_out_frame_t out = {};
        out.buffer = decoder->decoded_buffer;
        out.len = static_cast<uint32_t>(decoder->decoded_capacity);

        esp_audio_dec_info_t info = {};
        const esp_audio_err_t codec_ret = esp_opus_dec_decode(
            static_cast<esp_audio_dec_handle_t>(decoder->opus_handle), &raw, &out, &info);
        if (codec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (out.needed_size == 0U || out.needed_size > OPUS_MAX_DECODED_BUFFER_BYTES) {
                ESP_LOGE(TAG, "Opus 解码器请求异常输出缓冲：%lu字节",
                    static_cast<unsigned long>(out.needed_size));
                return ESP_ERR_INVALID_SIZE;
            }
            ret = opus_resize_decoded_buffer(decoder, out.needed_size);
            if (ret != ESP_OK) {
                return ret;
            }
            // 同一 packet 必须原样重试，不能提前推进 Ogg packet。
            out.buffer = decoder->decoded_buffer;
            out.len = static_cast<uint32_t>(decoder->decoded_capacity);
            raw.consumed = 0U;
            info = {};
            const esp_audio_err_t retry_ret = esp_opus_dec_decode(
                static_cast<esp_audio_dec_handle_t>(decoder->opus_handle), &raw, &out, &info);
            if (retry_ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "乐鑫 RAW Opus 扩容后重试失败：codec_ret=%d", static_cast<int>(retry_ret));
                return opus_audio_error_to_esp(retry_ret);
            }
        } else if (codec_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "乐鑫 RAW Opus 解码失败：packet=%uB codec_ret=%d",
                static_cast<unsigned>(packet_size), static_cast<int>(codec_ret));
            return opus_audio_error_to_esp(codec_ret);
        }

        if (raw.consumed != packet_size) {
            ESP_LOGE(TAG, "RAW Opus 未完整消费 packet：packet=%uB consumed=%luB",
                static_cast<unsigned>(packet_size), static_cast<unsigned long>(raw.consumed));
            return ESP_ERR_INVALID_RESPONSE;
        }

        decoder->input_packet_ready = false;
        decoder->input_size = 0U;
        ret = opus_ogg_begin_next_packet(decoder);
        if (ret != ESP_OK) {
            if (audio_source_eof(decoder->source)) {
                decoder->eof = true;
            } else {
                return ret;
            }
        }

        if (out.decoded_size == 0U) {
            continue;
        }
        if (!decoder->runtime_info_verified) {
            if (info.sample_rate != 48000U || !audio_rate_profile_get(info.sample_rate, nullptr)) {
                ESP_LOGE(TAG, "暂不支持该 Opus 输出采样率：%luHz（当前要求48kHz）",
                    static_cast<unsigned long>(info.sample_rate));
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (info.channel != decoder->channels || (info.channel != 1U && info.channel != 2U)) {
                ESP_LOGE(TAG, "Opus 解码声道与 OpusHead 不一致：header=%u decoder=%u",
                    static_cast<unsigned>(decoder->channels), static_cast<unsigned>(info.channel));
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (info.bits_per_sample != 16U) {
                ESP_LOGE(TAG, "暂不支持该 Opus PCM 位深：%u",
                    static_cast<unsigned>(info.bits_per_sample));
                return ESP_ERR_NOT_SUPPORTED;
            }
            decoder->sample_rate_hz = info.sample_rate;
            decoder->bits_per_sample = info.bits_per_sample;
            decoder->bitrate = info.bitrate;
            decoder->runtime_info_verified = true;
        }
        decoder->decoded_size = out.decoded_size;
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t opus_decoder_open(
    OpusDecoder *decoder,
    AudioSource *source,
    AudioDecodeWorkspace *workspace)
{
    if (decoder == nullptr || !audio_source_is_open(source)) {
        return ESP_ERR_INVALID_ARG;
    }
    opus_decoder_close(decoder);

    if (!audio_source_has_capability(source, AUDIO_SOURCE_CAP_READ) ||
        !audio_source_has_capability(source, AUDIO_SOURCE_CAP_SEEK)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    decoder->source = source;
    decoder->workspace = workspace;
    esp_err_t ret = ESP_OK;
    if (workspace != nullptr) {
        ret = audio_decode_workspace_reserve_input(
            workspace, OPUS_PACKET_BUFFER_BYTES, &decoder->input_buffer);
    } else {
        decoder->input_buffer = opus_alloc_buffer(OPUS_PACKET_BUFFER_BYTES);
        ret = decoder->input_buffer != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (ret != ESP_OK || decoder->input_buffer == nullptr) {
        opus_decoder_close(decoder);
        return ret != ESP_OK ? ret : ESP_ERR_NO_MEM;
    }
    decoder->input_capacity = OPUS_PACKET_BUFFER_BYTES;

    ret = opus_parse_stream_headers(decoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "解析 Ogg Opus 头失败：%s", esp_err_to_name(ret));
        opus_decoder_close(decoder);
        return ret;
    }

    // 在打开乐鑫 decoder 前先读取首个真正的音频 packet。
    // esp_audio_codec 2.6.0 的 Opus frame_duration 必须和编码帧匹配，不能固定写 60ms。
    size_t first_packet_size = 0U;
    ret = opus_ogg_read_current_packet(
        decoder,
        decoder->input_buffer,
        decoder->input_capacity,
        &first_packet_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取 Opus 首音频 packet 失败：%s", esp_err_to_name(ret));
        opus_decoder_close(decoder);
        return ret;
    }
    uint16_t frame_duration_q4_ms = 0U;
    ret = opus_validate_single_frame_packet(
        decoder,
        decoder->input_buffer,
        first_packet_size,
        &frame_duration_q4_ms);
    if (ret != ESP_OK) {
        opus_decoder_close(decoder);
        return ret;
    }
    decoder->input_size = first_packet_size;
    decoder->input_packet_ready = true;
    decoder->frame_duration_q4_ms = frame_duration_q4_ms;

    size_t decoded_buffer_bytes = 0U;
    ret = opus_calculate_decoded_buffer_bytes(
        decoder->channels,
        decoder->frame_duration_q4_ms,
        &decoded_buffer_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "计算 Opus PCM 缓冲失败：channels=%u frame_q4=%u ret=%s",
            static_cast<unsigned>(decoder->channels),
            static_cast<unsigned>(decoder->frame_duration_q4_ms),
            esp_err_to_name(ret));
        opus_decoder_close(decoder);
        return ret;
    }

    if (workspace != nullptr) {
        ret = audio_decode_workspace_reserve_decoded(
            workspace, decoded_buffer_bytes, &decoder->decoded_buffer);
    } else {
        decoder->decoded_buffer = opus_alloc_buffer(decoded_buffer_bytes);
        ret = decoder->decoded_buffer != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (ret != ESP_OK || decoder->decoded_buffer == nullptr) {
        opus_decoder_close(decoder);
        return ret != ESP_OK ? ret : ESP_ERR_NO_MEM;
    }
    decoder->decoded_capacity = decoded_buffer_bytes;

    esp_opus_dec_cfg_t opus_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    opus_cfg.sample_rate = 48000;
    opus_cfg.channel = decoder->channels;
    opus_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    opus_cfg.self_delimited = false;

    esp_audio_dec_handle_t handle = nullptr;
    const esp_audio_err_t codec_ret = esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &handle);
    if (codec_ret != ESP_AUDIO_ERR_OK || handle == nullptr) {
        ESP_LOGE(TAG, "打开乐鑫 Opus Decoder 失败：codec_ret=%d", static_cast<int>(codec_ret));
        opus_decoder_close(decoder);
        return opus_audio_error_to_esp(codec_ret);
    }
    decoder->opus_handle = handle;

    // 在 I2S/DAC 启动前预解第一包 PCM，验证 Ogg 拆包和真实输出格式。
    ret = opus_decode_next_output(decoder);
    if (ret != ESP_OK || decoder->decoded_size == 0U || !decoder->runtime_info_verified) {
        if (ret == ESP_OK) {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGE(TAG, "Ogg Opus 首包 PCM 预解码失败：%s", esp_err_to_name(ret));
        opus_decoder_close(decoder);
        return ret;
    }

    const size_t frame_bytes = sizeof(int16_t) * decoder->channels;
    if (frame_bytes == 0U || decoder->decoded_size % frame_bytes != 0U) {
        opus_decoder_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t opus_decoder_set_total_frames_hint(OpusDecoder *decoder, uint64_t total_frames)
{
    if (decoder == nullptr || !opus_decoder_is_open(decoder) || total_frames == 0ULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (decoder->frames_read > total_frames) {
        return ESP_ERR_INVALID_SIZE;
    }

    decoder->total_frames = total_frames;
    return ESP_OK;
}

esp_err_t opus_decoder_read_pcm32(
    OpusDecoder *decoder,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames)
{
    if (out_frames != nullptr) {
        *out_frames = 0U;
    }
    if (decoder == nullptr || out_interleaved_stereo == nullptr || out_frames == nullptr ||
        max_frames == 0U || !opus_decoder_is_open(decoder)) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t source_frame_bytes = sizeof(int16_t) * decoder->channels;
    if (decoder->total_frames > 0ULL && decoder->frames_read >= decoder->total_frames) {
        // final granule 已到：不再依赖物理文件 EOF，避免最后一个 packet 的 padding
        // 或预读 Source 的 EOF 时序拖住 Finished/自动下一首。
        decoder->decoded_offset = decoder->decoded_size;
        decoder->eof = true;
        return ESP_OK;
    }

    size_t produced = 0U;
    while (produced < max_frames) {
        if (decoder->decoded_offset >= decoder->decoded_size) {
            esp_err_t ret = opus_decode_next_output(decoder);
            if (ret != ESP_OK) {
                return ret;
            }
            if (decoder->decoded_offset >= decoder->decoded_size) {
                break;
            }
            if (decoder->decoded_size % source_frame_bytes != 0U) {
                return ESP_ERR_INVALID_SIZE;
            }
        }

        size_t available_frames =
            (decoder->decoded_size - decoder->decoded_offset) / source_frame_bytes;

        // RFC 7845 的 pre-skip 属于 48kHz PCM 样本数。只在流开头丢弃，
        // frames_read 从真正提交给播放链的第一个有效样本开始计数。
        if (decoder->pre_skip_remaining > 0U && available_frames > 0U) {
            const size_t skip_frames = decoder->pre_skip_remaining < available_frames
                ? decoder->pre_skip_remaining
                : available_frames;
            decoder->decoded_offset += skip_frames * source_frame_bytes;
            decoder->pre_skip_remaining -= static_cast<uint32_t>(skip_frames);
            available_frames -= skip_frames;
            if (available_frames == 0U) {
                continue;
            }
        }

        if (decoder->total_frames > 0ULL) {
            const uint64_t remaining_total = decoder->total_frames - decoder->frames_read;
            if (remaining_total == 0ULL) {
                decoder->decoded_offset = decoder->decoded_size;
                decoder->eof = true;
                break;
            }
            if (remaining_total < available_frames) {
                // RFC 7845 end trimming：最后一个 Opus packet 可以包含超过 final granule 的 padding。
                // frames_read 已经排除了 pre-skip，因此直接按 Catalog 的可播放总帧裁尾。
                available_frames = static_cast<size_t>(remaining_total);
            }
        }

        const size_t copy_frames = (max_frames - produced) < available_frames
            ? max_frames - produced
            : available_frames;
        const uint8_t *src = decoder->decoded_buffer + decoder->decoded_offset;
        for (size_t i = 0U; i < copy_frames; ++i) {
            int16_t left = 0;
            memcpy(&left, src, sizeof(left));
            int16_t right = left;
            if (decoder->channels == 2U) {
                memcpy(&right, src + sizeof(int16_t), sizeof(right));
            }
            out_interleaved_stereo[(produced + i) * 2U] = static_cast<int32_t>(left) * 65536;
            out_interleaved_stereo[(produced + i) * 2U + 1U] = static_cast<int32_t>(right) * 65536;
            src += source_frame_bytes;
        }

        const size_t consumed_bytes = copy_frames * source_frame_bytes;
        decoder->decoded_offset += consumed_bytes;
        decoder->frames_read += copy_frames;
        produced += copy_frames;

        if (decoder->total_frames > 0ULL && decoder->frames_read >= decoder->total_frames) {
            // 精确到 final granule 后立即进入逻辑 EOF。即使底层 Source 还预读了数据，
            // 下一次 AudioTask 调用也会得到 0 frame + EOF，从而发布 Finished 并自动下一首。
            decoder->decoded_offset = decoder->decoded_size;
            decoder->eof = true;
            break;
        }
    }
    *out_frames = produced;
    return ESP_OK;
}

void opus_decoder_close(OpusDecoder *decoder)
{
    if (decoder == nullptr) {
        return;
    }
    if (decoder->opus_handle != nullptr) {
        esp_opus_dec_close(static_cast<esp_audio_dec_handle_t>(decoder->opus_handle));
    }
    if (decoder->workspace == nullptr) {
        opus_free_buffer(decoder->input_buffer);
        opus_free_buffer(decoder->decoded_buffer);
    }
    *decoder = {};
}

bool opus_decoder_is_open(const OpusDecoder *decoder)
{
    return decoder != nullptr && audio_source_is_open(decoder->source) && decoder->opus_handle != nullptr;
}

bool opus_decoder_is_eof(const OpusDecoder *decoder)
{
    return decoder != nullptr && decoder->eof && decoder->decoded_offset >= decoder->decoded_size;
}
