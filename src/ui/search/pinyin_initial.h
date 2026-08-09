#pragma once

#include <stdint.h>

// Returns an uppercase A-Z initial for a covered CJK Unified Ideograph.
// Returns '\0' when the code point is not covered.
char pinyin_initial_for_codepoint(uint32_t codepoint);

// Convenience wrapper for the first UTF-8 code point.
char pinyin_initial_for_utf8(const char *text);
