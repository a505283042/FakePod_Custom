#include "media_artwork.h"
#include "storage_io.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "media_catalog_v2.h"
#include "media_ogg_opus.h"
#include "app_diag_config.h"

#if APP_DIAG_LIBRARY_ARTWORK
static const char *TAG = "封面索引";
#endif
static constexpr size_t IMAGE_HEADER_SCAN_BYTES = 16U * 1024U;
static constexpr size_t ID3_APIC_PREFIX_SCAN_BYTES = 64U * 1024U;
// 首次建库只需要找到一个可靠封面 locator；异常文件不能无限遍历标签结构。
static constexpr uint32_t MAX_ARTWORK_ID3_FRAME_COUNT = 2048U;
static constexpr uint32_t MAX_ARTWORK_FLAC_BLOCK_COUNT = 256U;
static constexpr TickType_t LIBRARY_SD_LOCK_TIMEOUT = pdMS_TO_TICKS(2000);

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
    return file != nullptr && offset <= static_cast<uint64_t>(LONG_MAX) &&
        fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
}

static bool id3_frame_id_valid(const uint8_t *id, size_t length)
{
    if (id == nullptr) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9'))) {
            return false;
        }
    }
    return true;
}

static char *artwork_psram_strdup(const char *text)
{
    if (text == nullptr) {
        return nullptr;
    }
    const size_t length = strlen(text) + 1U;
    char *copy = static_cast<char *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy != nullptr) {
        memcpy(copy, text, length);
    }
    return copy;
}

void media_artwork_build_release_v2(MediaArtworkBuildV2 *artwork)
{
    if (artwork == nullptr) {
        return;
    }
    heap_caps_free(artwork->external_path);
    *artwork = {};
}

static bool jpeg_dimensions(const uint8_t *data, size_t size, uint16_t *out_width, uint16_t *out_height)
{
    if (data == nullptr || size < 4U || data[0] != 0xFFU || data[1] != 0xD8U) {
        return false;
    }
    size_t cursor = 2U;
    while (cursor + 4U <= size) {
        if (data[cursor] != 0xFFU) {
            ++cursor;
            continue;
        }
        while (cursor < size && data[cursor] == 0xFFU) {
            ++cursor;
        }
        if (cursor >= size) {
            break;
        }
        const uint8_t marker = data[cursor++];
        if (marker == 0xD8U || marker == 0xD9U || (marker >= 0xD0U && marker <= 0xD7U) || marker == 0x01U) {
            continue;
        }
        if (cursor + 2U > size) {
            break;
        }
        const uint16_t segment_size = static_cast<uint16_t>((data[cursor] << 8) | data[cursor + 1U]);
        if (segment_size < 2U || cursor + segment_size > size) {
            break;
        }
        const bool sof = (marker >= 0xC0U && marker <= 0xCFU) &&
            marker != 0xC4U && marker != 0xC8U && marker != 0xCCU;
        if (sof && segment_size >= 7U) {
            const uint16_t height = static_cast<uint16_t>((data[cursor + 3U] << 8) | data[cursor + 4U]);
            const uint16_t width = static_cast<uint16_t>((data[cursor + 5U] << 8) | data[cursor + 6U]);
            if (width != 0U && height != 0U) {
                if (out_width != nullptr) *out_width = width;
                if (out_height != nullptr) *out_height = height;
                return true;
            }
        }
        cursor += segment_size;
    }
    return false;
}

static void detect_image_buffer(
    const uint8_t *data,
    size_t size,
    MediaArtworkFormatV2 *out_format,
    uint16_t *out_width,
    uint16_t *out_height
)
{
    if (out_format != nullptr) *out_format = MediaArtworkFormatV2::Unknown;
    if (out_width != nullptr) *out_width = 0U;
    if (out_height != nullptr) *out_height = 0U;
    if (data == nullptr) {
        return;
    }
    if (size >= 24U &&
        memcmp(data, "\x89PNG\x0D\x0A\x1A\x0A", 8U) == 0 &&
        memcmp(data + 12U, "IHDR", 4U) == 0) {
        const uint32_t width = read_be32(data + 16U);
        const uint32_t height = read_be32(data + 20U);
        if (out_format != nullptr) *out_format = MediaArtworkFormatV2::Png;
        if (out_width != nullptr && width <= UINT16_MAX) *out_width = static_cast<uint16_t>(width);
        if (out_height != nullptr && height <= UINT16_MAX) *out_height = static_cast<uint16_t>(height);
        return;
    }
    if (size >= 2U && data[0] == 0xFFU && data[1] == 0xD8U) {
        if (out_format != nullptr) *out_format = MediaArtworkFormatV2::Jpeg;
        jpeg_dimensions(data, size, out_width, out_height);
    }
}

static size_t deunsync_prefix(const uint8_t *source, size_t source_size, uint8_t *dest, size_t dest_capacity)
{
    if (source == nullptr || dest == nullptr) {
        return 0U;
    }
    size_t out = 0U;
    for (size_t i = 0; i < source_size && out < dest_capacity; ++i) {
        dest[out++] = source[i];
        if (source[i] == 0xFFU && i + 1U < source_size && source[i + 1U] == 0x00U) {
            ++i;
        }
    }
    return out;
}

static bool inspect_image_payload(
    FILE *file,
    uint64_t offset,
    uint32_t size,
    bool id3_unsynchronised,
    MediaArtworkFormatV2 *out_format,
    uint16_t *out_width,
    uint16_t *out_height
)
{
    if (file == nullptr || size < 2U || !seek_u64(file, offset)) {
        return false;
    }
    const size_t read_size = size < IMAGE_HEADER_SCAN_BYTES ? size : IMAGE_HEADER_SCAN_BYTES;
    uint8_t *raw = static_cast<uint8_t *>(heap_caps_malloc(read_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (raw == nullptr) {
        return false;
    }
    const size_t got = fread(raw, 1, read_size, file);
    const uint8_t *view = raw;
    size_t view_size = got;
    uint8_t *clean = nullptr;
    if (id3_unsynchronised && got > 0U) {
        clean = static_cast<uint8_t *>(heap_caps_malloc(got, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (clean != nullptr) {
            view_size = deunsync_prefix(raw, got, clean, got);
            view = clean;
        }
    }
    MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
    uint16_t width = 0U;
    uint16_t height = 0U;
    detect_image_buffer(view, view_size, &format, &width, &height);
    heap_caps_free(clean);
    heap_caps_free(raw);
    if (format == MediaArtworkFormatV2::Unknown) {
        return false;
    }
    if (out_format != nullptr) *out_format = format;
    if (out_width != nullptr) *out_width = width;
    if (out_height != nullptr) *out_height = height;
    return true;
}

static bool assign_embedded_candidate(
    MediaArtworkBuildV2 *selected,
    MediaArtworkSourceV2 source,
    uint64_t data_offset,
    uint32_t data_size,
    uint32_t flags,
    uint16_t width,
    uint16_t height,
    MediaArtworkFormatV2 format,
    uint8_t picture_type
)
{
    if (selected == nullptr || data_size == 0U ||
        data_size > MEDIA_ARTWORK_MAX_COMPRESSED_BYTES_V2 ||
        format == MediaArtworkFormatV2::Unknown) {
        return false;
    }
    const bool have_selected = selected->source != MediaArtworkSourceV2::None;
    const bool selected_front = have_selected && selected->picture_type == 3U;
    const bool candidate_front = picture_type == 3U;
    if (have_selected && (selected_front || !candidate_front)) {
        return false;
    }
    media_artwork_build_release_v2(selected);
    selected->data_offset = data_offset;
    selected->data_size = data_size;
    selected->flags = flags;
    selected->width = width;
    selected->height = height;
    selected->source = source;
    selected->format = format;
    selected->picture_type = picture_type;
    return true;
}

static esp_err_t scan_flac_embedded(FILE *file, uint64_t file_size, MediaArtworkBuildV2 *selected)
{
    if (file == nullptr || selected == nullptr || !seek_u64(file, 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t first10[10] = {};
    const size_t got = fread(first10, 1, sizeof(first10), file);
    if (got < 4U) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t flac_offset = 0U;
    if (memcmp(first10, "ID3", 3U) == 0) {
        if (got < sizeof(first10) || ((first10[6] | first10[7] | first10[8] | first10[9]) & 0x80U) != 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t id3_size = read_synchsafe_u28(first10 + 6U);
        flac_offset = 10ULL + id3_size + ((first10[5] & 0x10U) != 0U ? 10ULL : 0ULL);
    }
    if (flac_offset + 4U > file_size || !seek_u64(file, flac_offset)) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t marker[4] = {};
    if (fread(marker, 1, sizeof(marker), file) != sizeof(marker) || memcmp(marker, "fLaC", 4U) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool last = false;
    uint32_t block_count = 0U;
    while (!last) {
        if (++block_count > MAX_ARTWORK_FLAC_BLOCK_COUNT) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint8_t header[4] = {};
        if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
            return ESP_ERR_INVALID_SIZE;
        }
        last = (header[0] & 0x80U) != 0U;
        const uint8_t type = header[0] & 0x7FU;
        const uint32_t length = read_be24(header + 1U);
        const long pos = ftell(file);
        if (pos < 0) {
            return ESP_FAIL;
        }
        const uint64_t block_offset = static_cast<uint64_t>(pos);
        const uint64_t block_end = block_offset + length;
        if (block_end > file_size || block_end < block_offset) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (type != 6U) {
            if (!seek_u64(file, block_end)) {
                return ESP_ERR_INVALID_SIZE;
            }
            continue;
        }
        if (length < 32U || !seek_u64(file, block_offset)) {
            if (!seek_u64(file, block_end)) return ESP_ERR_INVALID_SIZE;
            continue;
        }
        uint8_t u32[4] = {};
        if (fread(u32, 1, 4U, file) != 4U) return ESP_ERR_INVALID_SIZE;
        const uint32_t picture_type = read_be32(u32);
        if (fread(u32, 1, 4U, file) != 4U) return ESP_ERR_INVALID_SIZE;
        const uint32_t mime_len = read_be32(u32);
        const long mime_pos = ftell(file);
        if (mime_pos < 0 || static_cast<uint64_t>(mime_pos) + mime_len > block_end ||
            !seek_u64(file, static_cast<uint64_t>(mime_pos) + mime_len)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (fread(u32, 1, 4U, file) != 4U) return ESP_ERR_INVALID_SIZE;
        const uint32_t desc_len = read_be32(u32);
        const long desc_pos = ftell(file);
        if (desc_pos < 0 || static_cast<uint64_t>(desc_pos) + desc_len + 20ULL > block_end ||
            !seek_u64(file, static_cast<uint64_t>(desc_pos) + desc_len)) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t dimensions[16] = {};
        if (fread(dimensions, 1, sizeof(dimensions), file) != sizeof(dimensions) ||
            fread(u32, 1, 4U, file) != 4U) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t declared_width = read_be32(dimensions);
        const uint32_t declared_height = read_be32(dimensions + 4U);
        const uint32_t data_size = read_be32(u32);
        const long data_pos = ftell(file);
        if (data_pos < 0) {
            return ESP_FAIL;
        }
        const uint64_t data_offset = static_cast<uint64_t>(data_pos);
        if (data_size == 0U || data_offset + data_size > block_end || data_offset + data_size < data_offset) {
            if (!seek_u64(file, block_end)) return ESP_ERR_INVALID_SIZE;
            continue;
        }
        MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
        uint16_t sniff_width = 0U;
        uint16_t sniff_height = 0U;
        if (inspect_image_payload(file, data_offset, data_size, false, &format, &sniff_width, &sniff_height)) {
            const uint16_t width = declared_width <= UINT16_MAX && declared_width != 0U
                ? static_cast<uint16_t>(declared_width) : sniff_width;
            const uint16_t height = declared_height <= UINT16_MAX && declared_height != 0U
                ? static_cast<uint16_t>(declared_height) : sniff_height;
            assign_embedded_candidate(selected, MediaArtworkSourceV2::FlacPicture,
                data_offset, data_size, MEDIA_ARTWORK_REF_NONE_V2,
                width, height, format,
                picture_type <= UINT8_MAX ? static_cast<uint8_t>(picture_type) : 0U);
        }
        if (!seek_u64(file, block_end)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

static size_t id3_description_terminator(const uint8_t *data, size_t size, size_t start, uint8_t encoding)
{
    if (data == nullptr || start >= size) {
        return SIZE_MAX;
    }
    if (encoding == 0U || encoding == 3U) {
        for (size_t i = start; i < size; ++i) {
            if (data[i] == 0U) return i + 1U;
        }
        return SIZE_MAX;
    }
    for (size_t i = start; i + 1U < size; i += 2U) {
        if (data[i] == 0U && data[i + 1U] == 0U) return i + 2U;
    }
    return SIZE_MAX;
}

static bool parse_apic_payload_locator(
    FILE *file,
    uint64_t payload_offset,
    uint32_t payload_size,
    bool v22_pic,
    bool unsynchronised,
    uint64_t *out_data_offset,
    uint32_t *out_data_size,
    uint8_t *out_picture_type,
    MediaArtworkFormatV2 *out_format,
    uint16_t *out_width,
    uint16_t *out_height
)
{
    if (file == nullptr || payload_size < (v22_pic ? 6U : 5U) || !seek_u64(file, payload_offset)) {
        return false;
    }
    const size_t prefix_size = payload_size < ID3_APIC_PREFIX_SCAN_BYTES
        ? payload_size : ID3_APIC_PREFIX_SCAN_BYTES;
    uint8_t *prefix = static_cast<uint8_t *>(heap_caps_malloc(prefix_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (prefix == nullptr) {
        return false;
    }
    const bool read_ok = fread(prefix, 1, prefix_size, file) == prefix_size;
    if (!read_ok || prefix[0] > 3U) {
        heap_caps_free(prefix);
        return false;
    }
    const uint8_t encoding = prefix[0];
    size_t cursor = 1U;
    if (v22_pic) {
        if (cursor + 4U > prefix_size) {
            heap_caps_free(prefix);
            return false;
        }
        cursor += 3U; // image format "JPG" / "PNG"
    } else {
        while (cursor < prefix_size && prefix[cursor] != 0U) {
            ++cursor;
        }
        if (cursor >= prefix_size) {
            heap_caps_free(prefix);
            return false;
        }
        ++cursor; // MIME NUL
    }
    if (cursor >= prefix_size) {
        heap_caps_free(prefix);
        return false;
    }
    const uint8_t picture_type = prefix[cursor++];
    const size_t image_start = id3_description_terminator(prefix, prefix_size, cursor, encoding);
    if (image_start == SIZE_MAX || image_start >= payload_size) {
        heap_caps_free(prefix);
        return false;
    }
    const uint64_t data_offset = payload_offset + image_start;
    const uint32_t data_size = payload_size - static_cast<uint32_t>(image_start);
    MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
    uint16_t width = 0U;
    uint16_t height = 0U;
    const bool valid_image = inspect_image_payload(
        file, data_offset, data_size, unsynchronised, &format, &width, &height
    );
    heap_caps_free(prefix);
    if (!valid_image) {
        return false;
    }
    if (out_data_offset != nullptr) *out_data_offset = data_offset;
    if (out_data_size != nullptr) *out_data_size = data_size;
    if (out_picture_type != nullptr) *out_picture_type = picture_type;
    if (out_format != nullptr) *out_format = format;
    if (out_width != nullptr) *out_width = width;
    if (out_height != nullptr) *out_height = height;
    return true;
}

static esp_err_t scan_mp3_embedded(FILE *file, uint64_t file_size, MediaArtworkBuildV2 *selected)
{
    if (file == nullptr || selected == nullptr || !seek_u64(file, 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t header[10] = {};
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "ID3", 3U) != 0) {
        return ESP_OK;
    }
    if ((header[6] | header[7] | header[8] | header[9]) & 0x80U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint8_t version = header[3];
    const uint8_t flags = header[5];
    if (version < 2U || version > 4U) {
        return ESP_OK;
    }
    const bool tag_unsync = (flags & 0x80U) != 0U;
    // v2.2/v2.3 tag-level unsync 会让物理 frame 边界不再适合直接建立零拷贝 locator，安全跳过。
    if (tag_unsync && version < 4U) {
        return ESP_OK;
    }
    const uint32_t tag_size = read_synchsafe_u28(header + 6U);
    const uint64_t tag_end = 10ULL + tag_size;
    if (tag_end > file_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t cursor = 10U;
    if (version >= 3U && (flags & 0x40U) != 0U) {
        uint8_t size_bytes[4] = {};
        if (!seek_u64(file, cursor) || fread(size_bytes, 1, 4U, file) != 4U) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t ext_size = version == 4U ? read_synchsafe_u28(size_bytes) : read_be32(size_bytes);
        const uint64_t total_ext = version == 4U ? ext_size : static_cast<uint64_t>(ext_size) + 4ULL;
        if (total_ext < 4U || cursor + total_ext > tag_end) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        cursor += total_ext;
    }

    uint32_t frame_count = 0U;
    while (cursor < tag_end) {
        if (++frame_count > MAX_ARTWORK_ID3_FRAME_COUNT) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const size_t frame_header_size = version == 2U ? 6U : 10U;
        if (tag_end - cursor < frame_header_size || !seek_u64(file, cursor)) {
            break;
        }
        uint8_t frame_header[10] = {};
        if (fread(frame_header, 1, frame_header_size, file) != frame_header_size) {
            break;
        }
        const size_t id_len = version == 2U ? 3U : 4U;
        bool all_zero = true;
        for (size_t i = 0; i < id_len; ++i) all_zero = all_zero && frame_header[i] == 0U;
        if (all_zero || !id3_frame_id_valid(frame_header, id_len)) {
            break;
        }
        char frame_id[5] = {};
        memcpy(frame_id, frame_header, id_len);
        uint32_t frame_size = 0U;
        bool frame_unsync = tag_unsync;
        bool unsupported_transform = false;
        if (version == 2U) {
            frame_size = read_be24(frame_header + 3U);
        } else if (version == 3U) {
            frame_size = read_be32(frame_header + 4U);
            unsupported_transform = (frame_header[9] & (0x80U | 0x40U | 0x20U)) != 0U;
        } else {
            if ((frame_header[4] | frame_header[5] | frame_header[6] | frame_header[7]) & 0x80U) {
                break;
            }
            frame_size = read_synchsafe_u28(frame_header + 4U);
            unsupported_transform = (frame_header[9] & (0x40U | 0x08U | 0x04U | 0x01U)) != 0U;
            frame_unsync = frame_unsync || (frame_header[9] & 0x02U) != 0U;
        }
        const uint64_t payload_offset = cursor + frame_header_size;
        if (frame_size == 0U || payload_offset + frame_size > tag_end || payload_offset + frame_size < payload_offset) {
            break;
        }
        const bool is_apic = strcmp(frame_id, "APIC") == 0 || strcmp(frame_id, "PIC") == 0;
        if (is_apic && !unsupported_transform) {
            uint64_t data_offset = 0U;
            uint32_t data_size = 0U;
            uint8_t picture_type = 0U;
            MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
            uint16_t width = 0U;
            uint16_t height = 0U;
            if (parse_apic_payload_locator(file, payload_offset, frame_size, version == 2U,
                    frame_unsync, &data_offset, &data_size, &picture_type, &format, &width, &height)) {
                assign_embedded_candidate(selected, MediaArtworkSourceV2::Mp3Apic,
                    data_offset, data_size,
                    frame_unsync ? MEDIA_ARTWORK_REF_NEEDS_ID3_UNSYNC_V2 : MEDIA_ARTWORK_REF_NONE_V2,
                    width, height, format, picture_type);
            }
        }
        cursor = payload_offset + frame_size;
    }
    return ESP_OK;
}

static bool inspect_external_file(
    const char *path,
    uint32_t *out_size,
    int64_t *out_modified_time,
    MediaArtworkFormatV2 *out_format,
    uint16_t *out_width,
    uint16_t *out_height
)
{
    if (path == nullptr) {
        return false;
    }
    struct stat info = {};
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 1 ||
        static_cast<uint64_t>(info.st_size) > MEDIA_ARTWORK_MAX_COMPRESSED_BYTES_V2) {
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
    uint16_t width = 0U;
    uint16_t height = 0U;
    const bool ok = inspect_image_payload(file, 0U, static_cast<uint32_t>(info.st_size), false,
        &format, &width, &height);
    fclose(file);
    if (!ok) {
        return false;
    }
    if (out_size != nullptr) *out_size = static_cast<uint32_t>(info.st_size);
    if (out_modified_time != nullptr) *out_modified_time = static_cast<int64_t>(info.st_mtime);
    if (out_format != nullptr) *out_format = format;
    if (out_width != nullptr) *out_width = width;
    if (out_height != nullptr) *out_height = height;
    return true;
}

esp_err_t media_artwork_find_directory_fallback_v2(
    const char *directory,
    MediaArtworkBuildV2 *out_fallback
)
{
    if (directory == nullptr || directory[0] == '\0' || out_fallback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    media_artwork_build_release_v2(out_fallback);

    StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
    if (!sd_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    static constexpr const char *kCandidates[] = {
        "cover.jpg", "cover.jpeg", "cover.png",
        "folder.jpg", "folder.jpeg", "folder.png",
        "front.jpg", "front.jpeg", "front.png",
    };
    for (const char *name : kCandidates) {
        const size_t dir_len = strlen(directory);
        const size_t name_len = strlen(name);
        const bool slash = dir_len > 0U && directory[dir_len - 1U] != '/';
        const size_t total = dir_len + (slash ? 1U : 0U) + name_len + 1U;
        char *path = static_cast<char *>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (path == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        snprintf(path, total, "%s%s%s", directory, slash ? "/" : "", name);
        uint32_t size = 0U;
        int64_t modified_time = 0;
        MediaArtworkFormatV2 format = MediaArtworkFormatV2::Unknown;
        uint16_t width = 0U;
        uint16_t height = 0U;
        if (inspect_external_file(path, &size, &modified_time, &format, &width, &height)) {
            out_fallback->external_path = path;
            out_fallback->data_offset = 0U;
            out_fallback->data_size = size;
            out_fallback->source_modified_time = modified_time;
            out_fallback->source = MediaArtworkSourceV2::ExternalFile;
            out_fallback->format = format;
            out_fallback->picture_type = 3U;
            out_fallback->width = width;
            out_fallback->height = height;
#if APP_DIAG_LIBRARY_ARTWORK
            ESP_LOGI(TAG, "ARTWORK_TRACE: 目录 fallback=%s format=%u size=%lu %ux%u",
                path, static_cast<unsigned>(format), static_cast<unsigned long>(size),
                static_cast<unsigned>(width), static_cast<unsigned>(height));
#endif
            return ESP_OK;
        }
        heap_caps_free(path);
    }
    return ESP_OK;
}

static esp_err_t scan_opus_embedded(FILE *file, uint64_t file_size, MediaArtworkBuildV2 *selected)
{
    if (file == nullptr || selected == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    MediaOggOpusPictureInfo picture = {};
    const esp_err_t ret = media_ogg_opus_find_picture(file, file_size, &picture);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    assign_embedded_candidate(
        selected, MediaArtworkSourceV2::OpusPicture,
        picture.comment_index, picture.data_size, MEDIA_ARTWORK_REF_NONE_V2,
        picture.width, picture.height, picture.format, picture.picture_type);
    return ESP_OK;
}

esp_err_t media_artwork_scan_open_file_v2(
    FILE *file,
    const char *path,
    MediaFormat format,
    uint64_t file_size,
    const MediaArtworkBuildV2 *directory_fallback,
    MediaArtworkBuildV2 *out_artwork
)
{
    if (path == nullptr || out_artwork == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    media_artwork_build_release_v2(out_artwork);

    if (format == MediaFormat::MP3 || format == MediaFormat::FLAC || format == MediaFormat::OPUS) {
        if (file == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t embedded_ret = ESP_OK;
        if (format == MediaFormat::MP3) {
            embedded_ret = scan_mp3_embedded(file, file_size, out_artwork);
        } else if (format == MediaFormat::FLAC) {
            embedded_ret = scan_flac_embedded(file, file_size, out_artwork);
        } else {
            embedded_ret = scan_opus_embedded(file, file_size, out_artwork);
        }
        if (embedded_ret == ESP_ERR_NO_MEM) {
            return embedded_ret;
        }
        // 单个损坏 APIC/PICTURE 不阻断 fallback；真正图片 locator 仍需 magic 校验才会进入结果。
        if (out_artwork->source != MediaArtworkSourceV2::None) {
#if APP_DIAG_LIBRARY_ARTWORK
            ESP_LOGI(TAG, "ARTWORK_TRACE: embedded source=%u type=%u format=%u offset=%llu size=%lu %ux%u path=%s",
                static_cast<unsigned>(out_artwork->source), static_cast<unsigned>(out_artwork->picture_type),
                static_cast<unsigned>(out_artwork->format), static_cast<unsigned long long>(out_artwork->data_offset),
                static_cast<unsigned long>(out_artwork->data_size), static_cast<unsigned>(out_artwork->width),
                static_cast<unsigned>(out_artwork->height), path);
#endif
            return ESP_OK;
        }
    }

    if (directory_fallback != nullptr && directory_fallback->source == MediaArtworkSourceV2::ExternalFile &&
        directory_fallback->external_path != nullptr && directory_fallback->external_path[0] != '\0') {
        out_artwork->external_path = artwork_psram_strdup(directory_fallback->external_path);
        if (out_artwork->external_path == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        out_artwork->data_offset = 0U;
        out_artwork->data_size = directory_fallback->data_size;
        out_artwork->source_modified_time = directory_fallback->source_modified_time;
        out_artwork->flags = MEDIA_ARTWORK_REF_NONE_V2;
        out_artwork->source = MediaArtworkSourceV2::ExternalFile;
        out_artwork->format = directory_fallback->format;
        out_artwork->width = directory_fallback->width;
        out_artwork->height = directory_fallback->height;
        out_artwork->picture_type = 3U;
#if APP_DIAG_LIBRARY_ARTWORK
        ESP_LOGI(TAG, "ARTWORK_TRACE: external format=%u size=%lu %ux%u path=%s",
            static_cast<unsigned>(out_artwork->format), static_cast<unsigned long>(out_artwork->data_size),
            static_cast<unsigned>(out_artwork->width), static_cast<unsigned>(out_artwork->height),
            out_artwork->external_path);
#endif
    }
    return ESP_OK;
}

esp_err_t media_artwork_scan_file_v2(
    const char *path,
    MediaFormat format,
    uint64_t file_size,
    const MediaArtworkBuildV2 *directory_fallback,
    MediaArtworkBuildV2 *out_artwork
)
{
    if (path == nullptr || out_artwork == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    StorageSdLockGuard sd_lock(LIBRARY_SD_LOCK_TIMEOUT);
    if (!sd_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }

    if (format != MediaFormat::MP3 && format != MediaFormat::FLAC && format != MediaFormat::OPUS) {
        return media_artwork_scan_open_file_v2(
            nullptr, path, format, file_size, directory_fallback, out_artwork);
    }

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return ESP_FAIL;
    }
    const esp_err_t ret = media_artwork_scan_open_file_v2(
        file, path, format, file_size, directory_fallback, out_artwork);
    fclose(file);
    return ret;
}

esp_err_t media_artwork_catalog_reuse_unchanged_v2(
    const MusicCatalogV2 *catalog,
    uint32_t track_index,
    const MediaArtworkBuildV2 *current_directory_fallback,
    bool *out_unchanged
)
{
    if (catalog == nullptr || out_unchanged == nullptr || track_index >= catalog->track_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_unchanged = false;
    const TrackRowV2 &track = catalog->tracks[track_index];

    if (track.artwork_ref_id != MEDIA_CATALOG_INVALID_ID_V2) {
        if (track.artwork_ref_id >= catalog->artwork_ref_count || catalog->artwork_refs == nullptr) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const ArtworkRefV2 &source = catalog->artwork_refs[track.artwork_ref_id];
        if (source.source == MediaArtworkSourceV2::Mp3Apic ||
            source.source == MediaArtworkSourceV2::FlacPicture ||
            source.source == MediaArtworkSourceV2::OpusPicture) {
            *out_unchanged = true;
            return ESP_OK;
        }
    }

    const bool have_current_external = current_directory_fallback != nullptr &&
        current_directory_fallback->source == MediaArtworkSourceV2::ExternalFile &&
        current_directory_fallback->external_path != nullptr &&
        current_directory_fallback->external_path[0] != '\0';

    const ArtworkRefV2 *old_ref = nullptr;
    const char *old_external_path = nullptr;
    if (track.artwork_ref_id != MEDIA_CATALOG_INVALID_ID_V2) {
        old_ref = &catalog->artwork_refs[track.artwork_ref_id];
        if (old_ref->source != MediaArtworkSourceV2::ExternalFile) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        old_external_path = media_catalog_v2_pool_str(catalog, old_ref->path_off);
        if (old_external_path == nullptr || old_external_path[0] == '\0') {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (!have_current_external) {
        *out_unchanged = old_ref == nullptr;
        return ESP_OK;
    }

    *out_unchanged = old_ref != nullptr &&
        strcmp(old_external_path, current_directory_fallback->external_path) == 0 &&
        old_ref->data_size == current_directory_fallback->data_size &&
        old_ref->source_modified_time == current_directory_fallback->source_modified_time &&
        old_ref->format == current_directory_fallback->format &&
        old_ref->width == current_directory_fallback->width &&
        old_ref->height == current_directory_fallback->height;
    return ESP_OK;
}

esp_err_t media_artwork_clone_exact_from_catalog_v2(
    const MusicCatalogV2 *catalog,
    uint32_t track_index,
    MediaArtworkBuildV2 *out_artwork
)
{
    if (catalog == nullptr || out_artwork == nullptr || track_index >= catalog->track_count) {
        return ESP_ERR_INVALID_ARG;
    }
    media_artwork_build_release_v2(out_artwork);
    const TrackRowV2 &track = catalog->tracks[track_index];
    if (track.artwork_ref_id == MEDIA_CATALOG_INVALID_ID_V2) {
        return ESP_OK;
    }
    if (track.artwork_ref_id >= catalog->artwork_ref_count || catalog->artwork_refs == nullptr) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const ArtworkRefV2 &source = catalog->artwork_refs[track.artwork_ref_id];
    out_artwork->data_offset = source.data_offset;
    out_artwork->data_size = source.data_size;
    out_artwork->source_modified_time = source.source_modified_time;
    out_artwork->flags = source.flags;
    out_artwork->width = source.width;
    out_artwork->height = source.height;
    out_artwork->source = source.source;
    out_artwork->format = source.format;
    out_artwork->picture_type = source.picture_type;

    if (source.source == MediaArtworkSourceV2::ExternalFile) {
        const char *path = media_catalog_v2_pool_str(catalog, source.path_off);
        if (path == nullptr || path[0] == '\0') {
            media_artwork_build_release_v2(out_artwork);
            return ESP_ERR_INVALID_RESPONSE;
        }
        out_artwork->external_path = artwork_psram_strdup(path);
        if (out_artwork->external_path == nullptr) {
            media_artwork_build_release_v2(out_artwork);
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t media_artwork_clone_from_catalog_v2(
    const MusicCatalogV2 *catalog,
    uint32_t track_index,
    const MediaArtworkBuildV2 *current_directory_fallback,
    MediaArtworkBuildV2 *out_artwork,
    bool *out_unchanged
)
{
    if (catalog == nullptr || out_artwork == nullptr || out_unchanged == nullptr ||
        track_index >= catalog->track_count) {
        return ESP_ERR_INVALID_ARG;
    }
    media_artwork_build_release_v2(out_artwork);
    *out_unchanged = false;
    const TrackRowV2 &track = catalog->tracks[track_index];

    // 当前 schema 已证明：如果旧结果是 Embedded，它只会随音频文件签名变化而失效。
    if (track.artwork_ref_id != MEDIA_CATALOG_INVALID_ID_V2) {
        if (track.artwork_ref_id >= catalog->artwork_ref_count || catalog->artwork_refs == nullptr) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const ArtworkRefV2 &source = catalog->artwork_refs[track.artwork_ref_id];
        if (source.source == MediaArtworkSourceV2::Mp3Apic || source.source == MediaArtworkSourceV2::FlacPicture ||
            source.source == MediaArtworkSourceV2::OpusPicture) {
            out_artwork->data_offset = source.data_offset;
            out_artwork->data_size = source.data_size;
            out_artwork->source_modified_time = 0;
            out_artwork->flags = source.flags;
            out_artwork->width = source.width;
            out_artwork->height = source.height;
            out_artwork->source = source.source;
            out_artwork->format = source.format;
            out_artwork->picture_type = source.picture_type;
            *out_unchanged = true;
            return ESP_OK;
        }
    }

    // 旧结果不是内嵌封面，说明该音频文件在当前 schema 下已经确认没有 APIC/PICTURE。
    // 因此目录 fallback 改变时只刷新 locator，不重新打开每首音频文件。
    const bool have_current_external = current_directory_fallback != nullptr &&
        current_directory_fallback->source == MediaArtworkSourceV2::ExternalFile &&
        current_directory_fallback->external_path != nullptr && current_directory_fallback->external_path[0] != '\0';
    const ArtworkRefV2 *old_ref = nullptr;
    const char *old_external_path = nullptr;
    if (track.artwork_ref_id != MEDIA_CATALOG_INVALID_ID_V2) {
        old_ref = &catalog->artwork_refs[track.artwork_ref_id];
        if (old_ref->source != MediaArtworkSourceV2::ExternalFile) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        old_external_path = media_catalog_v2_pool_str(catalog, old_ref->path_off);
        if (old_external_path == nullptr || old_external_path[0] == '\0') {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (!have_current_external) {
        *out_unchanged = old_ref == nullptr;
        return ESP_OK;
    }

    out_artwork->external_path = artwork_psram_strdup(current_directory_fallback->external_path);
    if (out_artwork->external_path == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    out_artwork->data_offset = 0U;
    out_artwork->data_size = current_directory_fallback->data_size;
    out_artwork->source_modified_time = current_directory_fallback->source_modified_time;
    out_artwork->flags = MEDIA_ARTWORK_REF_NONE_V2;
    out_artwork->width = current_directory_fallback->width;
    out_artwork->height = current_directory_fallback->height;
    out_artwork->source = MediaArtworkSourceV2::ExternalFile;
    out_artwork->format = current_directory_fallback->format;
    out_artwork->picture_type = 3U;

    *out_unchanged = old_ref != nullptr &&
        strcmp(old_external_path, current_directory_fallback->external_path) == 0 &&
        old_ref->data_size == current_directory_fallback->data_size &&
        old_ref->source_modified_time == current_directory_fallback->source_modified_time &&
        old_ref->format == current_directory_fallback->format &&
        old_ref->width == current_directory_fallback->width &&
        old_ref->height == current_directory_fallback->height;
    return ESP_OK;
}
