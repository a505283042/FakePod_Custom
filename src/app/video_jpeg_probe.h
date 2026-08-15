#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_jpeg_dec.h"
#include "esp_log.h"

#if defined(__has_include)
#  if __has_include("esp_jpeg_version.h")
#    include "esp_jpeg_version.h"
#    define VIDEO_JPEG_PROFILE_HAS_VERSION 1
#  endif
#endif
#ifndef VIDEO_JPEG_PROFILE_HAS_VERSION
#  define VIDEO_JPEG_PROFILE_HAS_VERSION 0
#endif

// Video V1 的 MJPEG 输入契约：保持源宽高比，JPEG 宽高均不超过 460，
// Baseline canonical YUV420（Y=2x2，Cb=1x1，Cr=1x1）。ESP32 不做实时缩放；
// JPEG 原尺寸解码后由显示端在 460x460 CO5300 画布中局部居中提交。
namespace VideoJpegProbe
{
namespace detail
{

static constexpr const char *kTag = "VideoJPEG";
static constexpr uint16_t kMaxWidth = 460U;
static constexpr uint16_t kMaxHeight = 460U;

struct ComponentSampling
{
    uint8_t h = 0U;
    uint8_t v = 0U;
};

struct ProfileInfo
{
    bool syntax_ok = true;
    bool baseline = false;
    bool progressive = false;
    uint8_t precision = 0U;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint8_t component_count = 0U;
    ComponentSampling component[3] = {};
};

static uint32_t g_parse_sequence = 0U;
static uint32_t g_active_sequence = 0U;
static size_t g_active_input_size = 0U;
static bool g_codec_logged = false;
static bool g_profile_pass_logged = false;

static inline uint16_t read_be16(const uint8_t *p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8U) | p[1]);
}

static inline void log_codec_once()
{
    if (g_codec_logged) return;
    g_codec_logged = true;
#if VIDEO_JPEG_PROFILE_HAS_VERSION
    const char *version = esp_jpeg_get_version();
    ESP_LOGI(kTag,
        "R.40.3.3 Video V1 JPEG：esp_new_jpeg=%s profile=native<=460x460/Baseline/YUV420(2x2,1x1,1x1)",
        version != nullptr ? version : "unknown");
#else
    ESP_LOGI(kTag,
        "R.40.3.3 Video V1 JPEG：esp_new_jpeg=unknown profile=native<=460x460/Baseline/YUV420(2x2,1x1,1x1)");
#endif
}

static inline ProfileInfo inspect_profile(const uint8_t *data, size_t size)
{
    ProfileInfo out = {};
    if (data == nullptr || size < 4U || data[0] != 0xFFU || data[1] != 0xD8U) {
        out.syntax_ok = false;
        return out;
    }

    size_t pos = 2U;
    while (pos + 1U < size) {
        while (pos < size && data[pos] != 0xFFU) ++pos;
        if (pos >= size) break;
        while (pos < size && data[pos] == 0xFFU) ++pos;
        if (pos >= size) break;

        const uint8_t marker = data[pos++];
        if (marker == 0x00U) continue;
        if (marker == 0xD9U || marker == 0xDAU) break;
        if (marker == 0xD8U || marker == 0x01U ||
            (marker >= 0xD0U && marker <= 0xD7U)) {
            continue;
        }
        if (size - pos < 2U) {
            out.syntax_ok = false;
            break;
        }

        const uint16_t segment_len = read_be16(data + pos);
        if (segment_len < 2U || static_cast<size_t>(segment_len) > size - pos) {
            out.syntax_ok = false;
            break;
        }
        const uint8_t *payload = data + pos + 2U;
        const size_t payload_len = static_cast<size_t>(segment_len) - 2U;

        if (marker == 0xC0U || marker == 0xC2U) {
            out.baseline = marker == 0xC0U;
            out.progressive = marker == 0xC2U;
            if (payload_len < 6U) {
                out.syntax_ok = false;
                break;
            }
            out.precision = payload[0];
            out.height = read_be16(payload + 1U);
            out.width = read_be16(payload + 3U);
            out.component_count = payload[5];
            const size_t need = 6U + static_cast<size_t>(out.component_count) * 3U;
            if (need > payload_len) {
                out.syntax_ok = false;
                break;
            }
            const size_t store = out.component_count < 3U ? out.component_count : 3U;
            for (size_t i = 0U; i < store; ++i) {
                const uint8_t sampling = payload[6U + i * 3U + 1U];
                out.component[i].h = static_cast<uint8_t>((sampling >> 4U) & 0x0FU);
                out.component[i].v = static_cast<uint8_t>(sampling & 0x0FU);
            }
            break;
        }

        pos += static_cast<size_t>(segment_len);
    }
    return out;
}

static inline bool profile_supported(const ProfileInfo &p)
{
    return p.syntax_ok && p.baseline && !p.progressive &&
        p.precision == 8U && p.width > 0U && p.height > 0U &&
        p.width <= kMaxWidth && p.height <= kMaxHeight &&
        p.component_count == 3U &&
        p.component[0].h == 2U && p.component[0].v == 2U &&
        p.component[1].h == 1U && p.component[1].v == 1U &&
        p.component[2].h == 1U && p.component[2].v == 1U;
}

static inline void log_profile_reject(uint32_t sequence, const ProfileInfo &p, size_t bytes)
{
    ESP_LOGE(kTag,
        "Video V1 JPEG不兼容：seq=%lu bytes=%u syntax=%u baseline=%u progressive=%u p=%u %ux%u comps=%u sampling=%ux%u/%ux%u/%ux%u；要求宽高1..460、Baseline canonical YUV420=2x2/1x1/1x1",
        static_cast<unsigned long>(sequence), static_cast<unsigned>(bytes),
        p.syntax_ok ? 1U : 0U, p.baseline ? 1U : 0U, p.progressive ? 1U : 0U,
        static_cast<unsigned>(p.precision), static_cast<unsigned>(p.width),
        static_cast<unsigned>(p.height), static_cast<unsigned>(p.component_count),
        static_cast<unsigned>(p.component[0].h), static_cast<unsigned>(p.component[0].v),
        static_cast<unsigned>(p.component[1].h), static_cast<unsigned>(p.component[1].v),
        static_cast<unsigned>(p.component[2].h), static_cast<unsigned>(p.component[2].v));
}

} // namespace detail

static inline jpeg_error_t parse_header(
    jpeg_dec_handle_t decoder,
    jpeg_dec_io_t *io,
    jpeg_dec_header_info_t *info)
{
    detail::log_codec_once();
    const uint32_t sequence = ++detail::g_parse_sequence;
    const uint8_t *input = io != nullptr ? io->inbuf : nullptr;
    const size_t input_size = (io != nullptr && io->inbuf_len > 0)
        ? static_cast<size_t>(io->inbuf_len) : 0U;

    detail::g_active_sequence = sequence;
    detail::g_active_input_size = input_size;

    const detail::ProfileInfo profile = detail::inspect_profile(input, input_size);
    if (!detail::profile_supported(profile)) {
        detail::log_profile_reject(sequence, profile, input_size);
        return JPEG_ERR_BAD_DATA;
    }
    if (!detail::g_profile_pass_logged) {
        detail::g_profile_pass_logged = true;
        ESP_LOGI(detail::kTag,
            "Video V1 JPEG Profile PASS：%ux%u Baseline YUV420 sampling=2x2/1x1/1x1；native-size，无实时缩放",
            static_cast<unsigned>(profile.width), static_cast<unsigned>(profile.height));
    }

    const jpeg_error_t ret = ::jpeg_dec_parse_header(decoder, io, info);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(detail::kTag,
            "JPEG Header失败：seq=%lu bytes=%u jpeg_ret=%d",
            static_cast<unsigned long>(sequence), static_cast<unsigned>(input_size),
            static_cast<int>(ret));
    }
    return ret;
}

static inline jpeg_error_t process(jpeg_dec_handle_t decoder, jpeg_dec_io_t *io)
{
    const jpeg_error_t ret = ::jpeg_dec_process(decoder, io);
    if (ret != JPEG_ERR_OK) {
        const uintptr_t out_address = (io != nullptr && io->outbuf != nullptr)
            ? reinterpret_cast<uintptr_t>(io->outbuf) : 0U;
        ESP_LOGE(detail::kTag,
            "JPEG Process失败：seq=%lu bytes=%u jpeg_ret=%d out_align16=%u out_size=%d in_remain=%d",
            static_cast<unsigned long>(detail::g_active_sequence),
            static_cast<unsigned>(detail::g_active_input_size), static_cast<int>(ret),
            (out_address != 0U && (out_address & 0x0FU) == 0U) ? 1U : 0U,
            io != nullptr ? io->out_size : 0,
            io != nullptr ? io->inbuf_remain : 0);
    }
    return ret;
}

} // namespace VideoJpegProbe

// 仅影响本头之后、当前 video_benchmark.cpp 翻译单元内的现有调用。
#define jpeg_dec_parse_header(...) VideoJpegProbe::parse_header(__VA_ARGS__)
#define jpeg_dec_process(...) VideoJpegProbe::process(__VA_ARGS__)
