#pragma once

#include <stddef.h>

// Builds a compact ASCII initials key for quick library search.
// Han characters contribute one Mandarin pinyin initial each; consecutive
// Latin text contributes one uppercase initial per word; numeric words keep
// their first digit. Unsupported non-ASCII runs collapse to '#'.
// Returns the number of bytes written, excluding the trailing NUL.
size_t search_key_build_initials(const char *text, char *out, size_t out_size);
