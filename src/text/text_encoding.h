#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace TextEncoding
{

enum class Encoding : uint8_t
{
    Utf8 = 0,
    Utf16Le,
    Utf16Be,
    Gbk,
};

enum class DecodeResult : uint8_t
{
    Ok = 0,
    Incomplete,
    Invalid,
};

struct Detection
{
    Encoding encoding = Encoding::Utf8;
    size_t bom_bytes = 0;
};

// 根据已有文本字节识别编码。
// UTF-16 仅接受 BOM；无 BOM 且不是有效 UTF-8 时，尝试 GBK/CP936。
esp_err_t detect(
    const uint8_t *data, size_t size, Detection *out_detection,
    bool allow_incomplete_tail = false);

// 解码一个源字符。source_bytes 返回该字符消耗的源字节数。
DecodeResult decode_one(
    Encoding encoding,
    const uint8_t *data,
    size_t size,
    size_t offset,
    uint32_t *out_codepoint,
    size_t *out_source_bytes);

// 把单个 Unicode code point 编码为 UTF-8。
size_t encode_utf8(uint32_t codepoint, char out[4]);

// 规范化纯文本里常见但容易被字体显示成方框的空白字符。
// 返回 0 表示该码点应忽略；其余返回值直接用于显示。
uint32_t normalize_plain_text_codepoint(uint32_t codepoint);

// 整段转成 UTF-8，结果使用 PSRAM 优先分配，调用方使用 heap_caps_free() 释放。
// UTF-8 输入同样会校验并去掉 BOM。
esp_err_t convert_to_utf8(
    const uint8_t *data,
    size_t size,
    char **out_utf8,
    size_t *out_size,
    Detection *out_detection = nullptr);

const char *encoding_name(Encoding encoding);

} // namespace TextEncoding
