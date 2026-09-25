#pragma once

#include <stddef.h>
#include <stdint.h>

// 按当前界面字体测量 UTF-8 文本宽度。
int32_t lyrics_text_measure_width(const char *text);

// 短句保持单行；超过 max_width 时按实际字形宽度寻找接近中点的断点，最多输出两行。
// 返回 true 表示输出中实际插入了换行；极端超长第二行会在末尾省略。
bool lyrics_text_format_balanced(
    const char *text,
    int32_t max_width,
    char *out,
    size_t out_size);
