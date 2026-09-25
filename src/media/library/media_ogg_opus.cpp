#include "media_ogg_opus.h"

#include <limits.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"


static constexpr uint32_t OGG_MAX_COMMENT_COUNT = 4096U;
static constexpr uint32_t OGG_MAX_TEXT_COMMENT_BYTES = 16U * 1024U;
static constexpr size_t OGG_TAIL_SCAN_BYTES = 128U * 1024U;
static constexpr size_t OGG_BASE64_READ_BYTES = 8U * 1024U;
static constexpr uint32_t OGG_MAX_PICTURE_BYTES = 2U * 1024U * 1024U;
static constexpr char OGG_PICTURE_KEY[] = "METADATA_BLOCK_PICTURE=";

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

static uint32_t read_be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
        (static_cast<uint32_t>(p[1]) << 16) |
        (static_cast<uint32_t>(p[2]) << 8) |
        static_cast<uint32_t>(p[3]);
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
    uint64_t position = 0ULL;
    uint32_t serial = 0U;
    bool serial_valid = false;
    MediaOggOpusIoCallback io_callback = nullptr;
    void *io_context = nullptr;

    uint8_t lacing[255] = {};
    uint8_t segment_count = 0U;
    uint8_t segment_index = 0U;
    uint16_t segment_remaining = 0U;
    bool segment_ends_packet = false;
    bool page_loaded = false;
    bool packet_active = false;
    bool packet_ended = false;
};

static esp_err_t ogg_io_transfer(OggPacketReader *reader, void *buffer, uint32_t bytes, bool skip)
{
    if (reader == nullptr || reader->file == nullptr || reader->position > reader->file_size ||
        bytes > reader->file_size - reader->position) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = ESP_OK;
    if (reader->io_callback != nullptr) {
        ret = reader->io_callback(reader->file, buffer, bytes, skip, reader->io_context);
    } else if (skip) {
        if (bytes > static_cast<uint32_t>(LONG_MAX) || fseek(reader->file, static_cast<long>(bytes), SEEK_CUR) != 0) {
            ret = ESP_FAIL;
        }
    } else if (bytes > 0U && fread(buffer, 1, bytes, reader->file) != bytes) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    if (ret == ESP_OK) {
        reader->position += bytes;
    }
    return ret;
}

static esp_err_t ogg_load_page(OggPacketReader *reader, bool continuation_required, bool first_page)
{
    if (reader == nullptr || reader->file == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint64_t page_pos = reader->position;

    uint8_t header[27] = {};
    esp_err_t io_ret = ogg_io_transfer(reader, header, sizeof(header), false);
    if (io_ret != ESP_OK) {
        return io_ret;
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
    if (reader->segment_count > 0U) {
        io_ret = ogg_io_transfer(reader, reader->lacing, reader->segment_count, false);
        if (io_ret != ESP_OK) {
            return io_ret;
        }
    }

    uint32_t payload_bytes = 0U;
    for (uint16_t i = 0; i < reader->segment_count; ++i) {
        payload_bytes += reader->lacing[i];
    }
    const uint64_t payload_offset = page_pos + 27ULL + reader->segment_count;
    if (payload_offset > reader->file_size || payload_bytes > reader->file_size - payload_offset) {
        return ESP_ERR_INVALID_SIZE;
    }

    reader->page_loaded = true;
    return ESP_OK;
}

static esp_err_t ogg_begin_first_packet_ex(
    OggPacketReader *reader,
    FILE *file,
    uint64_t file_size,
    MediaOggOpusIoCallback io_callback,
    void *io_context,
    bool reset_file)
{
    if (reader == nullptr || file == nullptr || file_size < 27U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (reset_file && !seek_u64(file, 0U)) {
        return ESP_FAIL;
    }
    *reader = {};
    reader->file = file;
    reader->file_size = file_size;
    reader->io_callback = io_callback;
    reader->io_context = io_context;
    esp_err_t ret = ogg_load_page(reader, false, true);
    if (ret != ESP_OK) {
        return ret;
    }
    reader->packet_active = true;
    reader->packet_ended = false;
    return ESP_OK;
}

static esp_err_t ogg_begin_first_packet(OggPacketReader *reader, FILE *file, uint64_t file_size)
{
    return ogg_begin_first_packet_ex(reader, file, file_size, nullptr, nullptr, true);
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
        ret = ogg_io_transfer(reader, skip ? nullptr : out + done, chunk, skip);
        if (ret != ESP_OK) {
            return ret;
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

static esp_err_t ogg_begin_opus_tags(OggPacketReader *reader)
{
    if (reader == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ogg_read_opus_head(reader, nullptr, nullptr);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ogg_begin_next_packet(reader);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t signature[8] = {};
    ret = ogg_packet_read(reader, signature, sizeof(signature));
    if (ret != ESP_OK) {
        return ret;
    }
    return memcmp(signature, "OpusTags", sizeof(signature)) == 0
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t ogg_read_comment_header(OggPacketReader *reader, uint32_t *out_comment_count)
{
    if (reader == nullptr || out_comment_count == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t u32[4] = {};
    esp_err_t ret = ogg_packet_read(reader, u32, sizeof(u32));
    if (ret != ESP_OK) {
        return ret;
    }
    const uint32_t vendor_length = read_le32(u32);
    ret = ogg_packet_skip(reader, vendor_length);
    if (ret != ESP_OK || (ret = ogg_packet_read(reader, u32, sizeof(u32))) != ESP_OK) {
        return ret;
    }
    const uint32_t comment_count = read_le32(u32);
    if (comment_count > OGG_MAX_COMMENT_COUNT) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_comment_count = comment_count;
    return ESP_OK;
}

struct OggBase64Reader
{
    OggPacketReader *reader = nullptr;
    uint32_t encoded_remaining = 0U;
    uint8_t *encoded = nullptr;
    size_t encoded_capacity = 0U;
    size_t encoded_pos = 0U;
    size_t encoded_size = 0U;
    uint8_t decoded[3] = {};
    uint8_t decoded_pos = 0U;
    uint8_t decoded_size = 0U;
    bool padded = false;
};

static int base64_value(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + c - 'a';
    if (c >= '0' && c <= '9') return 52 + c - '0';
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool base64_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static esp_err_t base64_next_encoded(OggBase64Reader *stream, uint8_t *out)
{
    if (stream == nullptr || stream->reader == nullptr || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    while (stream->encoded_pos >= stream->encoded_size) {
        if (stream->encoded_remaining == 0U) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (stream->encoded == nullptr || stream->encoded_capacity == 0U) {
            return ESP_ERR_INVALID_STATE;
        }
        const uint32_t chunk = stream->encoded_remaining < stream->encoded_capacity
            ? stream->encoded_remaining
            : static_cast<uint32_t>(stream->encoded_capacity);
        const esp_err_t ret = ogg_packet_read(stream->reader, stream->encoded, chunk);
        if (ret != ESP_OK) {
            return ret;
        }
        stream->encoded_remaining -= chunk;
        stream->encoded_pos = 0U;
        stream->encoded_size = chunk;
    }
    *out = stream->encoded[stream->encoded_pos++];
    return ESP_OK;
}

static esp_err_t base64_fill_decoded(OggBase64Reader *stream)
{
    if (stream == nullptr || stream->padded) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t q[4] = {};
    uint8_t count = 0U;
    while (count < 4U) {
        uint8_t c = 0U;
        const esp_err_t ret = base64_next_encoded(stream, &c);
        if (ret != ESP_OK) {
            return ret;
        }
        if (base64_space(c)) {
            continue;
        }
        q[count++] = c;
    }

    const int a = base64_value(q[0]);
    const int b = base64_value(q[1]);
    if (a < 0 || b < 0 || q[0] == '=' || q[1] == '=') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const bool pad2 = q[2] == '=';
    const bool pad3 = q[3] == '=';
    const int c = pad2 ? 0 : base64_value(q[2]);
    const int d = pad3 ? 0 : base64_value(q[3]);
    if (c < 0 || d < 0 || (pad2 && !pad3)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    stream->decoded[0] = static_cast<uint8_t>((a << 2) | (b >> 4));
    stream->decoded[1] = static_cast<uint8_t>((b << 4) | (c >> 2));
    stream->decoded[2] = static_cast<uint8_t>((c << 6) | d);
    stream->decoded_pos = 0U;
    stream->decoded_size = pad2 ? 1U : (pad3 ? 2U : 3U);
    stream->padded = pad2 || pad3;
    return ESP_OK;
}

static esp_err_t base64_read(OggBase64Reader *stream, void *out, uint32_t bytes)
{
    if (stream == nullptr || (out == nullptr && bytes != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *dest = static_cast<uint8_t *>(out);
    uint32_t done = 0U;
    while (done < bytes) {
        if (stream->decoded_pos >= stream->decoded_size) {
            const esp_err_t ret = base64_fill_decoded(stream);
            if (ret != ESP_OK) {
                return ret;
            }
        }
        const uint32_t available = stream->decoded_size - stream->decoded_pos;
        const uint32_t chunk = (bytes - done) < available ? (bytes - done) : available;
        memcpy(dest + done, stream->decoded + stream->decoded_pos, chunk);
        stream->decoded_pos = static_cast<uint8_t>(stream->decoded_pos + chunk);
        done += chunk;
    }
    return ESP_OK;
}

static esp_err_t base64_skip_decoded(OggBase64Reader *stream, uint32_t bytes)
{
    uint8_t scratch[32] = {};
    uint32_t remaining = bytes;
    while (remaining > 0U) {
        const uint32_t chunk = remaining < sizeof(scratch) ? remaining : static_cast<uint32_t>(sizeof(scratch));
        const esp_err_t ret = base64_read(stream, scratch, chunk);
        if (ret != ESP_OK) {
            return ret;
        }
        remaining -= chunk;
    }
    return ESP_OK;
}

static MediaArtworkFormatV2 picture_format_from_prefix(const uint8_t *data, size_t size)
{
    if (data == nullptr) return MediaArtworkFormatV2::Unknown;
    if (size >= 8U && memcmp(data, "\x89PNG\x0D\x0A\x1A\x0A", 8U) == 0) {
        return MediaArtworkFormatV2::Png;
    }
    if (size >= 2U && data[0] == 0xFFU && data[1] == 0xD8U) {
        return MediaArtworkFormatV2::Jpeg;
    }
    return MediaArtworkFormatV2::Unknown;
}

static esp_err_t parse_picture_block(
    OggPacketReader *reader,
    uint32_t base64_bytes,
    uint32_t comment_index,
    uint8_t *out_data,
    uint32_t out_capacity,
    MediaOggOpusPictureInfo *out_picture)
{
    if (reader == nullptr || out_picture == nullptr || base64_bytes == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    OggBase64Reader stream = {};
    stream.reader = reader;
    stream.encoded_remaining = base64_bytes;
    stream.encoded = static_cast<uint8_t *>(
        heap_caps_malloc(OGG_BASE64_READ_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (stream.encoded == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    stream.encoded_capacity = OGG_BASE64_READ_BYTES;

    auto finish = [&](esp_err_t result) -> esp_err_t {
        esp_err_t final_result = result;
        if (stream.encoded_remaining > 0U) {
            const esp_err_t skip_ret = ogg_packet_skip(reader, stream.encoded_remaining);
            stream.encoded_remaining = 0U;
            if (skip_ret != ESP_OK) final_result = skip_ret;
        }
        heap_caps_free(stream.encoded);
        stream.encoded = nullptr;
        return final_result;
    };

    uint8_t u32[4] = {};
    esp_err_t ret = base64_read(&stream, u32, sizeof(u32));
    if (ret != ESP_OK) return finish(ret);
    const uint32_t picture_type = read_be32(u32);

    if ((ret = base64_read(&stream, u32, sizeof(u32))) != ESP_OK) return finish(ret);
    const uint32_t mime_length = read_be32(u32);
    if (mime_length > 4096U) return finish(ESP_ERR_INVALID_RESPONSE);
    if ((ret = base64_skip_decoded(&stream, mime_length)) != ESP_OK) return finish(ret);

    if ((ret = base64_read(&stream, u32, sizeof(u32))) != ESP_OK) return finish(ret);
    const uint32_t description_length = read_be32(u32);
    if (description_length > 1024U * 1024U) return finish(ESP_ERR_INVALID_RESPONSE);
    if ((ret = base64_skip_decoded(&stream, description_length)) != ESP_OK) return finish(ret);

    uint8_t fields[20] = {};
    if ((ret = base64_read(&stream, fields, sizeof(fields))) != ESP_OK) return finish(ret);
    const uint32_t width32 = read_be32(fields);
    const uint32_t height32 = read_be32(fields + 4U);
    const uint32_t data_size = read_be32(fields + 16U);
    if (data_size == 0U || data_size > OGG_MAX_PICTURE_BYTES) {
        return finish(ESP_ERR_INVALID_SIZE);
    }

    const uint32_t sniff_size = data_size < 24U ? data_size : 24U;
    uint8_t sniff[24] = {};
    if ((ret = base64_read(&stream, sniff, sniff_size)) != ESP_OK) return finish(ret);
    const MediaArtworkFormatV2 format = picture_format_from_prefix(sniff, sniff_size);
    if (format == MediaArtworkFormatV2::Unknown) {
        return finish(ESP_ERR_INVALID_RESPONSE);
    }

    if (out_data != nullptr) {
        if (out_capacity < data_size) {
            return finish(ESP_ERR_INVALID_SIZE);
        }
        memcpy(out_data, sniff, sniff_size);
        if (data_size > sniff_size) {
            ret = base64_read(&stream, out_data + sniff_size, data_size - sniff_size);
            if (ret != ESP_OK) return finish(ret);
        }
    }

    out_picture->comment_index = comment_index;
    out_picture->data_size = data_size;
    out_picture->width = width32 > 0U && width32 <= UINT16_MAX ? static_cast<uint16_t>(width32) : 0U;
    out_picture->height = height32 > 0U && height32 <= UINT16_MAX ? static_cast<uint16_t>(height32) : 0U;
    out_picture->format = format;
    out_picture->picture_type = picture_type <= UINT8_MAX ? static_cast<uint8_t>(picture_type) : 0U;
    return finish(ESP_OK);
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
        // 内嵌封面由独立流式接口处理；文本扫描对大 comment 直接沿 Ogg lacing 跳过，
        // 不分配大块 PSRAM，也不影响后面的普通文本标签。
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

static esp_err_t ogg_read_picture_comment_prefix(
    OggPacketReader *reader,
    uint32_t comment_length,
    bool *out_is_picture,
    uint32_t *out_base64_bytes)
{
    if (reader == nullptr || out_is_picture == nullptr || out_base64_bytes == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_is_picture = false;
    *out_base64_bytes = 0U;
    const uint32_t key_length = static_cast<uint32_t>(sizeof(OGG_PICTURE_KEY) - 1U);
    if (comment_length < key_length) {
        return ogg_packet_skip(reader, comment_length);
    }

    char prefix[sizeof(OGG_PICTURE_KEY) - 1U] = {};
    esp_err_t ret = ogg_packet_read(reader, prefix, key_length);
    if (ret != ESP_OK) {
        return ret;
    }
    if (strncasecmp(prefix, OGG_PICTURE_KEY, key_length) != 0) {
        return ogg_packet_skip(reader, comment_length - key_length);
    }
    *out_is_picture = true;
    *out_base64_bytes = comment_length - key_length;
    return ESP_OK;
}

esp_err_t media_ogg_opus_find_picture(
    FILE *file,
    uint64_t file_size,
    MediaOggOpusPictureInfo *out_picture)
{
    if (file == nullptr || out_picture == nullptr || file_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_picture = {};

    OggPacketReader reader = {};
    esp_err_t ret = ogg_begin_first_packet(&reader, file, file_size);
    if (ret != ESP_OK) return ret;
    ret = ogg_begin_opus_tags(&reader);
    if (ret != ESP_OK) return ret;

    uint32_t comment_count = 0U;
    ret = ogg_read_comment_header(&reader, &comment_count);
    if (ret != ESP_OK) return ret;

    bool have_selected = false;
    for (uint32_t i = 0U; i < comment_count; ++i) {
        uint8_t u32[4] = {};
        if ((ret = ogg_packet_read(&reader, u32, sizeof(u32))) != ESP_OK) return ret;
        const uint32_t comment_length = read_le32(u32);
        if (comment_length == 0U) continue;

        bool is_picture = false;
        uint32_t base64_bytes = 0U;
        ret = ogg_read_picture_comment_prefix(&reader, comment_length, &is_picture, &base64_bytes);
        if (ret != ESP_OK) return ret;
        if (!is_picture) continue;

        MediaOggOpusPictureInfo candidate = {};
        ret = parse_picture_block(&reader, base64_bytes, i, nullptr, 0U, &candidate);
        if (ret != ESP_OK) {
            // 单个损坏图片字段不阻断其余 metadata / 目录 fallback。
            if (ret == ESP_ERR_INVALID_RESPONSE || ret == ESP_ERR_INVALID_SIZE) {
                continue;
            }
            return ret;
        }

        const bool candidate_front = candidate.picture_type == 3U;
        const bool selected_front = have_selected && out_picture->picture_type == 3U;
        if (!have_selected || (!selected_front && candidate_front)) {
            *out_picture = candidate;
            have_selected = true;
        }
        if (candidate_front) {
            break;
        }
    }
    return have_selected ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t media_ogg_opus_read_picture(
    FILE *file,
    uint64_t file_size,
    uint32_t comment_index,
    uint8_t *out_data,
    uint32_t out_capacity,
    MediaOggOpusPictureInfo *out_picture,
    MediaOggOpusIoCallback io_callback,
    void *io_context)
{
    if (file == nullptr || out_data == nullptr || out_capacity == 0U || out_picture == nullptr || file_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_picture = {};

    OggPacketReader reader = {};
    const bool reset_file = io_callback == nullptr;
    esp_err_t ret = ogg_begin_first_packet_ex(
        &reader, file, file_size, io_callback, io_context, reset_file);
    if (ret != ESP_OK) return ret;
    ret = ogg_begin_opus_tags(&reader);
    if (ret != ESP_OK) return ret;

    uint32_t comment_count = 0U;
    ret = ogg_read_comment_header(&reader, &comment_count);
    if (ret != ESP_OK) return ret;
    if (comment_index >= comment_count) {
        return ESP_ERR_NOT_FOUND;
    }

    for (uint32_t i = 0U; i < comment_count; ++i) {
        uint8_t u32[4] = {};
        if ((ret = ogg_packet_read(&reader, u32, sizeof(u32))) != ESP_OK) return ret;
        const uint32_t comment_length = read_le32(u32);
        if (i != comment_index) {
            if ((ret = ogg_packet_skip(&reader, comment_length)) != ESP_OK) return ret;
            continue;
        }

        bool is_picture = false;
        uint32_t base64_bytes = 0U;
        ret = ogg_read_picture_comment_prefix(&reader, comment_length, &is_picture, &base64_bytes);
        if (ret != ESP_OK) return ret;
        if (!is_picture) return ESP_ERR_INVALID_RESPONSE;
        return parse_picture_block(
            &reader, base64_bytes, i, out_data, out_capacity, out_picture);
    }
    return ESP_ERR_NOT_FOUND;
}
