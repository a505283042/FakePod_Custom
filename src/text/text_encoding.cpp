#include "text_encoding.h"

#include <string.h>

#include "esp_heap_caps.h"

namespace TextEncoding
{
namespace
{

#include "text_encoding_gbk_table.inc"

static void *text_alloc(size_t bytes)
{
    if (bytes == 0U) return nullptr;
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        memory = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return memory;
}

static bool gbk_trail_valid(uint8_t value)
{
    return (value >= 0x40U && value <= 0x7EU) || (value >= 0x80U && value <= 0xFEU);
}

static size_t gbk_trail_index(uint8_t value)
{
    return value <= 0x7EU
        ? static_cast<size_t>(value - 0x40U)
        : static_cast<size_t>(value - 0x41U);
}

static DecodeResult decode_utf8(
    const uint8_t *data,
    size_t size,
    size_t offset,
    uint32_t *out_codepoint,
    size_t *out_source_bytes)
{
    const uint8_t first = data[offset];
    if (first < 0x80U) {
        *out_codepoint = first;
        *out_source_bytes = 1U;
        return DecodeResult::Ok;
    }

    size_t continuation = 0U;
    uint32_t codepoint = 0U;
    uint32_t minimum = 0U;
    if ((first & 0xE0U) == 0xC0U) {
        continuation = 1U;
        codepoint = first & 0x1FU;
        minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
        continuation = 2U;
        codepoint = first & 0x0FU;
        minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
        continuation = 3U;
        codepoint = first & 0x07U;
        minimum = 0x10000U;
    } else {
        return DecodeResult::Invalid;
    }

    if (offset + continuation >= size) {
        return DecodeResult::Incomplete;
    }
    for (size_t i = 0U; i < continuation; ++i) {
        const uint8_t next = data[offset + i + 1U];
        if ((next & 0xC0U) != 0x80U) {
            return DecodeResult::Invalid;
        }
        codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if (codepoint < minimum || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        return DecodeResult::Invalid;
    }

    *out_codepoint = codepoint;
    *out_source_bytes = continuation + 1U;
    return DecodeResult::Ok;
}

static DecodeResult decode_utf16(
    const uint8_t *data,
    size_t size,
    size_t offset,
    bool little_endian,
    uint32_t *out_codepoint,
    size_t *out_source_bytes)
{
    if (offset + 1U >= size) {
        return DecodeResult::Incomplete;
    }
    const auto read_u16 = [little_endian](const uint8_t *p) -> uint16_t {
        return little_endian
            ? static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8U))
            : static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8U) | p[1]);
    };

    const uint16_t first = read_u16(data + offset);
    if (first >= 0xD800U && first <= 0xDBFFU) {
        if (offset + 3U >= size) {
            return DecodeResult::Incomplete;
        }
        const uint16_t second = read_u16(data + offset + 2U);
        if (second < 0xDC00U || second > 0xDFFFU) {
            return DecodeResult::Invalid;
        }
        *out_codepoint = 0x10000U +
            ((static_cast<uint32_t>(first - 0xD800U) << 10U) |
             static_cast<uint32_t>(second - 0xDC00U));
        *out_source_bytes = 4U;
        return DecodeResult::Ok;
    }
    if (first >= 0xDC00U && first <= 0xDFFFU) {
        return DecodeResult::Invalid;
    }

    *out_codepoint = first;
    *out_source_bytes = 2U;
    return DecodeResult::Ok;
}

static bool validate_encoding(
    const uint8_t *data, size_t size, Encoding encoding, bool allow_incomplete_tail)
{
    if (data == nullptr) return false;
    size_t offset = 0U;
    while (offset < size) {
        uint32_t codepoint = 0U;
        size_t source_bytes = 0U;
        const DecodeResult ret = decode_one(
            encoding, data, size, offset, &codepoint, &source_bytes);
        if (ret == DecodeResult::Incomplete) {
            return allow_incomplete_tail;
        }
        if (ret != DecodeResult::Ok || source_bytes == 0U) {
            return false;
        }
        offset += source_bytes;
    }
    return true;
}

} // namespace

DecodeResult decode_one(
    Encoding encoding,
    const uint8_t *data,
    size_t size,
    size_t offset,
    uint32_t *out_codepoint,
    size_t *out_source_bytes)
{
    if (data == nullptr || out_codepoint == nullptr || out_source_bytes == nullptr || offset >= size) {
        return DecodeResult::Invalid;
    }

    switch (encoding) {
        case Encoding::Utf8:
            return decode_utf8(data, size, offset, out_codepoint, out_source_bytes);
        case Encoding::Utf16Le:
            return decode_utf16(data, size, offset, true, out_codepoint, out_source_bytes);
        case Encoding::Utf16Be:
            return decode_utf16(data, size, offset, false, out_codepoint, out_source_bytes);
        case Encoding::Gbk: {
            const uint8_t first = data[offset];
            if (first < 0x80U) {
                *out_codepoint = first;
                *out_source_bytes = 1U;
                return DecodeResult::Ok;
            }
            if (first < 0x81U || first > 0xFEU) {
                return DecodeResult::Invalid;
            }
            if (offset + 1U >= size) {
                return DecodeResult::Incomplete;
            }
            const uint8_t second = data[offset + 1U];
            if (!gbk_trail_valid(second)) {
                return DecodeResult::Invalid;
            }
            const uint16_t mapped = kGbkToUnicode[first - 0x81U][gbk_trail_index(second)];
            if (mapped == 0U) {
                return DecodeResult::Invalid;
            }
            *out_codepoint = mapped;
            *out_source_bytes = 2U;
            return DecodeResult::Ok;
        }
        default:
            return DecodeResult::Invalid;
    }
}

size_t encode_utf8(uint32_t codepoint, char out[4])
{
    if (out == nullptr || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        return 0U;
    }
    if (codepoint <= 0x7FU) {
        out[0] = static_cast<char>(codepoint);
        return 1U;
    }
    if (codepoint <= 0x7FFU) {
        out[0] = static_cast<char>(0xC0U | (codepoint >> 6U));
        out[1] = static_cast<char>(0x80U | (codepoint & 0x3FU));
        return 2U;
    }
    if (codepoint <= 0xFFFFU) {
        out[0] = static_cast<char>(0xE0U | (codepoint >> 12U));
        out[1] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
        out[2] = static_cast<char>(0x80U | (codepoint & 0x3FU));
        return 3U;
    }
    out[0] = static_cast<char>(0xF0U | (codepoint >> 18U));
    out[1] = static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU));
    out[2] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
    out[3] = static_cast<char>(0x80U | (codepoint & 0x3FU));
    return 4U;
}

uint32_t normalize_plain_text_codepoint(uint32_t codepoint)
{
    switch (codepoint) {
        case 0x00A0U: // 不换行空格
        case 0x3000U: // 全角空格
            return ' ';
        case 0x200BU: // 零宽空格
        case 0xFEFFU: // 文本中嵌入的 BOM/零宽不换行空格
            return 0U;
        default:
            return codepoint;
    }
}

esp_err_t detect(
    const uint8_t *data, size_t size, Detection *out_detection, bool allow_incomplete_tail)
{
    if (data == nullptr || out_detection == nullptr || size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    Detection detected = {};
    if (size >= 3U && data[0] == 0xEFU && data[1] == 0xBBU && data[2] == 0xBFU) {
        detected.encoding = Encoding::Utf8;
        detected.bom_bytes = 3U;
        if (!validate_encoding(
                data + 3U, size - 3U, Encoding::Utf8, allow_incomplete_tail)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *out_detection = detected;
        return ESP_OK;
    }
    if (size >= 2U && data[0] == 0xFFU && data[1] == 0xFEU) {
        detected.encoding = Encoding::Utf16Le;
        detected.bom_bytes = 2U;
        if (!validate_encoding(
                data + 2U, size - 2U, Encoding::Utf16Le, allow_incomplete_tail)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *out_detection = detected;
        return ESP_OK;
    }
    if (size >= 2U && data[0] == 0xFEU && data[1] == 0xFFU) {
        detected.encoding = Encoding::Utf16Be;
        detected.bom_bytes = 2U;
        if (!validate_encoding(
                data + 2U, size - 2U, Encoding::Utf16Be, allow_incomplete_tail)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *out_detection = detected;
        return ESP_OK;
    }

    if (validate_encoding(data, size, Encoding::Utf8, allow_incomplete_tail)) {
        detected.encoding = Encoding::Utf8;
        *out_detection = detected;
        return ESP_OK;
    }
    if (validate_encoding(data, size, Encoding::Gbk, allow_incomplete_tail)) {
        detected.encoding = Encoding::Gbk;
        *out_detection = detected;
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t convert_to_utf8(
    const uint8_t *data,
    size_t size,
    char **out_utf8,
    size_t *out_size,
    Detection *out_detection)
{
    if (data == nullptr || size == 0U || out_utf8 == nullptr || out_size == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_utf8 = nullptr;
    *out_size = 0U;

    Detection detected = {};
    esp_err_t ret = detect(data, size, &detected);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t source = detected.bom_bytes;
    size_t utf8_bytes = 0U;
    while (source < size) {
        uint32_t codepoint = 0U;
        size_t source_bytes = 0U;
        const DecodeResult decoded = decode_one(
            detected.encoding, data, size, source, &codepoint, &source_bytes);
        if (decoded != DecodeResult::Ok || source_bytes == 0U) {
            return decoded == DecodeResult::Incomplete
                ? ESP_ERR_INVALID_SIZE
                : ESP_ERR_INVALID_RESPONSE;
        }
        char encoded[4] = {};
        const size_t bytes = encode_utf8(codepoint, encoded);
        if (bytes == 0U || utf8_bytes > SIZE_MAX - bytes - 1U) {
            return ESP_ERR_INVALID_SIZE;
        }
        utf8_bytes += bytes;
        source += source_bytes;
    }

    char *output = static_cast<char *>(text_alloc(utf8_bytes + 1U));
    if (output == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    source = detected.bom_bytes;
    size_t write = 0U;
    while (source < size) {
        uint32_t codepoint = 0U;
        size_t source_bytes = 0U;
        if (decode_one(detected.encoding, data, size, source, &codepoint, &source_bytes) != DecodeResult::Ok) {
            heap_caps_free(output);
            return ESP_ERR_INVALID_RESPONSE;
        }
        char encoded[4] = {};
        const size_t bytes = encode_utf8(codepoint, encoded);
        memcpy(output + write, encoded, bytes);
        write += bytes;
        source += source_bytes;
    }
    output[write] = '\0';

    *out_utf8 = output;
    *out_size = write;
    if (out_detection != nullptr) {
        *out_detection = detected;
    }
    return ESP_OK;
}

const char *encoding_name(Encoding encoding)
{
    switch (encoding) {
        case Encoding::Utf8: return "UTF-8";
        case Encoding::Utf16Le: return "UTF-16LE";
        case Encoding::Utf16Be: return "UTF-16BE";
        case Encoding::Gbk: return "GBK";
        default: return "Unknown";
    }
}

} // namespace TextEncoding
