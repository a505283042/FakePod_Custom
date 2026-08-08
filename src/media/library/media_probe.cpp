#include "media_probe.h"
#include "storage_io.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "媒体探针";
static constexpr size_t MP3_SCAN_BUFFER_BYTES = 8192;
static constexpr size_t MP3_MAX_LEADING_SCAN_BYTES = 256 * 1024;

static uint32_t read_be24(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) |
           static_cast<uint32_t>(p[2]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

static uint32_t read_synchsafe_u28(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0] & 0x7FU) << 21) |
           (static_cast<uint32_t>(p[1] & 0x7FU) << 14) |
           (static_cast<uint32_t>(p[2] & 0x7FU) << 7) |
           static_cast<uint32_t>(p[3] & 0x7FU);
}

static bool seek_u64(FILE *file, uint64_t offset)
{
    if (file == nullptr || offset > static_cast<uint64_t>(LONG_MAX)) {
        return false;
    }
    return fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
}

static bool skip_bytes(FILE *file, uint64_t bytes)
{
    const long current = file != nullptr ? ftell(file) : -1;
    if (current < 0) {
        return false;
    }
    return seek_u64(file, static_cast<uint64_t>(current) + bytes);
}

static esp_err_t probe_flac_picture_block(
    FILE *file,
    uint64_t block_data_offset,
    uint32_t block_length,
    MediaTechnicalInfo *info
)
{
    if (file == nullptr || info == nullptr || block_length < 32U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!seek_u64(file, block_data_offset)) {
        return ESP_FAIL;
    }

    uint8_t u32buf[4] = {};
    if (fread(u32buf, 1, 4, file) != 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    // picture type 当前只需跳过，后续做封面优先级时再使用。

    if (fread(u32buf, 1, 4, file) != 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t mime_length = read_be32(u32buf);
    if (!skip_bytes(file, mime_length)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (fread(u32buf, 1, 4, file) != 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t description_length = read_be32(u32buf);
    if (!skip_bytes(file, description_length)) {
        return ESP_ERR_INVALID_SIZE;
    }

    // width / height / depth / indexed colors
    if (!skip_bytes(file, 16)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (fread(u32buf, 1, 4, file) != 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t artwork_size = read_be32(u32buf);
    const long artwork_offset = ftell(file);
    if (artwork_offset < 0) {
        return ESP_FAIL;
    }

    const uint64_t block_end = block_data_offset + block_length;
    const uint64_t artwork_end = static_cast<uint64_t>(artwork_offset) + artwork_size;
    if (artwork_end > block_end) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 第一张合法内嵌图先记录；后续可以根据 picture type 决定 Front Cover 优先级。
    if ((info->flags & MEDIA_TECH_HAS_ARTWORK) == 0U) {
        info->artwork_offset = static_cast<uint64_t>(artwork_offset);
        info->artwork_size = artwork_size;
        info->flags |= MEDIA_TECH_HAS_ARTWORK;
    }
    return ESP_OK;
}

static esp_err_t probe_flac(FILE *file, MediaTechnicalInfo *info)
{
    uint8_t first10[10] = {};
    const size_t first_read = fread(first10, 1, sizeof(first10), file);
    if (first_read < 4) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t flac_offset = 0;
    if (memcmp(first10, "ID3", 3) == 0) {
        if (first_read < sizeof(first10)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if ((first10[6] | first10[7] | first10[8] | first10[9]) & 0x80U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t tag_size = read_synchsafe_u28(&first10[6]);
        const bool has_footer = (first10[5] & 0x10U) != 0;
        flac_offset = 10ULL + tag_size + (has_footer ? 10ULL : 0ULL);
    }

    if (!seek_u64(file, flac_offset)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t marker[4] = {};
    if (fread(marker, 1, 4, file) != 4 || memcmp(marker, "fLaC", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool have_streaminfo = false;
    bool last = false;
    while (!last) {
        uint8_t header[4] = {};
        if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
            return ESP_ERR_INVALID_SIZE;
        }

        last = (header[0] & 0x80U) != 0;
        const uint8_t type = header[0] & 0x7FU;
        const uint32_t length = read_be24(&header[1]);
        const long block_position = ftell(file);
        if (block_position < 0) {
            return ESP_FAIL;
        }
        const uint64_t block_data_offset = static_cast<uint64_t>(block_position);

        if (type == 0U) {
            if (length != 34U) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            uint8_t streaminfo[34] = {};
            if (fread(streaminfo, 1, sizeof(streaminfo), file) != sizeof(streaminfo)) {
                return ESP_ERR_INVALID_SIZE;
            }

            info->max_block_size = static_cast<uint16_t>(
                (static_cast<uint16_t>(streaminfo[2]) << 8) | streaminfo[3]
            );
            info->max_frame_size = read_be24(&streaminfo[7]);
            info->sample_rate_hz =
                (static_cast<uint32_t>(streaminfo[10]) << 12) |
                (static_cast<uint32_t>(streaminfo[11]) << 4) |
                (static_cast<uint32_t>(streaminfo[12]) >> 4);
            info->channels = static_cast<uint8_t>(((streaminfo[12] >> 1) & 0x07U) + 1U);
            info->bits_per_sample = static_cast<uint8_t>(
                (((static_cast<uint16_t>(streaminfo[12]) & 0x01U) << 4) |
                 (streaminfo[13] >> 4)) + 1U
            );
            info->total_frames =
                (static_cast<uint64_t>(streaminfo[13] & 0x0FU) << 32) |
                (static_cast<uint64_t>(streaminfo[14]) << 24) |
                (static_cast<uint64_t>(streaminfo[15]) << 16) |
                (static_cast<uint64_t>(streaminfo[16]) << 8) |
                static_cast<uint64_t>(streaminfo[17]);

            have_streaminfo = info->sample_rate_hz != 0 && info->channels != 0;
        } else if (type == 6U) {
            const esp_err_t picture_ret = probe_flac_picture_block(
                file,
                block_data_offset,
                length,
                info
            );
            if (picture_ret != ESP_OK) {
                // 封面损坏不应该让整首音乐无法建立音频索引。
                info->artwork_offset = 0;
                info->artwork_size = 0;
                info->flags &= ~MEDIA_TECH_HAS_ARTWORK;
            }
            if (!seek_u64(file, block_data_offset + length)) {
                return ESP_ERR_INVALID_SIZE;
            }
        } else {
            if (!seek_u64(file, block_data_offset + length)) {
                return ESP_ERR_INVALID_SIZE;
            }
        }
    }

    const long audio_offset = ftell(file);
    if (!have_streaminfo || audio_offset < 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    info->audio_data_offset = static_cast<uint64_t>(audio_offset);
    info->metadata_end_offset = info->audio_data_offset;
    info->duration_ms = info->sample_rate_hz > 0
        ? static_cast<uint32_t>((info->total_frames * 1000ULL) / info->sample_rate_hz)
        : 0;
    info->flags |= MEDIA_TECH_PARSED;
    return ESP_OK;
}

struct Mp3FrameHeader
{
    uint32_t sample_rate_hz = 0;
    uint32_t bitrate_kbps = 0;
    uint32_t frame_size_bytes = 0;
    uint16_t samples_per_frame = 0;
    uint8_t channels = 0;
    uint8_t version_id = 0;
    bool has_crc = false;
};

static bool mp3_parse_frame_header(const uint8_t *h, Mp3FrameHeader *out)
{
    if (h == nullptr || out == nullptr) {
        return false;
    }
    if (h[0] != 0xFFU || (h[1] & 0xE0U) != 0xE0U) {
        return false;
    }

    const uint8_t version_id = (h[1] >> 3) & 0x03U;
    const uint8_t layer = (h[1] >> 1) & 0x03U;
    if (version_id == 1U || layer != 1U) {
        return false;
    }

    const uint8_t bitrate_index = (h[2] >> 4) & 0x0FU;
    const uint8_t sample_index = (h[2] >> 2) & 0x03U;
    if (bitrate_index == 0U || bitrate_index == 15U || sample_index == 3U) {
        return false;
    }

    static constexpr uint16_t kBitrateMpeg1[16] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
    };
    static constexpr uint16_t kBitrateMpeg2[16] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
    };
    static constexpr uint32_t kMpeg1Rates[3] = {44100, 48000, 32000};

    uint32_t sample_rate = kMpeg1Rates[sample_index];
    if (version_id == 2U) {
        sample_rate /= 2U;
    } else if (version_id == 0U) {
        sample_rate /= 4U;
    }

    const bool mpeg1 = version_id == 3U;
    const uint32_t bitrate = mpeg1 ? kBitrateMpeg1[bitrate_index] : kBitrateMpeg2[bitrate_index];
    const uint32_t padding = (h[2] >> 1) & 0x01U;
    const uint32_t frame_size =
        ((mpeg1 ? 144000U : 72000U) * bitrate) / sample_rate + padding;
    if (frame_size < 24U) {
        return false;
    }

    out->sample_rate_hz = sample_rate;
    out->bitrate_kbps = bitrate;
    out->frame_size_bytes = frame_size;
    out->samples_per_frame = mpeg1 ? 1152U : 576U;
    out->channels = ((h[3] >> 6) & 0x03U) == 3U ? 1U : 2U;
    out->version_id = version_id;
    out->has_crc = (h[1] & 0x01U) == 0U;
    return true;
}

static bool mp3_headers_compatible(const Mp3FrameHeader &a, const Mp3FrameHeader &b)
{
    return a.sample_rate_hz == b.sample_rate_hz &&
           a.samples_per_frame == b.samples_per_frame &&
           a.version_id == b.version_id;
}

static esp_err_t mp3_find_first_frame(
    FILE *file,
    uint64_t search_start,
    uint64_t file_size,
    uint64_t *out_offset,
    Mp3FrameHeader *out_header
)
{
    if (file == nullptr || out_offset == nullptr || out_header == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(MP3_SCAN_BUFFER_BYTES + 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (buffer == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const uint64_t search_limit = search_start + MP3_MAX_LEADING_SCAN_BYTES < file_size
        ? search_start + MP3_MAX_LEADING_SCAN_BYTES
        : file_size;
    uint64_t cursor = search_start;
    esp_err_t result = ESP_ERR_NOT_FOUND;

    while (cursor + 4 <= search_limit) {
        if (!seek_u64(file, cursor)) {
            result = ESP_FAIL;
            break;
        }
        const size_t max_read = static_cast<size_t>(
            (search_limit - cursor) < MP3_SCAN_BUFFER_BYTES
                ? (search_limit - cursor)
                : MP3_SCAN_BUFFER_BYTES
        );
        const size_t got = fread(buffer, 1, max_read, file);
        if (got < 4) {
            break;
        }

        for (size_t i = 0; i + 4 <= got; ++i) {
            Mp3FrameHeader first = {};
            if (!mp3_parse_frame_header(buffer + i, &first)) {
                continue;
            }

            const uint64_t frame_offset = cursor + i;
            const uint64_t next_offset = frame_offset + first.frame_size_bytes;
            if (next_offset + 4 > file_size || !seek_u64(file, next_offset)) {
                continue;
            }

            uint8_t next_bytes[4] = {};
            if (fread(next_bytes, 1, sizeof(next_bytes), file) != sizeof(next_bytes)) {
                continue;
            }
            Mp3FrameHeader next = {};
            if (!mp3_parse_frame_header(next_bytes, &next) || !mp3_headers_compatible(first, next)) {
                continue;
            }

            *out_offset = frame_offset;
            *out_header = first;
            result = ESP_OK;
            break;
        }

        if (result == ESP_OK) {
            break;
        }
        if (got < max_read) {
            break;
        }
        // 保留 3 字节重叠，避免帧头恰好跨块边界。
        cursor += got > 3 ? got - 3 : got;
    }

    heap_caps_free(buffer);
    return result;
}

static uint32_t mp3_side_info_size(const Mp3FrameHeader &header)
{
    const bool mpeg1 = header.version_id == 3U;
    if (mpeg1) {
        return header.channels == 1U ? 17U : 32U;
    }
    return header.channels == 1U ? 9U : 17U;
}

static bool mp3_probe_xing(
    FILE *file,
    uint64_t frame_offset,
    const Mp3FrameHeader &header,
    uint32_t *out_frame_count
)
{
    const uint64_t offset = frame_offset + 4U + (header.has_crc ? 2U : 0U) + mp3_side_info_size(header);
    if (!seek_u64(file, offset)) {
        return false;
    }

    uint8_t prefix[12] = {};
    if (fread(prefix, 1, sizeof(prefix), file) != sizeof(prefix)) {
        return false;
    }
    if (memcmp(prefix, "Xing", 4) != 0 && memcmp(prefix, "Info", 4) != 0) {
        return false;
    }

    const uint32_t flags = read_be32(&prefix[4]);
    if ((flags & 0x00000001U) == 0U) {
        return false;
    }
    *out_frame_count = read_be32(&prefix[8]);
    return *out_frame_count != 0U;
}

static bool mp3_probe_vbri(FILE *file, uint64_t frame_offset, uint32_t *out_frame_count)
{
    // Fraunhofer VBRI 位于 MPEG 音频帧头之后固定 32 字节处，即帧起点 + 36。
    if (!seek_u64(file, frame_offset + 36U)) {
        return false;
    }
    uint8_t header[18] = {};
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "VBRI", 4) != 0) {
        return false;
    }
    *out_frame_count = read_be32(&header[14]);
    return *out_frame_count != 0U;
}

static esp_err_t probe_mp3(FILE *file, uint64_t file_size, MediaTechnicalInfo *info)
{
    uint8_t first10[10] = {};
    if (!seek_u64(file, 0)) {
        return ESP_FAIL;
    }
    const size_t first_read = fread(first10, 1, sizeof(first10), file);
    if (first_read < 4) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t search_start = 0;
    if (memcmp(first10, "ID3", 3) == 0) {
        if (first_read < sizeof(first10)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if ((first10[6] | first10[7] | first10[8] | first10[9]) & 0x80U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t tag_size = read_synchsafe_u28(&first10[6]);
        const bool has_footer = (first10[5] & 0x10U) != 0;
        search_start = 10ULL + tag_size + (has_footer ? 10ULL : 0ULL);
    }

    uint64_t first_frame_offset = 0;
    Mp3FrameHeader frame = {};
    esp_err_t ret = mp3_find_first_frame(
        file,
        search_start,
        file_size,
        &first_frame_offset,
        &frame
    );
    if (ret != ESP_OK) {
        return ret;
    }

    info->sample_rate_hz = frame.sample_rate_hz;
    info->bitrate_kbps = frame.bitrate_kbps;
    info->samples_per_frame = frame.samples_per_frame;
    info->channels = frame.channels;
    info->bits_per_sample = 16;
    info->audio_data_offset = first_frame_offset;
    info->metadata_end_offset = first_frame_offset;

    uint32_t frame_count = 0;
    if (
        mp3_probe_xing(file, first_frame_offset, frame, &frame_count) ||
        mp3_probe_vbri(file, first_frame_offset, &frame_count)
    ) {
        info->flags |= MEDIA_TECH_HAS_VBR_HEADER;
        info->total_frames = static_cast<uint64_t>(frame_count) * frame.samples_per_frame;
        info->duration_ms = static_cast<uint32_t>(
            (info->total_frames * 1000ULL) / frame.sample_rate_hz
        );
    } else if (frame.bitrate_kbps > 0 && file_size > first_frame_offset) {
        // 无 Xing/VBRI 时只做明确标记的 CBR 近似，后续 seek 阶段可进一步升级为稀疏帧索引。
        const uint64_t audio_bytes = file_size - first_frame_offset;
        const uint64_t duration_ms = (audio_bytes * 8ULL) / frame.bitrate_kbps;
        info->duration_ms = duration_ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(duration_ms);
        info->total_frames =
            (static_cast<uint64_t>(info->duration_ms) * frame.sample_rate_hz) / 1000ULL;
        info->flags |= MEDIA_TECH_DURATION_ESTIMATED;
    }

    info->flags |= MEDIA_TECH_PARSED;
    return ESP_OK;
}

esp_err_t media_probe_file(
    const char *path,
    MediaFormat format,
    MediaTechnicalInfo *out_info
)
{
    if (path == nullptr || out_info == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_info = {};
    if (format != MediaFormat::FLAC && format != MediaFormat::MP3) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    StorageSdLockGuard sd_lock;
    if (!sd_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    uint64_t file_size = 0;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long end = ftell(file);
        if (end >= 0) {
            file_size = static_cast<uint64_t>(end);
        }
    }
    rewind(file);

    esp_err_t ret = ESP_FAIL;
    if (format == MediaFormat::FLAC) {
        ret = probe_flac(file, out_info);
    } else {
        ret = probe_mp3(file, file_size, out_info);
    }

    fclose(file);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "技术信息解析失败：%s [%s] ret=%s",
            path,
            media_format_name(format),
            esp_err_to_name(ret));
    }
    return ret;
}
