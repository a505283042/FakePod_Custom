#include "search_key_builder.h"

#include <stdint.h>

#include "pinyin_initial.h"

namespace {

static bool is_ascii_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_ascii_leading_separator(uint8_t c)
{
    return is_ascii_space(c) || c == '.' || c == '-' || c == '_' ||
        c == '\'' || c == '"' || c == '(' || c == ')' || c == '[' ||
        c == ']' || c == '{' || c == '}';
}

static bool is_ascii_alpha(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_ascii_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

static char ascii_upper(uint8_t c)
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : static_cast<char>(c);
}

static size_t decode_utf8(const uint8_t *p, uint32_t *codepoint)
{
    if (p == nullptr || codepoint == nullptr || p[0] == 0U) {
        return 0U;
    }
    if (p[0] < 0x80U) {
        *codepoint = p[0];
        return 1U;
    }
    if ((p[0] & 0xE0U) == 0xC0U && p[1] != 0U && (p[1] & 0xC0U) == 0x80U) {
        const uint32_t cp = ((p[0] & 0x1FU) << 6U) | (p[1] & 0x3FU);
        if (cp >= 0x80U) {
            *codepoint = cp;
            return 2U;
        }
    }
    if ((p[0] & 0xF0U) == 0xE0U && p[1] != 0U && p[2] != 0U &&
        (p[1] & 0xC0U) == 0x80U && (p[2] & 0xC0U) == 0x80U) {
        const uint32_t cp = ((p[0] & 0x0FU) << 12U) |
            ((p[1] & 0x3FU) << 6U) | (p[2] & 0x3FU);
        if (cp >= 0x800U && !(cp >= 0xD800U && cp <= 0xDFFFU)) {
            *codepoint = cp;
            return 3U;
        }
    }
    if ((p[0] & 0xF8U) == 0xF0U && p[1] != 0U && p[2] != 0U && p[3] != 0U &&
        (p[1] & 0xC0U) == 0x80U && (p[2] & 0xC0U) == 0x80U && (p[3] & 0xC0U) == 0x80U) {
        const uint32_t cp = ((p[0] & 0x07U) << 18U) |
            ((p[1] & 0x3FU) << 12U) | ((p[2] & 0x3FU) << 6U) | (p[3] & 0x3FU);
        if (cp >= 0x10000U && cp <= 0x10FFFFU) {
            *codepoint = cp;
            return 4U;
        }
    }
    return 0U;
}

static void append_initial(char value, char *out, size_t out_size, size_t *length)
{
    if (out == nullptr || length == nullptr || out_size == 0U || *length + 1U >= out_size) {
        return;
    }
    // Unsupported Unicode runs are represented by one '#', not many.
    if (value == '#' && *length > 0U && out[*length - 1U] == '#') {
        return;
    }
    out[(*length)++] = value;
    out[*length] = '\0';
}

static const uint8_t *skip_common_track_number_prefix(const uint8_t *p)
{
    if (p == nullptr) {
        return p;
    }
    while (*p != 0U && is_ascii_leading_separator(*p)) {
        ++p;
    }
    if (!is_ascii_digit(*p)) {
        return p;
    }

    const uint8_t *digits = p;
    while (is_ascii_digit(*digits)) {
        ++digits;
    }
    const uint8_t *after = digits;
    while (*after != 0U && is_ascii_leading_separator(*after)) {
        ++after;
    }
    // Only drop the leading number when it is followed by separators and real text.
    return after > digits && *after != 0U ? after : p;
}

} // namespace

size_t search_key_build_initials(const char *text, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0U) {
        return 0U;
    }
    out[0] = '\0';
    if (text == nullptr || text[0] == '\0') {
        return 0U;
    }

    const uint8_t *p = skip_common_track_number_prefix(reinterpret_cast<const uint8_t *>(text));
    size_t length = 0U;
    bool ascii_word = false;

    while (*p != 0U && length + 1U < out_size) {
        const uint8_t c = *p;
        if (c < 0x80U) {
            if (is_ascii_alpha(c)) {
                if (!ascii_word) {
                    append_initial(ascii_upper(c), out, out_size, &length);
                }
                ascii_word = true;
            } else if (is_ascii_digit(c)) {
                if (!ascii_word) {
                    append_initial(static_cast<char>(c), out, out_size, &length);
                }
                ascii_word = true;
            } else if (c == '\'' && ascii_word) {
                // Apostrophes stay inside the same Latin word: STAYIN' -> S.
            } else {
                ascii_word = false;
            }
            ++p;
            continue;
        }

        uint32_t codepoint = 0U;
        const size_t consumed = decode_utf8(p, &codepoint);
        if (consumed == 0U) {
            append_initial('#', out, out_size, &length);
            ascii_word = false;
            ++p;
            continue;
        }

        const char initial = pinyin_initial_for_codepoint(codepoint);
        append_initial(initial != '\0' ? initial : '#', out, out_size, &length);
        ascii_word = false;
        p += consumed;
    }

    if (length == 0U) {
        append_initial('#', out, out_size, &length);
    }
    return length;
}
