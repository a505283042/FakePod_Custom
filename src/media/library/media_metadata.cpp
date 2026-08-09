#include "media_metadata.h"
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

static const char *TAG = "元数据扫描";
static constexpr size_t MAX_TEXT_FRAME_BYTES = 64U * 1024U;
static constexpr size_t MAX_TAG_TEXT_BYTES = 16U * 1024U;
static constexpr size_t MAX_VORBIS_KEY_BYTES = 128U;

static void *metadata_alloc(size_t bytes)
{
    if (bytes == 0) {
        return nullptr;
    }
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *metadata_realloc(void *ptr, size_t bytes)
{
    return heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static char *metadata_strdup_n(const char *text, size_t length)
{
    if (text == nullptr || length > SIZE_MAX - 1U) {
        return nullptr;
    }
    char *copy = static_cast<char *>(metadata_alloc(length + 1U));
    if (copy == nullptr) {
        return nullptr;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static char *metadata_strdup(const char *text)
{
    return text != nullptr ? metadata_strdup_n(text, strlen(text)) : nullptr;
}

static void trim_ascii_in_place(char *text)
{
    if (text == nullptr) {
        return;
    }
    char *start = text;
    while (*start != '\0' && static_cast<unsigned char>(*start) <= 0x20U) {
        ++start;
    }
    char *end = start + strlen(start);
    while (end > start && static_cast<unsigned char>(end[-1]) <= 0x20U) {
        --end;
    }
    const size_t length = static_cast<size_t>(end - start);
    if (start != text && length > 0) {
        memmove(text, start, length);
    }
    text[length] = '\0';
}

static bool ensure_artist_capacity(MediaMetadataBuildV2 *metadata, uint16_t required)
{
    if (metadata == nullptr) {
        return false;
    }
    if (required <= metadata->artist_capacity) {
        return true;
    }
    uint32_t next = metadata->artist_capacity == 0 ? 2U : metadata->artist_capacity;
    while (next < required) {
        next *= 2U;
        if (next > UINT16_MAX) {
            return false;
        }
    }
    void *grown = metadata_realloc(metadata->artists, next * sizeof(char *));
    if (grown == nullptr) {
        return false;
    }
    metadata->artists = static_cast<char **>(grown);
    for (uint16_t i = metadata->artist_capacity; i < next; ++i) {
        metadata->artists[i] = nullptr;
    }
    metadata->artist_capacity = static_cast<uint16_t>(next);
    return true;
}

static bool add_artist_owned(MediaMetadataBuildV2 *metadata, char *artist)
{
    if (metadata == nullptr || artist == nullptr) {
        heap_caps_free(artist);
        return false;
    }
    trim_ascii_in_place(artist);
    if (artist[0] == '\0') {
        heap_caps_free(artist);
        return true;
    }
    // 只去掉标签里完全重复的多值；绝不根据 '/', '&', 'feat.' 猜拆分。
    for (uint16_t i = 0; i < metadata->artist_count; ++i) {
        if (strcmp(metadata->artists[i], artist) == 0) {
            heap_caps_free(artist);
            return true;
        }
    }
    if (metadata->artist_count == UINT16_MAX ||
        !ensure_artist_capacity(metadata, static_cast<uint16_t>(metadata->artist_count + 1U))) {
        heap_caps_free(artist);
        return false;
    }
    metadata->artists[metadata->artist_count++] = artist;
    return true;
}

static bool ensure_lyrics_capacity(MediaMetadataBuildV2 *metadata, uint16_t required)
{
    if (metadata == nullptr) {
        return false;
    }
    if (required <= metadata->lyrics_capacity) {
        return true;
    }
    uint32_t next = metadata->lyrics_capacity == 0 ? 2U : metadata->lyrics_capacity;
    while (next < required) {
        next *= 2U;
        if (next > UINT16_MAX) {
            return false;
        }
    }
    void *grown = metadata_realloc(metadata->lyrics, next * sizeof(MediaMetadataLyricsBuildV2));
    if (grown == nullptr) {
        return false;
    }
    metadata->lyrics = static_cast<MediaMetadataLyricsBuildV2 *>(grown);
    for (uint16_t i = metadata->lyrics_capacity; i < next; ++i) {
        metadata->lyrics[i] = {};
    }
    metadata->lyrics_capacity = static_cast<uint16_t>(next);
    return true;
}

static bool add_lyrics_owned(MediaMetadataBuildV2 *metadata, MediaMetadataLyricsBuildV2 *source)
{
    if (metadata == nullptr || source == nullptr) {
        return false;
    }
    if (metadata->lyrics_count == UINT16_MAX ||
        !ensure_lyrics_capacity(metadata, static_cast<uint16_t>(metadata->lyrics_count + 1U))) {
        heap_caps_free(source->path);
        heap_caps_free(source->language);
        *source = {};
        return false;
    }
    metadata->lyrics[metadata->lyrics_count++] = *source;
    *source = {};
    return true;
}

void media_metadata_build_release(MediaMetadataBuildV2 *metadata)
{
    if (metadata == nullptr) {
        return;
    }
    heap_caps_free(metadata->title);
    heap_caps_free(metadata->display_artist);
    heap_caps_free(metadata->album);
    heap_caps_free(metadata->album_artist);
    for (uint16_t i = 0; i < metadata->artist_count; ++i) {
        heap_caps_free(metadata->artists[i]);
    }
    heap_caps_free(metadata->artists);
    for (uint16_t i = 0; i < metadata->lyrics_count; ++i) {
        heap_caps_free(metadata->lyrics[i].path);
        heap_caps_free(metadata->lyrics[i].language);
    }
    heap_caps_free(metadata->lyrics);
    *metadata = {};
}

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

static uint32_t read_le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
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
    return current >= 0 && seek_u64(file, static_cast<uint64_t>(current) + bytes);
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

static bool utf8_append_codepoint(char *out, size_t capacity, size_t *length, uint32_t cp)
{
    if (out == nullptr || length == nullptr) {
        return false;
    }
    uint8_t bytes[4] = {};
    size_t count = 0;
    if (cp <= 0x7FU) {
        bytes[0] = static_cast<uint8_t>(cp);
        count = 1;
    } else if (cp <= 0x7FFU) {
        bytes[0] = static_cast<uint8_t>(0xC0U | (cp >> 6));
        bytes[1] = static_cast<uint8_t>(0x80U | (cp & 0x3FU));
        count = 2;
    } else if (cp <= 0xFFFFU) {
        bytes[0] = static_cast<uint8_t>(0xE0U | (cp >> 12));
        bytes[1] = static_cast<uint8_t>(0x80U | ((cp >> 6) & 0x3FU));
        bytes[2] = static_cast<uint8_t>(0x80U | (cp & 0x3FU));
        count = 3;
    } else if (cp <= 0x10FFFFU) {
        bytes[0] = static_cast<uint8_t>(0xF0U | (cp >> 18));
        bytes[1] = static_cast<uint8_t>(0x80U | ((cp >> 12) & 0x3FU));
        bytes[2] = static_cast<uint8_t>(0x80U | ((cp >> 6) & 0x3FU));
        bytes[3] = static_cast<uint8_t>(0x80U | (cp & 0x3FU));
        count = 4;
    } else {
        return true;
    }
    if (*length + count + 1U > capacity) {
        return false;
    }
    memcpy(out + *length, bytes, count);
    *length += count;
    out[*length] = '\0';
    return true;
}

static char *decode_latin1(const uint8_t *data, size_t size)
{
    if (data == nullptr) {
        return nullptr;
    }
    if (size > (SIZE_MAX - 1U) / 2U) {
        return nullptr;
    }
    char *out = static_cast<char *>(metadata_alloc(size * 2U + 1U));
    if (out == nullptr) {
        return nullptr;
    }
    size_t length = 0;
    out[0] = '\0';
    for (size_t i = 0; i < size; ++i) {
        const uint8_t c = data[i];
        if (c == 0U) {
            break;
        }
        if (!utf8_append_codepoint(out, size * 2U + 1U, &length, c)) {
            heap_caps_free(out);
            return nullptr;
        }
    }
    trim_ascii_in_place(out);
    return out;
}

static uint16_t read_u16(const uint8_t *p, bool little_endian)
{
    return little_endian
        ? static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8))
        : static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static char *decode_utf16(const uint8_t *data, size_t size, bool bom_allowed, bool default_be)
{
    if (data == nullptr || size < 2U) {
        return metadata_strdup("");
    }
    bool little = !default_be;
    size_t cursor = 0;
    if (bom_allowed && size >= 2U) {
        if (data[0] == 0xFFU && data[1] == 0xFEU) {
            little = true;
            cursor = 2U;
        } else if (data[0] == 0xFEU && data[1] == 0xFFU) {
            little = false;
            cursor = 2U;
        }
    }
    char *out = static_cast<char *>(metadata_alloc(size * 2U + 1U));
    if (out == nullptr) {
        return nullptr;
    }
    size_t length = 0;
    out[0] = '\0';
    while (cursor + 1U < size) {
        uint16_t u = read_u16(data + cursor, little);
        cursor += 2U;
        if (u == 0U) {
            break;
        }
        uint32_t cp = u;
        if (u >= 0xD800U && u <= 0xDBFFU && cursor + 1U < size) {
            const uint16_t low = read_u16(data + cursor, little);
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                cursor += 2U;
                cp = 0x10000U + ((static_cast<uint32_t>(u - 0xD800U) << 10) | (low - 0xDC00U));
            }
        }
        if (!utf8_append_codepoint(out, size * 2U + 1U, &length, cp)) {
            heap_caps_free(out);
            return nullptr;
        }
    }
    trim_ascii_in_place(out);
    return out;
}

static char *decode_id3_text_segment(const uint8_t *data, size_t size, uint8_t encoding)
{
    switch (encoding) {
        case 0:
            return decode_latin1(data, size);
        case 1:
            return decode_utf16(data, size, true, false);
        case 2:
            return decode_utf16(data, size, false, true);
        case 3: {
            size_t length = 0;
            while (length < size && data[length] != 0U) {
                ++length;
            }
            char *out = metadata_strdup_n(reinterpret_cast<const char *>(data), length);
            if (out != nullptr) {
                trim_ascii_in_place(out);
            }
            return out;
        }
        default:
            return nullptr;
    }
}

static size_t id3_segment_raw_length(const uint8_t *data, size_t size, uint8_t encoding)
{
    if (encoding == 0U || encoding == 3U) {
        size_t i = 0;
        while (i < size && data[i] != 0U) {
            ++i;
        }
        return i;
    }
    size_t i = 0;
    while (i + 1U < size) {
        if (data[i] == 0U && data[i + 1U] == 0U) {
            break;
        }
        i += 2U;
    }
    return i;
}

static size_t id3_terminator_bytes(uint8_t encoding)
{
    return (encoding == 0U || encoding == 3U) ? 1U : 2U;
}

static uint8_t *id3_deunsync_copy(const uint8_t *data, size_t size, size_t *out_size)
{
    if (data == nullptr || out_size == nullptr) {
        return nullptr;
    }
    uint8_t *out = static_cast<uint8_t *>(metadata_alloc(size == 0 ? 1U : size));
    if (out == nullptr) {
        return nullptr;
    }
    size_t w = 0;
    for (size_t r = 0; r < size; ++r) {
        out[w++] = data[r];
        if (data[r] == 0xFFU && r + 1U < size && data[r + 1U] == 0x00U) {
            ++r;
        }
    }
    *out_size = w;
    return out;
}

static bool parse_number_pair(const char *text, uint16_t *number, uint16_t *total, bool *have_total)
{
    if (text == nullptr || number == nullptr || total == nullptr || have_total == nullptr) {
        return false;
    }
    const char *p = text;
    while (*p != '\0' && isspace(static_cast<unsigned char>(*p))) {
        ++p;
    }
    char *end = nullptr;
    const unsigned long first = strtoul(p, &end, 10);
    if (end == p || first == 0U || first > UINT16_MAX) {
        return false;
    }
    *number = static_cast<uint16_t>(first);
    *total = 0;
    *have_total = false;
    while (*end != '\0' && isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }
    if (*end == '/') {
        ++end;
        while (*end != '\0' && isspace(static_cast<unsigned char>(*end))) {
            ++end;
        }
        char *total_end = nullptr;
        const unsigned long second = strtoul(end, &total_end, 10);
        if (total_end != end && second > 0U && second <= UINT16_MAX) {
            *total = static_cast<uint16_t>(second);
            *have_total = true;
        }
    }
    return true;
}

static bool parse_positive_u16(const char *text, uint16_t *value)
{
    if (text == nullptr || value == nullptr) {
        return false;
    }
    while (*text != '\0' && isspace(static_cast<unsigned char>(*text))) {
        ++text;
    }
    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || parsed == 0U || parsed > UINT16_MAX) {
        return false;
    }
    *value = static_cast<uint16_t>(parsed);
    return true;
}

static bool parse_year(const char *text, uint16_t *year)
{
    if (text == nullptr || year == nullptr) {
        return false;
    }
    for (const char *p = text; p[0] && p[1] && p[2] && p[3]; ++p) {
        if (isdigit(static_cast<unsigned char>(p[0])) &&
            isdigit(static_cast<unsigned char>(p[1])) &&
            isdigit(static_cast<unsigned char>(p[2])) &&
            isdigit(static_cast<unsigned char>(p[3]))) {
            const unsigned value = static_cast<unsigned>((p[0] - '0') * 1000 + (p[1] - '0') * 100 +
                (p[2] - '0') * 10 + (p[3] - '0'));
            if (value >= 1000U && value <= 9999U) {
                *year = static_cast<uint16_t>(value);
                return true;
            }
        }
    }
    return false;
}

static void apply_track_number(MediaMetadataBuildV2 *metadata, const char *text)
{
    uint16_t number = 0;
    uint16_t total = 0;
    bool have_total = false;
    if (parse_number_pair(text, &number, &total, &have_total)) {
        metadata->track_number = number;
        metadata->metadata_flags |= MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2;
        if (have_total) {
            metadata->track_total = total;
            metadata->metadata_flags |= MEDIA_TRACK_META_HAS_TRACK_TOTAL_V2;
        }
    }
}

static void apply_disc_number(MediaMetadataBuildV2 *metadata, const char *text)
{
    uint16_t number = 0;
    uint16_t total = 0;
    bool have_total = false;
    if (parse_number_pair(text, &number, &total, &have_total)) {
        metadata->disc_number = number;
        metadata->metadata_flags |= MEDIA_TRACK_META_HAS_DISC_NUMBER_V2;
        if (have_total) {
            metadata->disc_total = total;
            metadata->metadata_flags |= MEDIA_TRACK_META_HAS_DISC_TOTAL_V2;
        }
    }
}

static void apply_track_total(MediaMetadataBuildV2 *metadata, const char *text)
{
    uint16_t value = 0;
    if (parse_positive_u16(text, &value)) {
        metadata->track_total = value;
        metadata->metadata_flags |= MEDIA_TRACK_META_HAS_TRACK_TOTAL_V2;
    }
}

static void apply_disc_total(MediaMetadataBuildV2 *metadata, const char *text)
{
    uint16_t value = 0;
    if (parse_positive_u16(text, &value)) {
        metadata->disc_total = value;
        metadata->metadata_flags |= MEDIA_TRACK_META_HAS_DISC_TOTAL_V2;
    }
}

static void apply_release_year(MediaMetadataBuildV2 *metadata, const char *text)
{
    uint16_t year = 0;
    if (parse_year(text, &year)) {
        metadata->release_year = year;
        metadata->metadata_flags |= MEDIA_TRACK_META_HAS_RELEASE_YEAR_V2;
    }
}

static void apply_original_year(MediaMetadataBuildV2 *metadata, const char *text)
{
    uint16_t year = 0;
    if (parse_year(text, &year)) {
        metadata->original_year = year;
        metadata->metadata_flags |= MEDIA_TRACK_META_HAS_ORIGINAL_YEAR_V2;
    }
}

static bool build_display_artist(MediaMetadataBuildV2 *metadata)
{
    if (metadata == nullptr || metadata->artist_count == 0 || metadata->display_artist != nullptr) {
        return true;
    }
    size_t bytes = 1U;
    for (uint16_t i = 0; i < metadata->artist_count; ++i) {
        const size_t len = strlen(metadata->artists[i]);
        if (bytes > SIZE_MAX - len - (i == 0 ? 0U : 3U)) {
            return false;
        }
        bytes += len + (i == 0 ? 0U : 3U);
    }
    char *joined = static_cast<char *>(metadata_alloc(bytes));
    if (joined == nullptr) {
        return false;
    }
    size_t cursor = 0;
    for (uint16_t i = 0; i < metadata->artist_count; ++i) {
        if (i != 0) {
            memcpy(joined + cursor, " / ", 3U);
            cursor += 3U;
        }
        const size_t len = strlen(metadata->artists[i]);
        memcpy(joined + cursor, metadata->artists[i], len);
        cursor += len;
    }
    joined[cursor] = '\0';
    metadata->display_artist = joined;
    return true;
}

static bool add_external_lrc(const char *audio_path, MediaMetadataBuildV2 *metadata)
{
    if (audio_path == nullptr || metadata == nullptr) {
        return false;
    }
    const char *dot = strrchr(audio_path, '.');
    if (dot == nullptr) {
        return true;
    }
    const size_t prefix = static_cast<size_t>(dot - audio_path);
    if (prefix > SIZE_MAX - 5U) {
        return false;
    }
    char *candidate = static_cast<char *>(metadata_alloc(prefix + 5U));
    if (candidate == nullptr) {
        return false;
    }
    memcpy(candidate, audio_path, prefix);
    memcpy(candidate + prefix, ".lrc", 5U);

    struct stat info = {};
    if (stat(candidate, &info) != 0 || !S_ISREG(info.st_mode)) {
        heap_caps_free(candidate);
        return true;
    }

    MediaMetadataLyricsBuildV2 lyrics = {};
    lyrics.path = candidate;
    lyrics.language = metadata_strdup("");
    lyrics.source = MediaLyricsSourceV2::ExternalFile;
    lyrics.kind = MediaLyricsKindV2::Synced;
    lyrics.encoding = MediaLyricsEncodingV2::Unknown;
    if (lyrics.language == nullptr || !add_lyrics_owned(metadata, &lyrics)) {
        heap_caps_free(lyrics.path);
        heap_caps_free(lyrics.language);
        return false;
    }
    return true;
}

static MediaLyricsEncodingV2 lyrics_encoding_from_id3(uint8_t encoding)
{
    switch (encoding) {
        case 0: return MediaLyricsEncodingV2::Latin1;
        case 1: return MediaLyricsEncodingV2::Unknown; // UTF-16 with BOM，真实端序由 LyricsTask 读取 BOM 后决定。
        case 2: return MediaLyricsEncodingV2::Utf16Be;
        case 3: return MediaLyricsEncodingV2::Utf8;
        default: return MediaLyricsEncodingV2::Unknown;
    }
}

static bool id3_text_frame_apply(
    const char *frame_id,
    const uint8_t *payload,
    size_t payload_size,
    bool allow_multi_artist,
    MediaMetadataBuildV2 *metadata
)
{
    if (frame_id == nullptr || payload == nullptr || payload_size < 1U || metadata == nullptr) {
        return true;
    }
    const uint8_t encoding = payload[0];
    if (encoding > 3U) {
        return true;
    }
    const uint8_t *cursor = payload + 1U;
    size_t remaining = payload_size - 1U;
    bool first = true;
    while (remaining > 0U) {
        const size_t raw_len = id3_segment_raw_length(cursor, remaining, encoding);
        char *value = decode_id3_text_segment(cursor, raw_len, encoding);
        if (value == nullptr) {
            return false;
        }
        if (value[0] != '\0') {
            if (strcmp(frame_id, "TIT2") == 0 || strcmp(frame_id, "TT2") == 0) {
                if (metadata->title == nullptr) {
                    metadata->title = value;
                    metadata->metadata_flags |= MEDIA_TRACK_META_HAS_TITLE_TAG_V2;
                    value = nullptr;
                }
            } else if (strcmp(frame_id, "TPE1") == 0 || strcmp(frame_id, "TP1") == 0) {
                if (allow_multi_artist || first) {
                    if (!add_artist_owned(metadata, value)) {
                        return false;
                    }
                    value = nullptr;
                    metadata->metadata_flags |= MEDIA_TRACK_META_HAS_ARTIST_TAG_V2;
                }
            } else if (strcmp(frame_id, "TPE2") == 0 || strcmp(frame_id, "TP2") == 0) {
                if (metadata->album_artist == nullptr) {
                    metadata->album_artist = value;
                    metadata->metadata_flags |= MEDIA_TRACK_META_HAS_ALBUM_ARTIST_TAG_V2;
                    value = nullptr;
                }
            } else if (strcmp(frame_id, "TALB") == 0 || strcmp(frame_id, "TAL") == 0) {
                if (metadata->album == nullptr) {
                    metadata->album = value;
                    metadata->metadata_flags |= MEDIA_TRACK_META_HAS_ALBUM_TAG_V2;
                    value = nullptr;
                }
            } else if (strcmp(frame_id, "TRCK") == 0 || strcmp(frame_id, "TRK") == 0) {
                apply_track_number(metadata, value);
            } else if (strcmp(frame_id, "TPOS") == 0 || strcmp(frame_id, "TPA") == 0) {
                apply_disc_number(metadata, value);
            } else if (strcmp(frame_id, "TDRC") == 0 || strcmp(frame_id, "TYER") == 0 || strcmp(frame_id, "TYE") == 0) {
                apply_release_year(metadata, value);
            } else if (strcmp(frame_id, "TDOR") == 0 || strcmp(frame_id, "TORY") == 0 || strcmp(frame_id, "TOR") == 0) {
                apply_original_year(metadata, value);
            }
        }
        heap_caps_free(value);
        first = false;

        if (!allow_multi_artist) {
            break;
        }
        const size_t term = id3_terminator_bytes(encoding);
        if (raw_len + term > remaining) {
            break;
        }
        cursor += raw_len + term;
        remaining -= raw_len + term;
    }
    return true;
}

static bool id3_add_lyrics_ref(
    const char *frame_id,
    FILE *file,
    uint64_t payload_offset,
    uint32_t payload_size,
    bool unsynchronised,
    MediaMetadataBuildV2 *metadata
)
{
    if (frame_id == nullptr || file == nullptr || metadata == nullptr || payload_size < 4U) {
        return true;
    }
    const size_t fixed_prefix = (strcmp(frame_id, "SYLT") == 0 || strcmp(frame_id, "SLT") == 0) ? 6U : 4U;
    if (payload_size < fixed_prefix || !seek_u64(file, payload_offset)) {
        return true;
    }
    uint8_t prefix[6] = {};
    if (fread(prefix, 1, fixed_prefix, file) != fixed_prefix || prefix[0] > 3U) {
        return true;
    }

    char language_text[4] = {
        static_cast<char>(prefix[1]), static_cast<char>(prefix[2]), static_cast<char>(prefix[3]), '\0'
    };
    for (size_t i = 0; i < 3; ++i) {
        if (!isalpha(static_cast<unsigned char>(language_text[i]))) {
            language_text[i] = '\0';
            break;
        }
    }

    MediaMetadataLyricsBuildV2 lyrics = {};
    lyrics.language = metadata_strdup(language_text);
    lyrics.data_offset = payload_offset;
    lyrics.data_size = payload_size;
    lyrics.source = MediaLyricsSourceV2::EmbeddedTag;
    lyrics.kind = fixed_prefix == 6U ? MediaLyricsKindV2::Synced : MediaLyricsKindV2::Unsynced;
    lyrics.encoding = lyrics_encoding_from_id3(prefix[0]);
    lyrics.flags = MEDIA_LYRICS_REF_ID3_FRAME_PAYLOAD_V2 |
        (unsynchronised ? MEDIA_LYRICS_REF_NEEDS_ID3_UNSYNC_V2 : MEDIA_LYRICS_REF_NONE_V2);
    if (lyrics.language == nullptr || !add_lyrics_owned(metadata, &lyrics)) {
        heap_caps_free(lyrics.language);
        return false;
    }
    return true;
}

static esp_err_t parse_id3(FILE *file, uint64_t file_size, MediaMetadataBuildV2 *metadata)
{
    if (file == nullptr || metadata == nullptr || !seek_u64(file, 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t header[10] = {};
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "ID3", 3) != 0) {
        return ESP_OK; // 没有 ID3 不是错误。
    }
    if ((header[6] | header[7] | header[8] | header[9]) & 0x80U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t version = header[3];
    const uint8_t flags = header[5];
    if (version < 2U || version > 4U) {
        ESP_LOGW(TAG, "暂不解析 ID3v2.%u，保留文件名 fallback", static_cast<unsigned>(version));
        return ESP_OK;
    }
    // v2.2/v2.3 的 tag-level unsynchronisation 会改变原始 frame 数据布局；当前宁可安全跳过，
    // 也不根据错误 offset 构造歌词引用。v2.4 frame size 定义允许安全逐帧跳过并记录 unsync flag。
    const bool tag_unsync = (flags & 0x80U) != 0U;
    if (tag_unsync && version < 4U) {
        ESP_LOGW(TAG, "检测到 ID3v2.%u tag-level unsynchronisation，本阶段安全跳过内嵌标签", static_cast<unsigned>(version));
        return ESP_OK;
    }

    const uint32_t tag_size = read_synchsafe_u28(&header[6]);
    const uint64_t tag_end = 10ULL + tag_size;
    if (tag_end > file_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t cursor = 10U;

    if (version >= 3U && (flags & 0x40U) != 0U) {
        uint8_t size_bytes[4] = {};
        if (!seek_u64(file, cursor) || fread(size_bytes, 1, 4, file) != 4) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t ext_size = version == 4U ? read_synchsafe_u28(size_bytes) : read_be32(size_bytes);
        const uint64_t total_ext = version == 4U ? ext_size : static_cast<uint64_t>(ext_size) + 4ULL;
        if (total_ext < 4U || cursor + total_ext > tag_end) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        cursor += total_ext;
    }

    while (cursor < tag_end) {
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
        for (size_t i = 0; i < id_len; ++i) {
            all_zero = all_zero && frame_header[i] == 0U;
        }
        if (all_zero) {
            break; // padding
        }
        if (!id3_frame_id_valid(frame_header, id_len)) {
            break;
        }

        char frame_id[5] = {};
        memcpy(frame_id, frame_header, id_len);
        uint32_t frame_size = 0;
        bool frame_unsync = tag_unsync;
        bool unsupported_transform = false;
        if (version == 2U) {
            frame_size = read_be24(&frame_header[3]);
        } else if (version == 3U) {
            frame_size = read_be32(&frame_header[4]);
            const uint8_t format_flags = frame_header[9];
            unsupported_transform = (format_flags & (0x80U | 0x40U | 0x20U)) != 0U;
        } else {
            if ((frame_header[4] | frame_header[5] | frame_header[6] | frame_header[7]) & 0x80U) {
                break;
            }
            frame_size = read_synchsafe_u28(&frame_header[4]);
            const uint8_t format_flags = frame_header[9];
            unsupported_transform = (format_flags & (0x40U | 0x08U | 0x04U | 0x01U)) != 0U;
            frame_unsync = frame_unsync || (format_flags & 0x02U) != 0U;
        }
        const uint64_t payload_offset = cursor + frame_header_size;
        if (frame_size == 0U || payload_offset + frame_size > tag_end) {
            break;
        }

        const bool is_text = frame_id[0] == 'T' &&
            (strcmp(frame_id, "TXXX") != 0 && strcmp(frame_id, "TXX") != 0);
        const bool is_lyrics = strcmp(frame_id, "USLT") == 0 || strcmp(frame_id, "SYLT") == 0 ||
            strcmp(frame_id, "ULT") == 0 || strcmp(frame_id, "SLT") == 0;

        if (!unsupported_transform && is_lyrics) {
            if (!id3_add_lyrics_ref(frame_id, file, payload_offset, frame_size, frame_unsync, metadata)) {
                return ESP_ERR_NO_MEM;
            }
        } else if (!unsupported_transform && is_text && frame_size <= MAX_TEXT_FRAME_BYTES) {
            uint8_t *raw = static_cast<uint8_t *>(metadata_alloc(frame_size));
            if (raw == nullptr) {
                return ESP_ERR_NO_MEM;
            }
            if (!seek_u64(file, payload_offset) || fread(raw, 1, frame_size, file) != frame_size) {
                heap_caps_free(raw);
                return ESP_ERR_INVALID_SIZE;
            }
            uint8_t *payload = raw;
            size_t payload_size = frame_size;
            uint8_t *deunsynced = nullptr;
            if (frame_unsync) {
                deunsynced = id3_deunsync_copy(raw, frame_size, &payload_size);
                if (deunsynced == nullptr) {
                    heap_caps_free(raw);
                    return ESP_ERR_NO_MEM;
                }
                payload = deunsynced;
            }
            const bool allow_multi_artist = version == 4U &&
                (strcmp(frame_id, "TPE1") == 0 || strcmp(frame_id, "TP1") == 0);
            const bool ok = id3_text_frame_apply(frame_id, payload, payload_size, allow_multi_artist, metadata);
            heap_caps_free(deunsynced);
            heap_caps_free(raw);
            if (!ok) {
                return ESP_ERR_NO_MEM;
            }
        }
        cursor = payload_offset + frame_size;
    }
    return ESP_OK;
}


static esp_err_t parse_id3v1(FILE *file, uint64_t file_size, MediaMetadataBuildV2 *metadata)
{
    if (file == nullptr || metadata == nullptr || file_size < 128U || !seek_u64(file, file_size - 128U)) {
        return ESP_OK;
    }
    uint8_t tag[128] = {};
    if (fread(tag, 1, sizeof(tag), file) != sizeof(tag) || memcmp(tag, "TAG", 3) != 0) {
        return ESP_OK;
    }

    if (metadata->title == nullptr) {
        char *value = decode_latin1(&tag[3], 30U);
        if (value == nullptr) return ESP_ERR_NO_MEM;
        if (value[0] != '\0') {
            metadata->title = value;
            metadata->metadata_flags |= MEDIA_TRACK_META_HAS_TITLE_TAG_V2;
        } else {
            heap_caps_free(value);
        }
    }
    if (metadata->artist_count == 0U) {
        char *value = decode_latin1(&tag[33], 30U);
        if (value == nullptr) return ESP_ERR_NO_MEM;
        if (value[0] != '\0') {
            if (!add_artist_owned(metadata, value)) return ESP_ERR_NO_MEM;
            metadata->metadata_flags |= MEDIA_TRACK_META_HAS_ARTIST_TAG_V2;
        } else {
            heap_caps_free(value);
        }
    }
    if (metadata->album == nullptr) {
        char *value = decode_latin1(&tag[63], 30U);
        if (value == nullptr) return ESP_ERR_NO_MEM;
        if (value[0] != '\0') {
            metadata->album = value;
            metadata->metadata_flags |= MEDIA_TRACK_META_HAS_ALBUM_TAG_V2;
        } else {
            heap_caps_free(value);
        }
    }
    if ((metadata->metadata_flags & MEDIA_TRACK_META_HAS_RELEASE_YEAR_V2) == 0U) {
        char year_text[5] = {};
        memcpy(year_text, &tag[93], 4U);
        apply_release_year(metadata, year_text);
    }
    // ID3v1.1：comment[28] == 0 且 comment[29] != 0 时最后一字节为 track number。
    if ((metadata->metadata_flags & MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2) == 0U &&
        tag[125] == 0U && tag[126] != 0U) {
        metadata->track_number = tag[126];
        metadata->metadata_flags |= MEDIA_TRACK_META_HAS_TRACK_NUMBER_V2;
    }
    return ESP_OK;
}

static esp_err_t read_utf8_value(FILE *file, uint64_t offset, uint32_t size, char **out_value)
{
    if (file == nullptr || out_value == nullptr || size > MAX_TAG_TEXT_BYTES || !seek_u64(file, offset)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_value = nullptr;
    char *value = static_cast<char *>(metadata_alloc(static_cast<size_t>(size) + 1U));
    if (value == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (size > 0U && fread(value, 1, size, file) != size) {
        heap_caps_free(value);
        return ESP_ERR_INVALID_SIZE;
    }
    value[size] = '\0';
    trim_ascii_in_place(value);
    *out_value = value;
    return ESP_OK;
}

static bool vorbis_key_equals(const char *key, const char *expected)
{
    return key != nullptr && expected != nullptr && strcasecmp(key, expected) == 0;
}

static esp_err_t apply_vorbis_comment(
    FILE *file,
    uint64_t comment_offset,
    uint32_t comment_length,
    MediaMetadataBuildV2 *metadata
)
{
    if (file == nullptr || metadata == nullptr || comment_length == 0U) {
        return ESP_OK;
    }
    const size_t prefix_size = comment_length < MAX_VORBIS_KEY_BYTES ? comment_length : MAX_VORBIS_KEY_BYTES;
    char prefix[MAX_VORBIS_KEY_BYTES + 1U] = {};
    if (!seek_u64(file, comment_offset) || fread(prefix, 1, prefix_size, file) != prefix_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    prefix[prefix_size] = '\0';
    char *equals = static_cast<char *>(memchr(prefix, '=', prefix_size));
    if (equals == nullptr) {
        return ESP_OK;
    }
    *equals = '\0';
    const size_t key_length = static_cast<size_t>(equals - prefix);
    if (key_length == 0U || key_length + 1U > comment_length) {
        return ESP_OK;
    }
    const uint64_t value_offset = comment_offset + key_length + 1U;
    const uint32_t value_size = comment_length - static_cast<uint32_t>(key_length + 1U);

    const bool lyrics_unsynced = vorbis_key_equals(prefix, "LYRICS") || vorbis_key_equals(prefix, "UNSYNCEDLYRICS");
    const bool lyrics_synced = vorbis_key_equals(prefix, "SYNCEDLYRICS");
    if (lyrics_unsynced || lyrics_synced) {
        if (value_size == 0U) {
            return ESP_OK;
        }
        MediaMetadataLyricsBuildV2 lyrics = {};
        lyrics.language = metadata_strdup("");
        lyrics.data_offset = value_offset;
        lyrics.data_size = value_size;
        lyrics.source = MediaLyricsSourceV2::EmbeddedTag;
        lyrics.kind = lyrics_synced ? MediaLyricsKindV2::Synced : MediaLyricsKindV2::Unsynced;
        lyrics.encoding = MediaLyricsEncodingV2::Utf8;
        if (lyrics.language == nullptr || !add_lyrics_owned(metadata, &lyrics)) {
            heap_caps_free(lyrics.language);
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    const bool interesting =
        vorbis_key_equals(prefix, "TITLE") || vorbis_key_equals(prefix, "ARTIST") ||
        vorbis_key_equals(prefix, "ALBUM") || vorbis_key_equals(prefix, "ALBUMARTIST") ||
        vorbis_key_equals(prefix, "ALBUM ARTIST") || vorbis_key_equals(prefix, "TRACKNUMBER") ||
        vorbis_key_equals(prefix, "TRACKTOTAL") || vorbis_key_equals(prefix, "TOTALTRACKS") ||
        vorbis_key_equals(prefix, "DISCNUMBER") || vorbis_key_equals(prefix, "DISCTOTAL") ||
        vorbis_key_equals(prefix, "TOTALDISCS") || vorbis_key_equals(prefix, "DATE") ||
        vorbis_key_equals(prefix, "YEAR") || vorbis_key_equals(prefix, "ORIGINALDATE") ||
        vorbis_key_equals(prefix, "ORIGINALYEAR");
    if (!interesting || value_size > MAX_TAG_TEXT_BYTES) {
        return ESP_OK;
    }

    char *value = nullptr;
    const esp_err_t read_ret = read_utf8_value(file, value_offset, value_size, &value);
    if (read_ret != ESP_OK) {
        return read_ret;
    }
    if (value[0] == '\0') {
        heap_caps_free(value);
        return ESP_OK;
    }

    if (vorbis_key_equals(prefix, "TITLE")) {
        if (metadata->title == nullptr) {
            metadata->title = value;
            metadata->metadata_flags |= MEDIA_TRACK_META_HAS_TITLE_TAG_V2;
            value = nullptr;
        }
    } else if (vorbis_key_equals(prefix, "ARTIST")) {
        if (!add_artist_owned(metadata, value)) {
            return ESP_ERR_NO_MEM;
        }
        value = nullptr;
        metadata->metadata_flags |= MEDIA_TRACK_META_HAS_ARTIST_TAG_V2;
    } else if (vorbis_key_equals(prefix, "ALBUM")) {
        if (metadata->album == nullptr) {
            metadata->album = value;
            metadata->metadata_flags |= MEDIA_TRACK_META_HAS_ALBUM_TAG_V2;
            value = nullptr;
        }
    } else if (vorbis_key_equals(prefix, "ALBUMARTIST") || vorbis_key_equals(prefix, "ALBUM ARTIST")) {
        if (metadata->album_artist == nullptr) {
            metadata->album_artist = value;
            metadata->metadata_flags |= MEDIA_TRACK_META_HAS_ALBUM_ARTIST_TAG_V2;
            value = nullptr;
        }
    } else if (vorbis_key_equals(prefix, "TRACKNUMBER")) {
        apply_track_number(metadata, value);
    } else if (vorbis_key_equals(prefix, "TRACKTOTAL") || vorbis_key_equals(prefix, "TOTALTRACKS")) {
        apply_track_total(metadata, value);
    } else if (vorbis_key_equals(prefix, "DISCNUMBER")) {
        apply_disc_number(metadata, value);
    } else if (vorbis_key_equals(prefix, "DISCTOTAL") || vorbis_key_equals(prefix, "TOTALDISCS")) {
        apply_disc_total(metadata, value);
    } else if (vorbis_key_equals(prefix, "DATE") || vorbis_key_equals(prefix, "YEAR")) {
        apply_release_year(metadata, value);
    } else if (vorbis_key_equals(prefix, "ORIGINALDATE") || vorbis_key_equals(prefix, "ORIGINALYEAR")) {
        apply_original_year(metadata, value);
    }
    heap_caps_free(value);
    return ESP_OK;
}

static esp_err_t parse_flac_vorbis(FILE *file, uint64_t file_size, MediaMetadataBuildV2 *metadata)
{
    if (file == nullptr || metadata == nullptr || !seek_u64(file, 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t first10[10] = {};
    const size_t got = fread(first10, 1, sizeof(first10), file);
    if (got < 4U) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint64_t flac_offset = 0;
    if (memcmp(first10, "ID3", 3) == 0) {
        if (got < 10U || ((first10[6] | first10[7] | first10[8] | first10[9]) & 0x80U) != 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t id3_size = read_synchsafe_u28(&first10[6]);
        const bool footer = (first10[5] & 0x10U) != 0U;
        flac_offset = 10ULL + id3_size + (footer ? 10ULL : 0ULL);
    }
    if (flac_offset + 4U > file_size || !seek_u64(file, flac_offset)) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t marker[4] = {};
    if (fread(marker, 1, 4, file) != 4U || memcmp(marker, "fLaC", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool last = false;
    while (!last) {
        uint8_t block_header[4] = {};
        if (fread(block_header, 1, 4, file) != 4U) {
            return ESP_ERR_INVALID_SIZE;
        }
        last = (block_header[0] & 0x80U) != 0U;
        const uint8_t type = block_header[0] & 0x7FU;
        const uint32_t length = read_be24(&block_header[1]);
        const long pos = ftell(file);
        if (pos < 0) {
            return ESP_FAIL;
        }
        const uint64_t block_offset = static_cast<uint64_t>(pos);
        if (block_offset + length > file_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (type != 4U) {
            if (!skip_bytes(file, length)) {
                return ESP_ERR_INVALID_SIZE;
            }
            continue;
        }

        const uint64_t block_end = block_offset + length;
        uint8_t u32[4] = {};
        if (length < 8U || fread(u32, 1, 4, file) != 4U) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t vendor_length = read_le32(u32);
        if (!skip_bytes(file, vendor_length) || ftell(file) < 0 || static_cast<uint64_t>(ftell(file)) + 4U > block_end ||
            fread(u32, 1, 4, file) != 4U) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t comment_count = read_le32(u32);
        for (uint32_t i = 0; i < comment_count; ++i) {
            const long len_pos = ftell(file);
            if (len_pos < 0 || static_cast<uint64_t>(len_pos) + 4U > block_end || fread(u32, 1, 4, file) != 4U) {
                return ESP_ERR_INVALID_SIZE;
            }
            const uint32_t comment_length = read_le32(u32);
            const long comment_pos = ftell(file);
            if (comment_pos < 0 || static_cast<uint64_t>(comment_pos) + comment_length > block_end) {
                return ESP_ERR_INVALID_SIZE;
            }
            const uint64_t comment_offset = static_cast<uint64_t>(comment_pos);
            const esp_err_t apply_ret = apply_vorbis_comment(file, comment_offset, comment_length, metadata);
            if (apply_ret != ESP_OK) {
                return apply_ret;
            }
            if (!seek_u64(file, comment_offset + comment_length)) {
                return ESP_ERR_INVALID_SIZE;
            }
        }
        if (!seek_u64(file, block_end)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

esp_err_t media_metadata_scan_file_v2(
    const char *path,
    MediaFormat format,
    uint64_t file_size,
    MediaMetadataBuildV2 *out_metadata
)
{
    if (path == nullptr || out_metadata == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    media_metadata_build_release(out_metadata);

    StorageSdLockGuard sd_lock;
    if (!sd_lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }

    if (format != MediaFormat::FLAC && format != MediaFormat::MP3) {
        out_metadata->metadata_flags |= MEDIA_TRACK_META_SCANNED_V2;
        return add_external_lrc(path, out_metadata) ? ESP_OK : ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = format == MediaFormat::FLAC
        ? parse_flac_vorbis(file, file_size, out_metadata)
        : parse_id3(file, file_size, out_metadata);
    if (ret == ESP_OK && format == MediaFormat::MP3) {
        ret = parse_id3v1(file, file_size, out_metadata);
    }
    fclose(file);
    if (ret != ESP_OK) {
        media_metadata_build_release(out_metadata);
        return ret;
    }
    if (!build_display_artist(out_metadata) || !add_external_lrc(path, out_metadata)) {
        media_metadata_build_release(out_metadata);
        return ESP_ERR_NO_MEM;
    }
    out_metadata->metadata_flags |= MEDIA_TRACK_META_SCANNED_V2;
    return ESP_OK;
}

static bool clone_string_from_pool(const MusicCatalogV2 *catalog, uint32_t offset, char **out)
{
    if (out == nullptr) {
        return false;
    }
    *out = nullptr;
    const char *text = media_catalog_v2_pool_str(catalog, offset);
    if (text == nullptr || text[0] == '\0') {
        return text != nullptr;
    }
    *out = metadata_strdup(text);
    return *out != nullptr;
}

esp_err_t media_metadata_clone_from_catalog_v2(
    const MusicCatalogV2 *catalog,
    uint32_t track_index,
    MediaMetadataBuildV2 *out_metadata
)
{
    if (catalog == nullptr || out_metadata == nullptr || track_index >= catalog->track_count) {
        return ESP_ERR_INVALID_ARG;
    }
    media_metadata_build_release(out_metadata);
    const TrackRowV2 &track = catalog->tracks[track_index];
    if ((track.metadata_flags & MEDIA_TRACK_META_SCANNED_V2) == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!clone_string_from_pool(catalog, track.title_off, &out_metadata->title) ||
        !clone_string_from_pool(catalog, track.display_artist_off, &out_metadata->display_artist)) {
        media_metadata_build_release(out_metadata);
        return ESP_ERR_NO_MEM;
    }
    if (track.album_id != MEDIA_CATALOG_INVALID_ID_V2) {
        const AlbumRowV2 &album = catalog->albums[track.album_id];
        if (!clone_string_from_pool(catalog, album.title_off, &out_metadata->album) ||
            !clone_string_from_pool(catalog, album.display_artist_off, &out_metadata->album_artist)) {
            media_metadata_build_release(out_metadata);
            return ESP_ERR_NO_MEM;
        }
    }

    for (uint16_t i = 0; i < track.artist_ref_count; ++i) {
        const TrackArtistRefV2 &ref = catalog->track_artist_refs[track.artist_ref_start + i];
        const char *name = ref.artist_id < catalog->artist_count
            ? media_catalog_v2_pool_str(catalog, catalog->artists[ref.artist_id].name_off)
            : nullptr;
        if (name == nullptr || !add_artist_owned(out_metadata, metadata_strdup(name))) {
            media_metadata_build_release(out_metadata);
            return ESP_ERR_NO_MEM;
        }
    }

    for (uint16_t i = 0; i < track.lyrics_ref_count; ++i) {
        const LyricsRefV2 &source = catalog->lyrics_refs[track.lyrics_ref_start + i];
        MediaMetadataLyricsBuildV2 lyrics = {};
        lyrics.data_offset = source.data_offset;
        lyrics.data_size = source.data_size;
        lyrics.flags = source.flags;
        lyrics.source = source.source;
        lyrics.kind = source.kind;
        lyrics.encoding = source.encoding;
        if (source.path_off != 0U) {
            const char *path = media_catalog_v2_pool_str(catalog, source.path_off);
            lyrics.path = metadata_strdup(path);
        }
        const char *language = media_catalog_v2_pool_str(catalog, source.language_off);
        lyrics.language = metadata_strdup(language != nullptr ? language : "");
        if ((source.path_off != 0U && lyrics.path == nullptr) || lyrics.language == nullptr ||
            !add_lyrics_owned(out_metadata, &lyrics)) {
            heap_caps_free(lyrics.path);
            heap_caps_free(lyrics.language);
            media_metadata_build_release(out_metadata);
            return ESP_ERR_NO_MEM;
        }
    }

    out_metadata->metadata_flags = track.metadata_flags;
    out_metadata->track_number = track.track_number;
    out_metadata->track_total = track.track_total;
    out_metadata->disc_number = track.disc_number;
    out_metadata->disc_total = track.disc_total;
    out_metadata->release_year = track.release_year;
    out_metadata->original_year = track.original_year;
    return ESP_OK;
}
