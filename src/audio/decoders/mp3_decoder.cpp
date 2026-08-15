#include "mp3_decoder.h"

#include <limits.h>
#include <string.h>
#include "esp_audio_simple_dec.h"
#include "esp_audio_types.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"
#if APP_DIAG_MP3_PERFORMANCE
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#endif
#include "../audio_rate_profile.h"

static const char *TAG = "MP3";

// MP3 压缩输入工作区放 PSRAM。320kbps/44.1k 对比实测：8KB 窗口虽然 fread 更频繁，
// 但同步阻塞峰值约 6.9ms、refill 峰值约 11.2ms，优于 12KB 窗口的约 10.7/15.0ms。
// 正式配置保留 8KB，优先保证 AudioTask 最坏实时延迟；MP3 无需额外预取任务。
static constexpr size_t MP3_INPUT_BUFFER_BYTES = 8 * 1024;
// 继续保留 2KB 低水位。它高于常见 320kbps/44.1k 单帧约 1KB 的压缩尺寸，
// 可避免 parser 面对不完整帧时在“尚未低于补读阈值”的窗口中反复无进度。
static constexpr size_t MP3_INPUT_REFILL_LOW_WATER_BYTES = 2048;
// MPEG-1 Layer III 单帧最多 1152 样本；16bit 双声道通常只需 4608B。
// 这里预留 16KB，兼容 parser 一次返回更大的连续 PCM，并允许按 needed_size 扩容。
static constexpr size_t MP3_DECODED_BUFFER_BYTES = 16384;
static constexpr size_t MP3_MAX_DECODED_BUFFER_BYTES = 128 * 1024;

#if APP_DIAG_MP3_PERFORMANCE
static constexpr uint32_t MP3_PERF_REPORT_INTERVAL_US = 5000000U;
static constexpr uint32_t MP3_SLOW_READ_US = 5000U;
static constexpr uint32_t MP3_CRITICAL_READ_US = 10000U;
static constexpr uint32_t MP3_SLOW_DECODE_US = 5000U;
static constexpr uint32_t MP3_CRITICAL_DECODE_US = 10000U;
static constexpr uint32_t MP3_SLOW_REFILL_US = 10000U;
static constexpr uint32_t MP3_CRITICAL_REFILL_US = 20000U;

static portMUX_TYPE g_mp3_perf_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static Mp3PerfSnapshot g_mp3_perf_snapshot = {};
#endif

static bool g_mp3_backend_registered = false;

static uint8_t *mp3_alloc_buffer(size_t bytes)
{
    if (bytes == 0) {
        return nullptr;
    }
    // 大块 MP3 输入/PCM 工作区只使用 PSRAM，避免回退到内部 RAM 后侵占 DMA 余量。
    return static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void mp3_free_buffer(void *buffer)
{
    if (buffer != nullptr) {
        heap_caps_free(buffer);
    }
}

static esp_err_t mp3_audio_error_to_esp(esp_audio_err_t error)
{
    switch (error) {
        case ESP_AUDIO_ERR_OK:
            return ESP_OK;
        case ESP_AUDIO_ERR_MEM_LACK:
            return ESP_ERR_NO_MEM;
        case ESP_AUDIO_ERR_INVALID_PARAMETER:
            return ESP_ERR_INVALID_ARG;
        case ESP_AUDIO_ERR_NOT_SUPPORT:
            return ESP_ERR_NOT_SUPPORTED;
        case ESP_AUDIO_ERR_BUFF_NOT_ENOUGH:
            return ESP_ERR_INVALID_SIZE;
        default:
            return ESP_FAIL;
    }
}


static constexpr uint64_t MP3_SEEK_PREROLL_MS = 500ULL;
static constexpr uint64_t MP3_SEEK_SCAN_FORWARD_BYTES = 256ULL * 1024ULL;

struct Mp3SeekFrameHeader
{
    uint32_t sample_rate_hz = 0;
    uint32_t bitrate_kbps = 0;
    uint32_t frame_size_bytes = 0;
    uint16_t samples_per_frame = 0;
    uint8_t channels = 0;
    uint8_t version_id = 0;
    bool has_crc = false;
};

static uint16_t mp3_seek_read_be16(const uint8_t *p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static uint32_t mp3_seek_read_be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
        (static_cast<uint32_t>(p[1]) << 16) |
        (static_cast<uint32_t>(p[2]) << 8) |
        static_cast<uint32_t>(p[3]);
}

static bool mp3_seek_parse_frame_header(const uint8_t *h, Mp3SeekFrameHeader *out)
{
    if (h == nullptr || out == nullptr || h[0] != 0xFFU || (h[1] & 0xE0U) != 0xE0U) {
        return false;
    }
    const uint8_t version_id = (h[1] >> 3) & 0x03U;
    const uint8_t layer = (h[1] >> 1) & 0x03U;
    if (version_id == 1U || layer != 1U) {
        return false;
    }
    const uint8_t bitrate_index = (h[2] >> 4) & 0x0FU;
    const uint8_t sample_index = (h[2] >> 2) & 0x03U;
    if (bitrate_index == 0U || bitrate_index == 15U || sample_index == 3U) {
        return false;
    }

    static constexpr uint16_t kBitrateMpeg1[16] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
    };
    static constexpr uint16_t kBitrateMpeg2[16] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
    };
    static constexpr uint32_t kMpeg1Rates[3] = {44100, 48000, 32000};

    uint32_t sample_rate = kMpeg1Rates[sample_index];
    if (version_id == 2U) {
        sample_rate /= 2U;
    } else if (version_id == 0U) {
        sample_rate /= 4U;
    }
    const bool mpeg1 = version_id == 3U;
    const uint32_t bitrate = mpeg1 ? kBitrateMpeg1[bitrate_index] : kBitrateMpeg2[bitrate_index];
    const uint32_t padding = (h[2] >> 1) & 0x01U;
    const uint32_t frame_size = ((mpeg1 ? 144000U : 72000U) * bitrate) / sample_rate + padding;
    if (frame_size < 24U) {
        return false;
    }

    out->sample_rate_hz = sample_rate;
    out->bitrate_kbps = bitrate;
    out->frame_size_bytes = frame_size;
    out->samples_per_frame = mpeg1 ? 1152U : 576U;
    out->channels = ((h[3] >> 6) & 0x03U) == 3U ? 1U : 2U;
    out->version_id = version_id;
    out->has_crc = (h[1] & 0x01U) == 0U;
    return true;
}

static bool mp3_seek_headers_compatible(const Mp3SeekFrameHeader &a, const Mp3SeekFrameHeader &b)
{
    return a.sample_rate_hz == b.sample_rate_hz &&
        a.samples_per_frame == b.samples_per_frame &&
        a.version_id == b.version_id;
}

static uint32_t mp3_seek_side_info_size(const Mp3SeekFrameHeader &header)
{
    if (header.version_id == 3U) {
        return header.channels == 1U ? 17U : 32U;
    }
    return header.channels == 1U ? 9U : 17U;
}

static esp_err_t mp3_seek_read_exact_at(
    AudioSource *source,
    uint64_t offset,
    void *buffer,
    size_t bytes)
{
    if (source == nullptr || buffer == nullptr || offset > static_cast<uint64_t>(INT64_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = audio_source_seek(source, static_cast<int64_t>(offset), AudioSourceSeekOrigin::Begin);
    if (ret != ESP_OK) {
        return ret;
    }
    size_t got = 0;
    ret = audio_source_read(source, buffer, bytes, &got);
    return ret == ESP_OK && got == bytes ? ESP_OK : (ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE);
}

static esp_err_t mp3_seek_read_first_header(
    Mp3Decoder *decoder,
    const MediaTechnicalInfo *technical,
    Mp3SeekFrameHeader *out_header)
{
    if (decoder == nullptr || technical == nullptr || out_header == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t h[4] = {};
    esp_err_t ret = mp3_seek_read_exact_at(decoder->source, technical->audio_data_offset, h, sizeof(h));
    if (ret != ESP_OK || !mp3_seek_parse_frame_header(h, out_header)) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static bool mp3_seek_estimate_xing(
    Mp3Decoder *decoder,
    const MediaTechnicalInfo *technical,
    const Mp3SeekFrameHeader &first,
    uint64_t target_frame,
    uint64_t *out_offset)
{
    if (decoder == nullptr || technical == nullptr || out_offset == nullptr || technical->total_frames == 0) {
        return false;
    }
    const uint64_t xing_offset = technical->audio_data_offset + 4ULL +
        (first.has_crc ? 2ULL : 0ULL) + mp3_seek_side_info_size(first);
    uint8_t data[120] = {};
    if (mp3_seek_read_exact_at(decoder->source, xing_offset, data, sizeof(data)) != ESP_OK ||
        (memcmp(data, "Xing", 4) != 0 && memcmp(data, "Info", 4) != 0)) {
        return false;
    }

    const uint32_t flags = mp3_seek_read_be32(data + 4);
    size_t cursor = 8;
    if ((flags & 0x1U) != 0U) {
        cursor += 4;
    }
    uint64_t audio_bytes = decoder->file_size_bytes > technical->audio_data_offset
        ? decoder->file_size_bytes - technical->audio_data_offset
        : 0;
    if ((flags & 0x2U) != 0U) {
        if (cursor + 4 > sizeof(data)) {
            return false;
        }
        const uint32_t xing_bytes = mp3_seek_read_be32(data + cursor);
        if (xing_bytes > 0) {
            audio_bytes = xing_bytes;
        }
        cursor += 4;
    }
    if ((flags & 0x4U) == 0U || cursor + 100 > sizeof(data) || audio_bytes == 0) {
        return false;
    }

    const uint8_t *toc = data + cursor;
    uint64_t scaled = (target_frame * 10000ULL) / technical->total_frames;
    if (scaled > 9999ULL) {
        scaled = 9999ULL;
    }
    const uint32_t index = static_cast<uint32_t>(scaled / 100ULL);
    const uint32_t rem = static_cast<uint32_t>(scaled % 100ULL);
    const int32_t a = toc[index];
    const int32_t b = index < 99U ? toc[index + 1U] : 256;
    if (b < a) {
        return false;
    }
    const int32_t interp_x100 = a * 100 + (b - a) * static_cast<int32_t>(rem);
    const uint64_t rel = (audio_bytes * static_cast<uint64_t>(interp_x100)) / 25600ULL;
    *out_offset = technical->audio_data_offset + rel;
    return true;
}

static bool mp3_seek_estimate_vbri(
    Mp3Decoder *decoder,
    const MediaTechnicalInfo *technical,
    const Mp3SeekFrameHeader &first,
    uint64_t target_frame,
    uint64_t *out_offset)
{
    if (decoder == nullptr || technical == nullptr || out_offset == nullptr || first.samples_per_frame == 0) {
        return false;
    }
    const uint64_t vbri_offset = technical->audio_data_offset + 36ULL;
    uint8_t header[26] = {};
    if (mp3_seek_read_exact_at(decoder->source, vbri_offset, header, sizeof(header)) != ESP_OK ||
        memcmp(header, "VBRI", 4) != 0) {
        return false;
    }
    const uint32_t total_mpeg_frames = mp3_seek_read_be32(header + 14);
    const uint16_t entries = mp3_seek_read_be16(header + 18);
    const uint16_t scale = mp3_seek_read_be16(header + 20);
    const uint16_t entry_bytes = mp3_seek_read_be16(header + 22);
    const uint16_t frames_per_entry = mp3_seek_read_be16(header + 24);
    if (total_mpeg_frames == 0 || entries == 0 || entries > 4096U || scale == 0 ||
        entry_bytes == 0 || entry_bytes > 4U || frames_per_entry == 0) {
        return false;
    }

    uint64_t target_mpeg = target_frame / first.samples_per_frame;
    if (target_mpeg >= total_mpeg_frames) {
        target_mpeg = total_mpeg_frames - 1ULL;
    }
    uint64_t accumulated_frames = 0;
    uint64_t accumulated_bytes = 0;
    uint64_t table_cursor = vbri_offset + sizeof(header);
    for (uint16_t i = 0; i < entries; ++i) {
        uint8_t raw[4] = {};
        if (mp3_seek_read_exact_at(decoder->source, table_cursor, raw, entry_bytes) != ESP_OK) {
            return false;
        }
        table_cursor += entry_bytes;
        uint32_t value = 0;
        for (uint16_t j = 0; j < entry_bytes; ++j) {
            value = (value << 8) | raw[j];
        }
        const uint64_t span_bytes = static_cast<uint64_t>(value) * scale;
        const uint64_t next_frames = accumulated_frames + frames_per_entry;
        if (target_mpeg < next_frames) {
            const uint64_t inside = target_mpeg - accumulated_frames;
            accumulated_bytes += (span_bytes * inside) / frames_per_entry;
            *out_offset = technical->audio_data_offset + accumulated_bytes;
            return true;
        }
        accumulated_frames = next_frames;
        accumulated_bytes += span_bytes;
    }
    return false;
}

static esp_err_t mp3_seek_find_resync(
    Mp3Decoder *decoder,
    uint64_t estimate,
    const Mp3SeekFrameHeader &reference,
    uint64_t audio_start,
    uint64_t *out_offset)
{
    if (decoder == nullptr || decoder->source == nullptr || decoder->input_buffer == nullptr ||
        decoder->input_capacity < 8 || out_offset == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // estimate 已经针对 500ms pre-roll 计算；从估算点向前找下一帧，避免再向前
    // 额外回退字节后却仍把时间轴记成 base_frame，造成可预见的时间偏差。
    uint64_t cursor = estimate < audio_start ? audio_start : estimate;
    const uint64_t remaining_after_estimate = decoder->file_size_bytes > estimate
        ? decoder->file_size_bytes - estimate : 0ULL;
    const uint64_t limit = remaining_after_estimate > MP3_SEEK_SCAN_FORWARD_BYTES
        ? estimate + MP3_SEEK_SCAN_FORWARD_BYTES
        : decoder->file_size_bytes;

    while (cursor + 8ULL <= limit) {
        const size_t max_read = static_cast<size_t>(
            (limit - cursor) < decoder->input_capacity ? (limit - cursor) : decoder->input_capacity);
        if (max_read < 4) {
            break;
        }
        if (audio_source_seek(decoder->source, static_cast<int64_t>(cursor), AudioSourceSeekOrigin::Begin) != ESP_OK) {
            return ESP_FAIL;
        }
        size_t got = 0;
        esp_err_t ret = audio_source_read(decoder->source, decoder->input_buffer, max_read, &got);
        if (ret != ESP_OK) {
            return ret;
        }
        for (size_t i = 0; i + 4 <= got; ++i) {
            Mp3SeekFrameHeader current = {};
            if (!mp3_seek_parse_frame_header(decoder->input_buffer + i, &current) ||
                !mp3_seek_headers_compatible(reference, current)) {
                continue;
            }
            const uint64_t frame_offset = cursor + i;
            const uint64_t next_offset = frame_offset + current.frame_size_bytes;
            if (next_offset + 4ULL > decoder->file_size_bytes) {
                continue;
            }
            uint8_t next_raw[4] = {};
            if (mp3_seek_read_exact_at(decoder->source, next_offset, next_raw, sizeof(next_raw)) != ESP_OK) {
                continue;
            }
            Mp3SeekFrameHeader next = {};
            if (!mp3_seek_parse_frame_header(next_raw, &next) || !mp3_seek_headers_compatible(current, next)) {
                continue;
            }
            *out_offset = frame_offset;
            return audio_source_seek(decoder->source, static_cast<int64_t>(frame_offset), AudioSourceSeekOrigin::Begin);
        }
        if (got < max_read) {
            break;
        }
        cursor += got > 3 ? got - 3 : got;
    }
    return ESP_ERR_NOT_FOUND;
}

#if APP_DIAG_MP3_PERFORMANCE
static void mp3_perf_reset_runtime(Mp3Decoder *decoder)
{
    if (decoder == nullptr) {
        return;
    }

    decoder->perf_read_total_us = 0;
    decoder->perf_read_calls = 0;
    decoder->perf_read_max_us = 0;
    decoder->perf_read_over_5ms = 0;
    decoder->perf_read_over_10ms = 0;
    decoder->perf_input_compact_calls = 0;
    decoder->perf_input_compact_bytes = 0;
    decoder->perf_input_topup_bytes = 0;
    decoder->perf_input_topup_max_bytes = 0;

    decoder->perf_decode_total_us = 0;
    decoder->perf_decode_calls = 0;
    decoder->perf_decode_max_us = 0;
    decoder->perf_decode_over_5ms = 0;
    decoder->perf_decode_over_10ms = 0;

    decoder->perf_refill_total_us = 0;
    decoder->perf_refill_calls = 0;
    decoder->perf_refill_max_us = 0;
    decoder->perf_refill_over_10ms = 0;
    decoder->perf_refill_over_20ms = 0;
    decoder->perf_refill_process_total = 0;
    decoder->perf_refill_process_max = 0;
    decoder->perf_refill_multi_process = 0;
    decoder->perf_refill_input_fill_max = 0;
    decoder->perf_refill_multi_fill = 0;

    decoder->perf_output_frames_total = 0;
    decoder->perf_output_frames_min = 0;
    decoder->perf_output_frames_max = 0;
    decoder->perf_audio_budget_total_us = 0;
    decoder->perf_refill_over_budget = 0;
    decoder->perf_refill_worst_over_budget_us = 0;
    decoder->perf_refill_min_margin_us = 0;
    decoder->perf_refill_peak_load_percent = 0;
    decoder->perf_last_report_us = 0;
}

static void mp3_perf_maybe_publish(Mp3Decoder *decoder, uint64_t now_us)
{
    if (decoder == nullptr || decoder->perf_refill_calls == 0) {
        return;
    }
    if (decoder->perf_last_report_us == 0) {
        decoder->perf_last_report_us = now_us;
        return;
    }
    if (now_us - decoder->perf_last_report_us < MP3_PERF_REPORT_INTERVAL_US) {
        return;
    }

    Mp3PerfSnapshot snapshot = {};
    snapshot.active = true;
    snapshot.sample_rate_hz = decoder->sample_rate_hz;
    snapshot.bitrate = decoder->bitrate;

    snapshot.read_avg_us = decoder->perf_read_calls > 0
        ? static_cast<uint32_t>(decoder->perf_read_total_us / decoder->perf_read_calls)
        : 0;
    snapshot.read_max_us = decoder->perf_read_max_us;
    snapshot.read_over_5ms = decoder->perf_read_over_5ms;
    snapshot.read_over_10ms = decoder->perf_read_over_10ms;
    snapshot.read_calls = decoder->perf_read_calls;
    snapshot.compact_calls = decoder->perf_input_compact_calls;
    snapshot.compact_total_bytes = static_cast<uint32_t>(
        decoder->perf_input_compact_bytes > UINT32_MAX
            ? UINT32_MAX
            : decoder->perf_input_compact_bytes
    );
    snapshot.topup_avg_bytes = decoder->perf_read_calls > 0
        ? static_cast<uint32_t>(decoder->perf_input_topup_bytes / decoder->perf_read_calls)
        : 0;
    snapshot.topup_max_bytes = decoder->perf_input_topup_max_bytes;

    snapshot.decode_avg_us = decoder->perf_decode_calls > 0
        ? static_cast<uint32_t>(decoder->perf_decode_total_us / decoder->perf_decode_calls)
        : 0;
    snapshot.decode_max_us = decoder->perf_decode_max_us;
    snapshot.decode_over_5ms = decoder->perf_decode_over_5ms;
    snapshot.decode_over_10ms = decoder->perf_decode_over_10ms;
    snapshot.decode_calls = decoder->perf_decode_calls;

    snapshot.refill_avg_us = static_cast<uint32_t>(
        decoder->perf_refill_total_us / decoder->perf_refill_calls
    );
    snapshot.refill_max_us = decoder->perf_refill_max_us;
    snapshot.refill_over_10ms = decoder->perf_refill_over_10ms;
    snapshot.refill_over_20ms = decoder->perf_refill_over_20ms;
    snapshot.refill_calls = decoder->perf_refill_calls;
    snapshot.process_per_refill_x100 = static_cast<uint32_t>(
        (decoder->perf_refill_process_total * 100ULL + decoder->perf_refill_calls / 2U) /
        decoder->perf_refill_calls
    );
    snapshot.process_per_refill_max = decoder->perf_refill_process_max;
    snapshot.multi_process_refills = decoder->perf_refill_multi_process;
    snapshot.input_fills_per_refill_max = decoder->perf_refill_input_fill_max;
    snapshot.multi_fill_refills = decoder->perf_refill_multi_fill;

    snapshot.output_frames_avg = static_cast<uint32_t>(
        decoder->perf_output_frames_total / decoder->perf_refill_calls
    );
    snapshot.output_frames_min = decoder->perf_output_frames_min;
    snapshot.output_frames_max = decoder->perf_output_frames_max;
    snapshot.audio_budget_avg_us = static_cast<uint32_t>(
        decoder->perf_audio_budget_total_us / decoder->perf_refill_calls
    );
    snapshot.refill_over_budget = decoder->perf_refill_over_budget;
    snapshot.refill_worst_over_budget_us = decoder->perf_refill_worst_over_budget_us;
    snapshot.refill_min_margin_us = decoder->perf_refill_min_margin_us;
    snapshot.refill_avg_load_percent = decoder->perf_audio_budget_total_us > 0
        ? static_cast<uint32_t>(
            (decoder->perf_refill_total_us * 100ULL + decoder->perf_audio_budget_total_us / 2ULL) /
            decoder->perf_audio_budget_total_us
        )
        : 0;
    snapshot.refill_peak_load_percent = decoder->perf_refill_peak_load_percent;

    portENTER_CRITICAL(&g_mp3_perf_snapshot_mux);
    snapshot.sequence = g_mp3_perf_snapshot.sequence + 1U;
    g_mp3_perf_snapshot = snapshot;
    portEXIT_CRITICAL(&g_mp3_perf_snapshot_mux);

    decoder->perf_last_report_us = now_us;
}

bool mp3_decoder_get_perf_snapshot(Mp3PerfSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }

    portENTER_CRITICAL(&g_mp3_perf_snapshot_mux);
    *out_snapshot = g_mp3_perf_snapshot;
    portEXIT_CRITICAL(&g_mp3_perf_snapshot_mux);
    return out_snapshot->sequence != 0;
}
#endif

static esp_err_t mp3_resize_decoded_buffer(Mp3Decoder *decoder, size_t requested)
{
    if (decoder == nullptr || requested == 0 || requested > MP3_MAX_DECODED_BUFFER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (requested <= decoder->decoded_capacity) {
        return ESP_OK;
    }

    uint8_t *new_buffer = nullptr;
    esp_err_t reserve_ret = ESP_OK;
    if (decoder->workspace != nullptr) {
        reserve_ret = audio_decode_workspace_reserve_decoded(
            decoder->workspace, requested, &new_buffer);
    } else {
        new_buffer = mp3_alloc_buffer(requested);
        reserve_ret = new_buffer != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (reserve_ret != ESP_OK || new_buffer == nullptr) {
        ESP_LOGE(TAG, "MP3 PCM 输出缓冲分配失败：%u字节", static_cast<unsigned>(requested));
        return reserve_ret != ESP_OK ? reserve_ret : ESP_ERR_NO_MEM;
    }
    if (decoder->workspace == nullptr) {
        mp3_free_buffer(decoder->decoded_buffer);
    }
    decoder->decoded_buffer = new_buffer;
    // 即使共享 workspace 的实际容量更大，也保持 codec 可见窗口为 requested，
    // 避免切换格式后无意改变经过实机验证的 decoder refill 行为。
    decoder->decoded_capacity = requested;
    decoder->decoded_offset = 0;
    decoder->decoded_size = 0;
#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG, "MP3 PCM 输出缓冲调整为 %u 字节（shared=%u）",
        static_cast<unsigned>(requested),
        static_cast<unsigned>(decoder->workspace != nullptr));
#endif
    return ESP_OK;
}

static esp_err_t mp3_fill_input(Mp3Decoder *decoder, bool *out_read)
{
    if (out_read != nullptr) {
        *out_read = false;
    }
    if (decoder == nullptr || decoder->source == nullptr || decoder->input_buffer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t remaining = decoder->input_offset < decoder->input_size
        ? decoder->input_size - decoder->input_offset
        : 0;

    if (decoder->input_chunk_eos) {
        if (remaining == 0) {
            decoder->eof = true;
        }
        return ESP_OK;
    }

    // 当前窗口仍有足够压缩数据时直接交给 parser，减少 AudioTask 进入同步 Source read 的频率。
    if (remaining >= MP3_INPUT_REFILL_LOW_WATER_BYTES) {
        return ESP_OK;
    }

    if (remaining > 0 && decoder->input_offset > 0) {
        memmove(decoder->input_buffer, decoder->input_buffer + decoder->input_offset, remaining);
#if APP_DIAG_MP3_PERFORMANCE
        ++decoder->perf_input_compact_calls;
        decoder->perf_input_compact_bytes += remaining;
#endif
    }
    decoder->input_offset = 0;
    decoder->input_size = remaining;

    const size_t free_bytes = decoder->input_capacity - decoder->input_size;
    if (free_bytes == 0) {
        return ESP_OK;
    }

#if APP_DIAG_MP3_PERFORMANCE
    const int64_t read_begin_us = esp_timer_get_time();
#endif
    size_t read_bytes = 0;
    const esp_err_t source_ret = audio_source_read(
        decoder->source,
        decoder->input_buffer + decoder->input_size,
        free_bytes,
        &read_bytes
    );
#if APP_DIAG_MP3_PERFORMANCE
    const uint32_t read_us = static_cast<uint32_t>(esp_timer_get_time() - read_begin_us);
    decoder->perf_read_total_us += read_us;
    ++decoder->perf_read_calls;
    if (read_us > decoder->perf_read_max_us) {
        decoder->perf_read_max_us = read_us;
    }
    if (read_us >= MP3_SLOW_READ_US) {
        ++decoder->perf_read_over_5ms;
    }
    if (read_us >= MP3_CRITICAL_READ_US) {
        ++decoder->perf_read_over_10ms;
    }
    decoder->perf_input_topup_bytes += read_bytes;
    if (read_bytes > decoder->perf_input_topup_max_bytes) {
        decoder->perf_input_topup_max_bytes = static_cast<uint32_t>(read_bytes);
    }
#endif
    if (out_read != nullptr && read_bytes > 0) {
        *out_read = true;
    }
    decoder->input_size += read_bytes;

    if (source_ret != ESP_OK) {
        ESP_LOGE(TAG, "读取 MP3 压缩数据失败：source=%s ret=%s",
            audio_source_name(decoder->source), esp_err_to_name(source_ret));
        return source_ret;
    }
    if (read_bytes < free_bytes && audio_source_eof(decoder->source)) {
        decoder->input_chunk_eos = true;
    }

    if (decoder->input_size == 0 && decoder->input_chunk_eos) {
        decoder->eof = true;
    }
    return ESP_OK;
}

static esp_err_t mp3_verify_runtime_info(Mp3Decoder *decoder)
{
    if (decoder == nullptr || decoder->simple_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (decoder->runtime_info_verified) {
        return ESP_OK;
    }

    esp_audio_simple_dec_info_t info = {};
    const esp_audio_err_t codec_ret = esp_audio_simple_dec_get_info(
        static_cast<esp_audio_simple_dec_handle_t>(decoder->simple_handle),
        &info
    );
    if (codec_ret != ESP_AUDIO_ERR_OK) {
        return mp3_audio_error_to_esp(codec_ret);
    }

#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG, "乐鑫 MP3 解码输出：%luHz / %ubit / %u声道，bitrate=%lu",
        static_cast<unsigned long>(info.sample_rate),
        static_cast<unsigned>(info.bits_per_sample),
        static_cast<unsigned>(info.channel),
        static_cast<unsigned long>(info.bitrate));
#endif

    if ((info.sample_rate != 44100U && info.sample_rate != 48000U) ||
        !audio_rate_profile_get(info.sample_rate, nullptr)) {
        ESP_LOGE(TAG, "暂不支持该 MP3 采样率：%luHz（当前仅44.1/48kHz）",
            static_cast<unsigned long>(info.sample_rate));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (info.channel != 1U && info.channel != 2U) {
        ESP_LOGE(TAG, "暂不支持该 MP3 声道数：%u", static_cast<unsigned>(info.channel));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (info.bits_per_sample != 16U) {
        ESP_LOGE(TAG, "暂不支持该 MP3 PCM 位深：%u", static_cast<unsigned>(info.bits_per_sample));
        return ESP_ERR_NOT_SUPPORTED;
    }

    decoder->sample_rate_hz = info.sample_rate;
    decoder->channels = info.channel;
    decoder->bits_per_sample = info.bits_per_sample;
    decoder->bitrate = info.bitrate;
    decoder->runtime_info_verified = true;
    return ESP_OK;
}

static esp_err_t mp3_decode_next_output(Mp3Decoder *decoder)
{
    if (decoder == nullptr || decoder->simple_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

#if APP_DIAG_MP3_PERFORMANCE
    const int64_t refill_begin_us = esp_timer_get_time();
    uint32_t refill_process_calls = 0;
    uint32_t refill_input_fills = 0;
#endif
    decoder->decoded_offset = 0;
    decoder->decoded_size = 0;

    while (!decoder->eof) {
        bool input_read = false;
        esp_err_t ret = mp3_fill_input(decoder, &input_read);
#if APP_DIAG_MP3_PERFORMANCE
        if (input_read && ret == ESP_OK) {
            ++refill_input_fills;
        }
#endif
        if (ret != ESP_OK || decoder->eof) {
            return ret;
        }

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = decoder->input_buffer + decoder->input_offset;
        raw.len = static_cast<uint32_t>(decoder->input_size - decoder->input_offset);
        raw.eos = decoder->input_chunk_eos;

        esp_audio_simple_dec_out_t out = {};
        out.buffer = decoder->decoded_buffer;
        out.len = static_cast<uint32_t>(decoder->decoded_capacity);

#if APP_DIAG_MP3_PERFORMANCE
        const int64_t decode_begin_us = esp_timer_get_time();
        ++refill_process_calls;
#endif
        const esp_audio_err_t codec_ret = esp_audio_simple_dec_process(
            static_cast<esp_audio_simple_dec_handle_t>(decoder->simple_handle),
            &raw,
            &out
        );
#if APP_DIAG_MP3_PERFORMANCE
        const uint32_t decode_us = static_cast<uint32_t>(esp_timer_get_time() - decode_begin_us);
        decoder->perf_decode_total_us += decode_us;
        ++decoder->perf_decode_calls;
        if (decode_us > decoder->perf_decode_max_us) {
            decoder->perf_decode_max_us = decode_us;
        }
        if (decode_us >= MP3_SLOW_DECODE_US) {
            ++decoder->perf_decode_over_5ms;
        }
        if (decode_us >= MP3_CRITICAL_DECODE_US) {
            ++decoder->perf_decode_over_10ms;
        }
#endif
        if (codec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (out.needed_size == 0 || out.needed_size > MP3_MAX_DECODED_BUFFER_BYTES) {
                ESP_LOGE(TAG, "MP3 解码器请求异常输出缓冲：%lu字节",
                    static_cast<unsigned long>(out.needed_size));
                return ESP_ERR_INVALID_SIZE;
            }
            ret = mp3_resize_decoded_buffer(decoder, out.needed_size);
            if (ret != ESP_OK) {
                return ret;
            }
            continue;
        }
        if (codec_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "乐鑫 MP3 解码失败：codec_ret=%d", static_cast<int>(codec_ret));
            return mp3_audio_error_to_esp(codec_ret);
        }
        if (raw.consumed > raw.len) {
            ESP_LOGE(TAG, "MP3 解码器报告非法 consumed=%lu/%lu",
                static_cast<unsigned long>(raw.consumed),
                static_cast<unsigned long>(raw.len));
            return ESP_ERR_INVALID_RESPONSE;
        }

        decoder->input_offset += raw.consumed;
        if (out.decoded_size > 0) {
            ret = mp3_verify_runtime_info(decoder);
            if (ret != ESP_OK) {
                return ret;
            }
            decoder->decoded_size = out.decoded_size;
            decoder->decoded_offset = 0;

#if APP_DIAG_MP3_PERFORMANCE
            const uint32_t refill_us = static_cast<uint32_t>(
                esp_timer_get_time() - refill_begin_us
            );
            const size_t frame_bytes = sizeof(int16_t) * decoder->channels;
            const uint32_t output_frames = frame_bytes > 0
                ? static_cast<uint32_t>(out.decoded_size / frame_bytes)
                : 0;
            const uint32_t audio_budget_us = decoder->sample_rate_hz > 0 && output_frames > 0
                ? static_cast<uint32_t>(
                    (static_cast<uint64_t>(output_frames) * 1000000ULL) /
                    decoder->sample_rate_hz
                )
                : 0;

            decoder->perf_refill_total_us += refill_us;
            ++decoder->perf_refill_calls;
            if (refill_us > decoder->perf_refill_max_us) {
                decoder->perf_refill_max_us = refill_us;
            }
            if (refill_us >= MP3_SLOW_REFILL_US) {
                ++decoder->perf_refill_over_10ms;
            }
            if (refill_us >= MP3_CRITICAL_REFILL_US) {
                ++decoder->perf_refill_over_20ms;
            }
            decoder->perf_refill_process_total += refill_process_calls;
            if (refill_process_calls > decoder->perf_refill_process_max) {
                decoder->perf_refill_process_max = refill_process_calls;
            }
            if (refill_process_calls > 1U) {
                ++decoder->perf_refill_multi_process;
            }
            if (refill_input_fills > decoder->perf_refill_input_fill_max) {
                decoder->perf_refill_input_fill_max = refill_input_fills;
            }
            if (refill_input_fills > 1U) {
                ++decoder->perf_refill_multi_fill;
            }

            decoder->perf_output_frames_total += output_frames;
            if (decoder->perf_output_frames_min == 0 || output_frames < decoder->perf_output_frames_min) {
                decoder->perf_output_frames_min = output_frames;
            }
            if (output_frames > decoder->perf_output_frames_max) {
                decoder->perf_output_frames_max = output_frames;
            }
            decoder->perf_audio_budget_total_us += audio_budget_us;

            if (audio_budget_us > 0) {
                const int32_t margin_us = static_cast<int32_t>(audio_budget_us) -
                    static_cast<int32_t>(refill_us);
                if (decoder->perf_refill_calls == 1U || margin_us < decoder->perf_refill_min_margin_us) {
                    decoder->perf_refill_min_margin_us = margin_us;
                }
                if (refill_us >= audio_budget_us) {
                    ++decoder->perf_refill_over_budget;
                    const uint32_t over_us = refill_us - audio_budget_us;
                    if (over_us > decoder->perf_refill_worst_over_budget_us) {
                        decoder->perf_refill_worst_over_budget_us = over_us;
                    }
                }
                const uint32_t load_percent = static_cast<uint32_t>(
                    (static_cast<uint64_t>(refill_us) * 100ULL + audio_budget_us / 2U) /
                    audio_budget_us
                );
                if (load_percent > decoder->perf_refill_peak_load_percent) {
                    decoder->perf_refill_peak_load_percent = load_percent;
                }
            }

            mp3_perf_maybe_publish(decoder, static_cast<uint64_t>(esp_timer_get_time()));
#endif
            return ESP_OK;
        }

        // parser 允许本轮只消费标签/不完整帧而暂时没有 PCM；必须保证循环有前进。
        if (raw.consumed == 0) {
            if (decoder->input_chunk_eos) {
                decoder->eof = true;
                return ESP_OK;
            }
            if (decoder->input_size - decoder->input_offset >= decoder->input_capacity) {
                ESP_LOGE(TAG, "MP3 parser 在完整输入窗口中无进度，拒绝死循环");
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    return ESP_OK;
}


static esp_err_t mp3_seek_reopen_at(
    Mp3Decoder *decoder,
    uint64_t source_offset,
    uint64_t base_frame)
{
    if (decoder == nullptr || decoder->source == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t expected_rate = decoder->sample_rate_hz;
    const uint16_t expected_channels = decoder->channels;
    const uint16_t expected_bits = decoder->bits_per_sample;

    if (decoder->simple_handle != nullptr) {
        esp_audio_simple_dec_close(static_cast<esp_audio_simple_dec_handle_t>(decoder->simple_handle));
        decoder->simple_handle = nullptr;
    }
    esp_err_t ret = audio_source_seek(
        decoder->source, static_cast<int64_t>(source_offset), AudioSourceSeekOrigin::Begin);
    if (ret != ESP_OK) {
        return ret;
    }

    decoder->input_offset = 0;
    decoder->input_size = 0;
    decoder->input_chunk_eos = false;
    decoder->decoded_offset = 0;
    decoder->decoded_size = 0;
    decoder->runtime_info_verified = false;
    decoder->eof = false;
    decoder->frames_read = base_frame;

    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    cfg.dec_cfg = nullptr;
    cfg.cfg_size = 0;
    cfg.use_frame_dec = false;
    esp_audio_simple_dec_handle_t handle = nullptr;
    const esp_audio_err_t codec_ret = esp_audio_simple_dec_open(&cfg, &handle);
    if (codec_ret != ESP_AUDIO_ERR_OK || handle == nullptr) {
        return mp3_audio_error_to_esp(codec_ret);
    }
    decoder->simple_handle = handle;

    ret = mp3_decode_next_output(decoder);
    if (ret != ESP_OK || decoder->decoded_size == 0 || !decoder->runtime_info_verified) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }
    if (decoder->sample_rate_hz != expected_rate || decoder->channels != expected_channels ||
        decoder->bits_per_sample != expected_bits) {
        ESP_LOGE(TAG, "MP3 seek 后输出格式变化：before=%lu/%u/%u after=%lu/%u/%u",
            static_cast<unsigned long>(expected_rate),
            static_cast<unsigned>(expected_bits),
            static_cast<unsigned>(expected_channels),
            static_cast<unsigned long>(decoder->sample_rate_hz),
            static_cast<unsigned>(decoder->bits_per_sample),
            static_cast<unsigned>(decoder->channels));
        return ESP_ERR_INVALID_RESPONSE;
    }
#if APP_DIAG_MP3_PERFORMANCE
    mp3_perf_reset_runtime(decoder);
#endif
    return ESP_OK;
}

static esp_err_t mp3_seek_discard_to(Mp3Decoder *decoder, uint64_t target_frame)
{
    if (decoder == nullptr || decoder->channels == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t frame_bytes = sizeof(int16_t) * decoder->channels;
    while (decoder->frames_read < target_frame) {
        if (decoder->decoded_offset >= decoder->decoded_size) {
            esp_err_t ret = mp3_decode_next_output(decoder);
            if (ret != ESP_OK) {
                return ret;
            }
            if (decoder->decoded_offset >= decoder->decoded_size) {
                return decoder->eof ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_STATE;
            }
        }
        const uint64_t needed = target_frame - decoder->frames_read;
        const size_t available = (decoder->decoded_size - decoder->decoded_offset) / frame_bytes;
        const size_t discard = needed < available ? static_cast<size_t>(needed) : available;
        decoder->decoded_offset += discard * frame_bytes;
        decoder->frames_read += discard;
    }
    return ESP_OK;
}

esp_err_t mp3_decoder_seek_frame(
    Mp3Decoder *decoder,
    uint64_t target_frame,
    const MediaTechnicalInfo *technical_info,
    uint64_t *out_frame,
    uint64_t *out_source_offset,
    Mp3SeekMethod *out_method)
{
    if (out_frame != nullptr) *out_frame = 0;
    if (out_source_offset != nullptr) *out_source_offset = 0;
    if (out_method != nullptr) *out_method = Mp3SeekMethod::None;
    if (decoder == nullptr || !mp3_decoder_is_open(decoder) || technical_info == nullptr ||
        (technical_info->flags & MEDIA_TECH_PARSED) == 0U ||
        technical_info->audio_data_offset >= decoder->file_size_bytes ||
        technical_info->sample_rate_hz != decoder->sample_rate_hz) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint64_t total_frames = technical_info->total_frames;
    if (total_frames == 0) {
        total_frames = decoder->total_frames;
    }
    if (total_frames > 0 && target_frame >= total_frames) {
        target_frame = total_frames - 1ULL;
    }

    Mp3SeekFrameHeader first = {};
    esp_err_t ret = mp3_seek_read_first_header(decoder, technical_info, &first);
    if (ret != ESP_OK || first.sample_rate_hz != decoder->sample_rate_hz) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }

    const uint64_t preroll_frames =
        (static_cast<uint64_t>(decoder->sample_rate_hz) * MP3_SEEK_PREROLL_MS) / 1000ULL;
    const uint64_t base_frame = target_frame > preroll_frames ? target_frame - preroll_frames : 0ULL;
    uint64_t estimate = technical_info->audio_data_offset;
    Mp3SeekMethod method = Mp3SeekMethod::None;

    if ((technical_info->flags & MEDIA_TECH_HAS_VBR_HEADER) != 0U && total_frames > 0 &&
        mp3_seek_estimate_xing(decoder, technical_info, first, base_frame, &estimate)) {
        method = Mp3SeekMethod::XingToc;
    } else if ((technical_info->flags & MEDIA_TECH_HAS_VBR_HEADER) != 0U &&
        mp3_seek_estimate_vbri(decoder, technical_info, first, base_frame, &estimate)) {
        method = Mp3SeekMethod::Vbri;
    } else if ((technical_info->flags & MEDIA_TECH_DURATION_ESTIMATED) != 0U &&
        technical_info->bitrate_kbps > 0U) {
        const uint64_t base_ms = decoder->sample_rate_hz > 0
            ? (base_frame * 1000ULL) / decoder->sample_rate_hz : 0ULL;
        estimate = technical_info->audio_data_offset +
            (base_ms * static_cast<uint64_t>(technical_info->bitrate_kbps)) / 8ULL;
        method = Mp3SeekMethod::CbrLinear;
    } else if (total_frames > 0 && decoder->file_size_bytes > technical_info->audio_data_offset) {
        const uint64_t audio_bytes = decoder->file_size_bytes - technical_info->audio_data_offset;
        estimate = technical_info->audio_data_offset + (audio_bytes * base_frame) / total_frames;
        method = Mp3SeekMethod::VbrLinearFallback;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (estimate >= decoder->file_size_bytes) {
        estimate = decoder->file_size_bytes - 1ULL;
    }

    uint64_t sync_offset = 0;
    ret = mp3_seek_find_resync(
        decoder, estimate, first, technical_info->audio_data_offset, &sync_offset);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MP3 seek 未能在估算位置附近重新同步 MPEG frame：estimate=%llu ret=%s",
            static_cast<unsigned long long>(estimate), esp_err_to_name(ret));
        return ret;
    }

    ret = mp3_seek_reopen_at(decoder, sync_offset, base_frame);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = mp3_seek_discard_to(decoder, target_frame);
    if (ret != ESP_OK) {
        return ret;
    }

    if (out_frame != nullptr) *out_frame = decoder->frames_read;
    if (out_source_offset != nullptr) *out_source_offset = sync_offset;
    if (out_method != nullptr) *out_method = method;
#if APP_DIAG_AUDIO_SEEK
    ESP_LOGI(TAG,
        "SEEK_TRACE: MP3 method=%u requested=%llu base=%llu actual=%llu estimate=%llu sync=%llu preroll=%llums",
        static_cast<unsigned>(method),
        static_cast<unsigned long long>(target_frame),
        static_cast<unsigned long long>(base_frame),
        static_cast<unsigned long long>(decoder->frames_read),
        static_cast<unsigned long long>(estimate),
        static_cast<unsigned long long>(sync_offset),
        static_cast<unsigned long long>(MP3_SEEK_PREROLL_MS));
#endif
    return ESP_OK;
}

esp_err_t mp3_decoder_register_backend()
{
    if (g_mp3_backend_registered) {
        return ESP_OK;
    }

    const esp_audio_err_t ret = esp_mp3_dec_register();
    if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_ALREADY_EXIST) {
        ESP_LOGE(TAG, "注册乐鑫 MP3 解码器失败：codec_ret=%d", static_cast<int>(ret));
        return mp3_audio_error_to_esp(ret);
    }

    g_mp3_backend_registered = true;
#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(TAG, "乐鑫 MP3 解码后端注册成功");
#endif
    return ESP_OK;
}

esp_err_t mp3_decoder_open(
    Mp3Decoder *decoder,
    AudioSource *source,
    AudioDecodeWorkspace *workspace,
    bool streaming_source)
{
    if (decoder == nullptr || !audio_source_is_open(source)) {
        return ESP_ERR_INVALID_ARG;
    }
    mp3_decoder_close(decoder);

    esp_err_t ret = mp3_decoder_register_backend();
    if (ret != ESP_OK) {
        return ret;
    }

    decoder->source = source;
    decoder->streaming_source = streaming_source;
    if (!audio_source_has_capability(source, AUDIO_SOURCE_CAP_READ)) {
        mp3_decoder_close(decoder);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (streaming_source) {
        if (!audio_source_has_capability(source, AUDIO_SOURCE_CAP_STREAMING)) {
            mp3_decoder_close(decoder);
            return ESP_ERR_NOT_SUPPORTED;
        }
        decoder->file_size_bytes = 0ULL;
    } else {
        if (!audio_source_has_capability(source, AUDIO_SOURCE_CAP_SEEK) ||
            !audio_source_has_capability(source, AUDIO_SOURCE_CAP_SIZE)) {
            mp3_decoder_close(decoder);
            return ESP_ERR_NOT_SUPPORTED;
        }
        uint64_t file_size = 0;
        ret = audio_source_size(source, &file_size);
        if (ret != ESP_OK || file_size == 0) {
            mp3_decoder_close(decoder);
            return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
        }
        ret = audio_source_seek(source, 0, AudioSourceSeekOrigin::Begin);
        if (ret != ESP_OK) {
            mp3_decoder_close(decoder);
            return ret;
        }
        decoder->file_size_bytes = file_size;
    }

    decoder->workspace = workspace;
    if (workspace != nullptr) {
        ret = audio_decode_workspace_reserve_input(
            workspace, MP3_INPUT_BUFFER_BYTES, &decoder->input_buffer);
        if (ret == ESP_OK) {
            ret = audio_decode_workspace_reserve_decoded(
                workspace, MP3_DECODED_BUFFER_BYTES, &decoder->decoded_buffer);
        }
    } else {
        decoder->input_buffer = mp3_alloc_buffer(MP3_INPUT_BUFFER_BYTES);
        decoder->decoded_buffer = mp3_alloc_buffer(MP3_DECODED_BUFFER_BYTES);
        ret = decoder->input_buffer != nullptr && decoder->decoded_buffer != nullptr
            ? ESP_OK
            : ESP_ERR_NO_MEM;
    }
    if (ret != ESP_OK || decoder->input_buffer == nullptr || decoder->decoded_buffer == nullptr) {
        ESP_LOGE(TAG, "MP3 流缓冲分配失败：输入=%u 输出=%u shared=%u",
            static_cast<unsigned>(MP3_INPUT_BUFFER_BYTES),
            static_cast<unsigned>(MP3_DECODED_BUFFER_BYTES),
            static_cast<unsigned>(workspace != nullptr));
        mp3_decoder_close(decoder);
        return ret != ESP_OK ? ret : ESP_ERR_NO_MEM;
    }
    // 保持 Stage 9.5 实机验证的逻辑窗口大小，不因 workspace 曾被 FLAC 扩大而变成 32KB fread。
    decoder->input_capacity = MP3_INPUT_BUFFER_BYTES;
    decoder->decoded_capacity = MP3_DECODED_BUFFER_BYTES;

    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    cfg.dec_cfg = nullptr;
    cfg.cfg_size = 0;
    cfg.use_frame_dec = false;

    esp_audio_simple_dec_handle_t handle = nullptr;
    const esp_audio_err_t codec_ret = esp_audio_simple_dec_open(&cfg, &handle);
    if (codec_ret != ESP_AUDIO_ERR_OK || handle == nullptr) {
        ESP_LOGE(TAG, "打开乐鑫 MP3 Simple Decoder 失败：codec_ret=%d", static_cast<int>(codec_ret));
        mp3_decoder_close(decoder);
        return mp3_audio_error_to_esp(codec_ret);
    }
    decoder->simple_handle = handle;

    // 与 FLAC 一致：在 I2S/DAC 启动前先解出第一块 PCM，确认真实格式并预热解码器。
    ret = mp3_decode_next_output(decoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MP3 首块 PCM 预解码失败：%s", esp_err_to_name(ret));
        mp3_decoder_close(decoder);
        return ret;
    }
    if (decoder->decoded_size == 0 || !decoder->runtime_info_verified) {
        ESP_LOGE(TAG, "MP3 首块 PCM 为空或输出格式未确认，拒绝启动播放链路");
        mp3_decoder_close(decoder);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t source_frame_bytes = sizeof(int16_t) * decoder->channels;
    if (decoder->decoded_size % source_frame_bytes != 0) {
        ESP_LOGE(TAG, "MP3 首块 PCM 长度与 16bit/%u声道不整除：%u字节",
            static_cast<unsigned>(decoder->channels),
            static_cast<unsigned>(decoder->decoded_size));
        mp3_decoder_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }

#if APP_DIAG_MP3_PERFORMANCE
    // 首块预解码发生在 I2S 启动前，不属于实时播放负载；正式统计从这里重新开始。
    mp3_perf_reset_runtime(decoder);
#endif

#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG, "MP3 decoder ready: source=%s streaming=%u file=%lluB input=%uB pcm=%uB first_pcm=%uB PSRAM",
        audio_source_name(decoder->source),
        static_cast<unsigned>(decoder->streaming_source),
        static_cast<unsigned long long>(decoder->file_size_bytes),
        static_cast<unsigned>(decoder->input_capacity),
        static_cast<unsigned>(decoder->decoded_capacity),
        static_cast<unsigned>(decoder->decoded_size));
#endif
    return ESP_OK;
}

esp_err_t mp3_decoder_read_pcm32(
    Mp3Decoder *decoder,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames)
{
    if (out_frames != nullptr) {
        *out_frames = 0;
    }
    if (decoder == nullptr || out_interleaved_stereo == nullptr || out_frames == nullptr ||
        max_frames == 0 || !mp3_decoder_is_open(decoder)) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t source_frame_bytes = sizeof(int16_t) * decoder->channels;
    size_t produced = 0;

    while (produced < max_frames) {
        if (decoder->decoded_offset >= decoder->decoded_size) {
            esp_err_t ret = mp3_decode_next_output(decoder);
            if (ret != ESP_OK) {
                return ret;
            }
            if (decoder->decoded_offset >= decoder->decoded_size) {
                break;
            }
            if (decoder->decoded_size % source_frame_bytes != 0) {
                ESP_LOGE(TAG, "MP3 PCM 输出长度与 16bit/%u声道不整除：%u字节",
                    static_cast<unsigned>(decoder->channels),
                    static_cast<unsigned>(decoder->decoded_size));
                return ESP_ERR_INVALID_SIZE;
            }
        }

        const size_t available_frames =
            (decoder->decoded_size - decoder->decoded_offset) / source_frame_bytes;
        const size_t copy_frames = (max_frames - produced) < available_frames
            ? (max_frames - produced)
            : available_frames;

        const uint8_t *src = decoder->decoded_buffer + decoder->decoded_offset;
        for (size_t i = 0; i < copy_frames; ++i) {
            int16_t left = 0;
            memcpy(&left, src, sizeof(left));
            int16_t right = left;
            if (decoder->channels == 2U) {
                memcpy(&right, src + sizeof(int16_t), sizeof(right));
            }
            out_interleaved_stereo[(produced + i) * 2] = static_cast<int32_t>(left) * 65536;
            out_interleaved_stereo[(produced + i) * 2 + 1] = static_cast<int32_t>(right) * 65536;
            src += source_frame_bytes;
        }

        const size_t consumed_bytes = copy_frames * source_frame_bytes;
        decoder->decoded_offset += consumed_bytes;
        decoder->frames_read += copy_frames;
        produced += copy_frames;
    }

    *out_frames = produced;
    return ESP_OK;
}

void mp3_decoder_close(Mp3Decoder *decoder)
{
    if (decoder == nullptr) {
        return;
    }
    if (decoder->simple_handle != nullptr) {
        esp_audio_simple_dec_close(static_cast<esp_audio_simple_dec_handle_t>(decoder->simple_handle));
    }
    // Source 生命周期由 PcmDecoder 所有；这里仅关闭 Codec，切歌时底层 Source 最后统一关闭。
    // 使用共享 workspace 时缓冲归 AudioTask 生命周期所有，切歌只关闭 codec，不释放 PSRAM。
    if (decoder->workspace == nullptr) {
        mp3_free_buffer(decoder->input_buffer);
        mp3_free_buffer(decoder->decoded_buffer);
    }

#if APP_DIAG_MP3_PERFORMANCE
    portENTER_CRITICAL(&g_mp3_perf_snapshot_mux);
    g_mp3_perf_snapshot.active = false;
    ++g_mp3_perf_snapshot.sequence;
    portEXIT_CRITICAL(&g_mp3_perf_snapshot_mux);
#endif

    *decoder = {};
}

bool mp3_decoder_is_open(const Mp3Decoder *decoder)
{
    return decoder != nullptr && audio_source_is_open(decoder->source) && decoder->simple_handle != nullptr;
}

bool mp3_decoder_is_eof(const Mp3Decoder *decoder)
{
    return decoder != nullptr && decoder->eof && decoder->decoded_offset >= decoder->decoded_size;
}
