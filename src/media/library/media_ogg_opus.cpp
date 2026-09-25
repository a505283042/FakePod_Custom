#include "media_ogg_opus.h"

#include <limits.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "OggOpus扫描";

static constexpr uint32_t OGG_MAX_COMMENT_COUNT = 4096U;
static constexpr uint32_t OGG_MAX_TEXT_COMMENT_BYTES = 16U * 1024U;
static constexpr size_t OGG_TAIL_SCAN_BYTES = 128U * 1024U;

static uint16_t read_le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
        (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    uint64_t value = 0ULL;
    for (uint8_t i = 0; i < 8U; ++i) {
        value |= static_cast<uint64_t>(p[i]) << (8U * i);
    }
    return value;
}

static bool seek_u64(FILE *file, uint64_t offset)
{
    return file != nullptr && offset <= static_cast<uint64_t>(LONG_MAX) &&
        fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
}

struct OggPacketReader
{
    FILE *file = nullptr;
    uint64_t file_size = 0ULL;
    uint32_t serial = 0U;
    bool serial_valid = false;

    uint8_t lacing[255] = {};
    uint8_t segment_count = 0U;
    uint8_t segment_index = 0U;
    uint16_t segment_remaining = 0U;
    bool segment_ends_packet = false;
    bool page_loaded = false;
    bool packet_active = false;
    bool packet_ended = false;
};

static esp_err_t ogg_load_page(OggPacketReader *reader, bool continuation_required, bool first_page)
{
    if (reader == nullptr || reader->file == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const long page_pos = ftell(reader->file);
    if (page_pos < 0) {
        return ESP_FAIL;
    }

    uint8_t header[27] = {};
    if (fread(header, 1, sizeof(header), reader->file) != sizeof(header)) {
        return ESP_ERR_INVALID_SIZE;
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

    const uint32_t serial = read_le32(header + 14U);
    if (!reader->serial_valid) {
        reader->serial = serial;
        reader->serial_valid = true;
    } else if (reader->serial != serial) {
        // 当前基础支持只处理一个 logical stream，避免 chained Ogg 的时钟/metadata 语义混淆。
        return ESP_ERR_NOT_SUPPORTED;
    }

    reader->segment_count = header[26];
    reader->segment_index = 0U;
    reader->segment_remaining = 0U;
    reader->segment_ends_packet = false;
    if (reader->segment_count > 0U &&
        fread(reader->lacing, 1, reader->segment_count, reader->file) != reader->segment_count) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t payload_bytes = 0U;
    for (uint16_t i = 0; i < reader->segment_count; ++i) {
        payload_bytes += reader->lacing[i];
    }
    const uint64_t payload_offset = static_cast<uint64_t>(page_pos) + 27ULL + reader->segment_count;
    if (payload_offset > reader->file_size || payload_bytes > reader->file_size - payload_offset) {
        return ESP_ERR_INVALID_SIZE;
    }

    reader->page_loaded = true;
    return ESP_OK;
}

static esp_err_t ogg_begin_first_packet(OggPacketReader *reader, FILE *file, uint64_t file_size)
{
    if (reader == nullptr || file == nullptr || file_size < 27U || !seek_u64(file, 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    *reader = {};
    reader->file = file;
    reader->file_size = file_size;
    esp_err_t ret = ogg_load_page(reader, false, true);
    if (ret != ESP_OK) {
        return ret;
    }
    reader->packet_active = true;
    reader->packet_ended = false;
    return ESP_OK;
}

static esp_err_t ogg_prepare_segment(OggPacketReader *reader)
{
    if (reader == nullptr || !reader->packet_active) {
        return ESP_ERR_INVALID_STATE;
    }
    while (reader->segment_remaining == 0U && !reader->packet_ended) {
        if (reader->segment_index >= reader->segment_count) {
            const esp_err_t ret = ogg_load_page(reader, true, false);
            if (ret != ESP_OK) {
                return ret;
            }
        }
        const uint8_t length = reader->lacing[reader->segment_index++];
        reader->segment_remaining = length;
        reader->segment_ends_packet = length < 255U;
        if (length == 0U && reader->segment_ends_packet) {
            reader->packet_ended = true;
        }
    }
    return ESP_OK;
}

static esp_err_t ogg_packet_transfer(OggPacketReader *reader, uint8_t *out, uint32_t bytes, bool skip)
{
    if (reader == nullptr || !reader->packet_active) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t done = 0U;
    while (done < bytes) {
        esp_err_t ret = ogg_prepare_segment(reader);
        if (ret != ESP_OK) {
            return ret;
        }
        if (reader->packet_ended) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t remaining = bytes - done;
        const uint32_t chunk = remaining < reader->segment_remaining
            ? remaining
            : reader->segment_remaining;
        if (skip) {
            if (fseek(reader->file, static_cast<long>(chunk), SEEK_CUR) != 0) {
                return ESP_FAIL;
            }
        } else if (chunk > 0U && fread(out + done, 1, chunk, reader->file) != chunk) {
            return ESP_ERR_INVALID_SIZE;
        }
        done += chunk;
        reader->segment_remaining = static_cast<uint16_t>(reader->segment_remaining - chunk);
        if (reader->segment_remaining == 0U && reader->segment_ends_packet) {
            reader->packet_ended = true;
        }
    }
    return ESP_OK;
}

static esp_err_t ogg_packet_read(OggPacketReader *reader, void *out, uint32_t bytes)
{
    return out != nullptr || bytes == 0U
        ? ogg_packet_transfer(reader, static_cast<uint8_t *>(out), bytes, false)
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t ogg_packet_skip(OggPacketReader *reader, uint32_t bytes)
{
    return ogg_packet_transfer(reader, nullptr, bytes, true);
}

static esp_err_t ogg_finish_packet(OggPacketReader *reader)
{
    if (reader == nullptr || !reader->packet_active) {
        return ESP_ERR_INVALID_ARG;
    }
    while (!reader->packet_ended) {
        esp_err_t ret = ogg_prepare_segment(reader);
        if (ret != ESP_OK) {
            return ret;
        }
        if (reader->packet_ended) {
            break;
        }
        ret = ogg_packet_skip(reader, reader->segment_remaining);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t ogg_begin_next_packet(OggPacketReader *reader)
{
    if (reader == nullptr || !reader->packet_active || !reader->packet_ended) {
        return ESP_ERR_INVALID_STATE;
    }
    if (reader->segment_index >= reader->segment_count) {
        const esp_err_t ret = ogg_load_page(reader, false, false);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    reader->packet_ended = false;
    reader->segment_remaining = 0U;
    reader->segment_ends_packet = false;
    return ESP_OK;
}

static esp_err_t ogg_read_opus_head(OggPacketReader *reader, uint8_t *out_channels, uint16_t *out_pre_skip)
{
    uint8_t head[19] = {};
    esp_err_t ret = ogg_packet_read(reader, head, sizeof(head));
    if (ret != ESP_OK) {
        return ret;
    }
    // RFC 7845 规定 0x00~0x0F 与当前主版本兼容；高四位非零才视为不兼容。
    if (memcmp(head, "OpusHead", 8U) != 0 || (head[8] & 0xF0U) != 0U || head[9] == 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (out_channels != nullptr) {
        *out_channels = head[9];
    }
    if (out_pre_skip != nullptr) {
        *out_pre_skip = read_le16(head + 10U);
    }
    return ogg_finish_packet(reader);
}

static bool ogg_find_final_granule(
    FILE *file,
    uint64_t file_size,
    uint32_t serial,
    uint64_t *out_granule)
{
    if (file == nullptr || out_granule == nullptr || file_size < 27U) {
        return false;
    }
    const size_t read_size = static_cast<size_t>(
        file_size < OGG_TAIL_SCAN_BYTES ? file_size : OGG_TAIL_SCAN_BYTES);
    const uint64_t start = file_size - read_size;
    uint8_t *tail = static_cast<uint8_t *>(
        heap_caps_malloc(read_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (tail == nullptr || !seek_u64(file, start)) {
        heap_caps_free(tail);
        return false;
    }
    if (fread(tail, 1, read_size, file) != read_size) {
        heap_caps_free(tail);
        return false;
    }

    bool found = false;
    for (size_t i = read_size >= 4U ? read_size - 4U : 0U; ; --i) {
        if (i + 27U <= read_size && memcmp(tail + i, "OggS", 4U) == 0 && tail[i + 4U] == 0U) {
            const uint8_t segment_count = tail[i + 26U];
            const size_t header_size = 27U + segment_count;
            if (i + header_size <= read_size) {
                uint32_t payload_size = 0U;
                for (uint16_t j = 0; j < segment_count; ++j) {
                    payload_size += tail[i + 27U + j];
                }
                const size_t page_size = header_size + payload_size;
                if (i + page_size <= read_size &&
                    (tail[i + 5U] & 0x04U) != 0U &&
                    read_le32(tail + i + 14U) == serial) {
                    const uint64_t granule = read_le64(tail + i + 6U);
                    if (granule != UINT64_MAX) {
                        *out_granule = granule;
                        found = true;
                        break;
                    }
                }
            }
        }
        if (i == 0U) {
            break;
        }
    }
    heap_caps_free(tail);
    return found;
}

esp_err_t media_ogg_opus_probe(FILE *file, uint64_t file_size, MediaTechnicalInfo *out_info)
{
    if (file == nullptr || out_info == nullptr || file_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_info = {};

    OggPacketReader reader = {};
    esp_err_t ret = ogg_begin_first_packet(&reader, file, file_size);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t channels = 0U;
    uint16_t pre_skip = 0U;
    ret = ogg_read_opus_head(&reader, &channels, &pre_skip);
    if (ret != ESP_OK) {
        return ret;
    }

    uint64_t final_granule = 0ULL;
    if (!ogg_find_final_granule(file, file_size, reader.serial, &final_granule) ||
        final_granule < pre_skip) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint64_t total_frames = final_granule - pre_skip;
    const uint64_t duration_ms = (total_frames * 1000ULL) / 48000ULL;
    out_info->sample_rate_hz = 48000U;
    out_info->channels = channels;
    out_info->bits_per_sample = 16U;
    out_info->total_frames = total_frames;
    out_info->duration_ms = duration_ms > UINT32_MAX
        ? UINT32_MAX
        : static_cast<uint32_t>(duration_ms);
    if (out_info->duration_ms > 0U) {
        const uint64_t bitrate_kbps = (file_size * 8ULL) / out_info->duration_ms;
        out_info->bitrate_kbps = bitrate_kbps > UINT32_MAX
            ? UINT32_MAX
            : static_cast<uint32_t>(bitrate_kbps);
    }
    // Ogg Simple Decoder 必须从容器起点消费 OpusHead/OpusTags，不能像裸帧格式那样跳到 audio offset。
    out_info->audio_data_offset = 0ULL;
    out_info->metadata_end_offset = 0ULL;
    out_info->flags = MEDIA_TECH_PARSED;
    return ESP_OK;
}

esp_err_t media_ogg_opus_visit_text_comments(
    FILE *file,
    uint64_t file_size,
    MediaOggOpusCommentCallback callback,
    void *context)
{
    if (file == nullptr || callback == nullptr || file_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    OggPacketReader reader = {};
    esp_err_t ret = ogg_begin_first_packet(&reader, file, file_size);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ogg_read_opus_head(&reader, nullptr, nullptr);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ogg_begin_next_packet(&reader);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t signature[8] = {};
    if ((ret = ogg_packet_read(&reader, signature, sizeof(signature))) != ESP_OK ||
        memcmp(signature, "OpusTags", sizeof(signature)) != 0) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t u32[4] = {};
    if ((ret = ogg_packet_read(&reader, u32, sizeof(u32))) != ESP_OK) {
        return ret;
    }
    const uint32_t vendor_length = read_le32(u32);
    if ((ret = ogg_packet_skip(&reader, vendor_length)) != ESP_OK ||
        (ret = ogg_packet_read(&reader, u32, sizeof(u32))) != ESP_OK) {
        return ret;
    }
    const uint32_t comment_count = read_le32(u32);
    if (comment_count > OGG_MAX_COMMENT_COUNT) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (uint32_t i = 0U; i < comment_count; ++i) {
        if ((ret = ogg_packet_read(&reader, u32, sizeof(u32))) != ESP_OK) {
            return ret;
        }
        const uint32_t comment_length = read_le32(u32);
        if (comment_length == 0U) {
            continue;
        }
        // 内嵌封面通常是很大的 METADATA_BLOCK_PICTURE Base64。本轮不支持内嵌封面，
        // 大 comment 直接沿 Ogg lacing 跳过，既不分配大块 PSRAM，也不影响后面的文本标签。
        if (comment_length > OGG_MAX_TEXT_COMMENT_BYTES) {
            ret = ogg_packet_skip(&reader, comment_length);
            if (ret != ESP_OK) {
                return ret;
            }
            continue;
        }

        char *comment = static_cast<char *>(
            heap_caps_malloc(static_cast<size_t>(comment_length) + 1U,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (comment == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        ret = ogg_packet_read(&reader, comment, comment_length);
        if (ret == ESP_OK) {
            comment[comment_length] = '\0';
            ret = callback(comment, comment_length, context);
        }
        heap_caps_free(comment);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}
