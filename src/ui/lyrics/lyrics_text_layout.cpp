#include "lyrics_text_layout.h"

#include <limits.h>
#include <string.h>

#include "font/font_manager.h"
#include "lyrics_service.h"
#include "lvgl.h"

namespace {

static constexpr size_t kFormattedBytes = LYRICS_VIEW_TEXT_BYTES + 8U;

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

static int32_t measure_text_range(const char *begin, const char *end)
{
    if (begin == nullptr || end == nullptr || begin >= end) {
        return 0;
    }
    const lv_font_t *font = font_manager_get_ui_font();
    if (font == nullptr || font->get_glyph_dsc == nullptr) {
        return static_cast<int32_t>(end - begin) * 12;
    }

    int32_t width = 0;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(begin);
    const uint8_t *limit = reinterpret_cast<const uint8_t *>(end);
    while (p < limit && *p != 0U) {
        uint32_t cp = 0U;
        const size_t used = decode_utf8(p, &cp);
        if (used == 0U || p + used > limit) {
            ++p;
            continue;
        }
        uint32_t next_cp = 0U;
        if (p + used < limit) {
            decode_utf8(p + used, &next_cp);
        }
        width += static_cast<int32_t>(lv_font_get_glyph_width(font, cp, next_cp));
        p += used;
    }
    return width;
}

static bool is_ascii_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t';
}

static bool is_preferred_break_after(uint32_t cp)
{
    switch (cp) {
        case ',': case '.': case '!': case '?': case ';': case ':':
        case 0x3001U: // 、
        case 0x3002U: // 。
        case 0xFF0CU: // ，
        case 0xFF0EU: // ．
        case 0xFF01U: // ！
        case 0xFF1FU: // ？
        case 0xFF1BU: // ；
        case 0xFF1AU: // ：
            return true;
        default:
            return false;
    }
}

static bool is_bad_second_line_start(uint32_t cp)
{
    switch (cp) {
        case ',': case '.': case '!': case '?': case ';': case ':':
        case ')': case ']': case '}':
        case 0x3001U: case 0x3002U:
        case 0xFF0CU: case 0xFF0EU: case 0xFF01U: case 0xFF1FU:
        case 0xFF1BU: case 0xFF1AU:
        case 0xFF09U: // ）
        case 0x3011U: // 】
        case 0x300BU: // 》
        case 0x300DU: // 」
        case 0x300FU: // 』
            return true;
        default:
            return false;
    }
}

static size_t copy_trimmed_range(
    char *out,
    size_t out_size,
    const char *begin,
    const char *end)
{
    if (out == nullptr || out_size == 0U || begin == nullptr || end == nullptr || begin >= end) {
        if (out != nullptr && out_size > 0U) {
            out[0] = '\0';
        }
        return 0U;
    }
    while (begin < end && (*begin == ' ' || *begin == '\t')) {
        ++begin;
    }
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
    const size_t bytes = static_cast<size_t>(end - begin);
    const size_t copy_bytes = bytes < out_size - 1U ? bytes : out_size - 1U;
    memcpy(out, begin, copy_bytes);
    out[copy_bytes] = '\0';
    return copy_bytes;
}

static void copy_text(char *out, size_t out_size, const char *text)
{
    if (out == nullptr || out_size == 0U) {
        return;
    }
    if (text == nullptr) {
        out[0] = '\0';
        return;
    }
    const size_t bytes = strnlen(text, out_size - 1U);
    memcpy(out, text, bytes);
    out[bytes] = '\0';
}

static void truncate_to_width(char *text, size_t text_size, int32_t max_width)
{
    if (text == nullptr || text_size == 0U || text[0] == '\0' ||
        lyrics_text_measure_width(text) <= max_width) {
        return;
    }

    constexpr char suffix[] = "...";
    const int32_t suffix_width = lyrics_text_measure_width(suffix);
    const int32_t content_limit = max_width > suffix_width ? max_width - suffix_width : max_width;
    const char *begin = text;
    const char *p = text;
    const char *best_end = text;
    while (*p != '\0') {
        uint32_t cp = 0U;
        const size_t used = decode_utf8(reinterpret_cast<const uint8_t *>(p), &cp);
        if (used == 0U) {
            ++p;
            continue;
        }
        const char *candidate_end = p + used;
        if (measure_text_range(begin, candidate_end) > content_limit) {
            break;
        }
        best_end = candidate_end;
        p = candidate_end;
    }

    char tmp[kFormattedBytes] = {};
    const size_t kept = copy_trimmed_range(tmp, sizeof(tmp), begin, best_end);
    if (kept + sizeof(suffix) <= sizeof(tmp)) {
        memcpy(tmp + kept, suffix, sizeof(suffix));
    }
    copy_text(text, text_size, tmp);
}

} // namespace

int32_t lyrics_text_measure_width(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    return measure_text_range(text, text + strlen(text));
}

bool lyrics_text_format_balanced(
    const char *text,
    int32_t max_width,
    char *out,
    size_t out_size)
{
    if (out == nullptr || out_size == 0U) {
        return false;
    }
    out[0] = '\0';
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    const size_t total_bytes = strlen(text);
    const char *text_end = text + total_bytes;
    if (lyrics_text_measure_width(text) <= max_width) {
        copy_text(out, out_size, text);
        return false;
    }

    struct BreakChoice {
        const char *first_end = nullptr;
        const char *second_begin = nullptr;
        int32_t score = INT32_MAX;
        bool both_fit = false;
    } best;

    const char *p = text;
    while (p < text_end && *p != '\0') {
        uint32_t cp = 0U;
        const size_t used = decode_utf8(reinterpret_cast<const uint8_t *>(p), &cp);
        if (used == 0U) {
            ++p;
            continue;
        }
        const char *after = p + used;
        if (after >= text_end) {
            break;
        }

        const char *first_end = after;
        const char *second_begin = after;
        bool preferred = is_preferred_break_after(cp);
        if (is_ascii_space(cp)) {
            first_end = p;
            second_begin = after;
            preferred = true;
            while (second_begin < text_end && (*second_begin == ' ' || *second_begin == '\t')) {
                ++second_begin;
            }
        }
        if (first_end <= text || second_begin >= text_end) {
            p = after;
            continue;
        }

        uint32_t second_cp = 0U;
        decode_utf8(reinterpret_cast<const uint8_t *>(second_begin), &second_cp);
        const int32_t first_w = measure_text_range(text, first_end);
        const int32_t second_w = measure_text_range(second_begin, text_end);
        const bool first_fit = first_w <= max_width;
        const bool both_fit = first_fit && second_w <= max_width;
        if (!both_fit && !first_fit) {
            p = after;
            continue;
        }

        const int32_t imbalance = first_w > second_w ? first_w - second_w : second_w - first_w;
        int32_t score = both_fit ? imbalance : (max_width - first_w);
        if (preferred) {
            score -= 18;
        }
        if (is_bad_second_line_start(second_cp)) {
            score += 80;
        }

        if ((both_fit && !best.both_fit) ||
            (both_fit == best.both_fit && score < best.score)) {
            best.first_end = first_end;
            best.second_begin = second_begin;
            best.score = score;
            best.both_fit = both_fit;
        }
        p = after;
    }

    if (best.first_end == nullptr || best.second_begin == nullptr) {
        copy_text(out, out_size, text);
        return false;
    }

    char first[kFormattedBytes] = {};
    char second[kFormattedBytes] = {};
    copy_trimmed_range(first, sizeof(first), text, best.first_end);
    copy_trimmed_range(second, sizeof(second), best.second_begin, text_end);
    truncate_to_width(second, sizeof(second), max_width);

    const size_t first_bytes = strlen(first);
    const size_t second_bytes = strlen(second);
    if (first_bytes + second_bytes + 2U > out_size) {
        copy_text(out, out_size, text);
        return false;
    }
    memcpy(out, first, first_bytes);
    out[first_bytes] = '\n';
    memcpy(out + first_bytes + 1U, second, second_bytes + 1U);
    return true;
}
