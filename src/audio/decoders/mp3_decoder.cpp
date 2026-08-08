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

// MP3 压缩输入工作区放 PSRAM。320kbps/44.1k 实测 8KB 窗口平均约每 6 个 MP3 frame
// 触发一次同步 fread，而 refill 峰值仍只有约 11.2ms / 26.1ms 预算。Stage 9.5.3
// 只把窗口小步放大到 12KB，降低 FAT/SD 调用和 compact 频率；不引入额外预取任务。
static constexpr size_t MP3_INPUT_BUFFER_BYTES = 12 * 1024;
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

    uint8_t *new_buffer = mp3_alloc_buffer(requested);
    if (new_buffer == nullptr) {
        ESP_LOGE(TAG, "MP3 PCM 输出缓冲分配失败：%u字节", static_cast<unsigned>(requested));
        return ESP_ERR_NO_MEM;
    }
    mp3_free_buffer(decoder->decoded_buffer);
    decoder->decoded_buffer = new_buffer;
    decoder->decoded_capacity = requested;
    decoder->decoded_offset = 0;
    decoder->decoded_size = 0;
    ESP_LOGI(TAG, "MP3 PCM 输出缓冲调整为 %u 字节", static_cast<unsigned>(requested));
    return ESP_OK;
}

static esp_err_t mp3_fill_input(Mp3Decoder *decoder, bool *out_read)
{
    if (out_read != nullptr) {
        *out_read = false;
    }
    if (decoder == nullptr || decoder->file == nullptr || decoder->input_buffer == nullptr) {
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

    // 当前窗口仍有足够压缩数据时直接交给 parser，减少 AudioTask 进入同步 SD fread 的频率。
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
    const size_t read_bytes = fread(
        decoder->input_buffer + decoder->input_size,
        1,
        free_bytes,
        decoder->file
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

    if (read_bytes < free_bytes) {
        if (ferror(decoder->file)) {
            ESP_LOGE(TAG, "读取 MP3 压缩数据失败");
            return ESP_FAIL;
        }
        if (feof(decoder->file)) {
            decoder->input_chunk_eos = true;
        }
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

    ESP_LOGI(TAG, "乐鑫 MP3 解码输出：%luHz / %ubit / %u声道，bitrate=%lu",
        static_cast<unsigned long>(info.sample_rate),
        static_cast<unsigned>(info.bits_per_sample),
        static_cast<unsigned>(info.channel),
        static_cast<unsigned long>(info.bitrate));

    if ((info.sample_rate != 44100U && info.sample_rate != 48000U) ||
        !audio_rate_profile_get(info.sample_rate, nullptr)) {
        ESP_LOGE(TAG, "Stage 9.5 暂不支持该 MP3 采样率：%luHz（当前仅44.1/48kHz）",
            static_cast<unsigned long>(info.sample_rate));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (info.channel != 1U && info.channel != 2U) {
        ESP_LOGE(TAG, "Stage 9.5 暂不支持该 MP3 声道数：%u", static_cast<unsigned>(info.channel));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (info.bits_per_sample != 16U) {
        ESP_LOGE(TAG, "Stage 9.5 暂不支持该 MP3 PCM 位深：%u", static_cast<unsigned>(info.bits_per_sample));
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
    ESP_LOGI(TAG, "乐鑫 MP3 解码后端注册成功");
    return ESP_OK;
}

esp_err_t mp3_decoder_open(Mp3Decoder *decoder, const char *path)
{
    if (decoder == nullptr || path == nullptr || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    mp3_decoder_close(decoder);

    esp_err_t ret = mp3_decoder_register_backend();
    if (ret != ESP_OK) {
        return ret;
    }

    decoder->file = fopen(path, "rb");
    if (decoder->file == nullptr) {
        ESP_LOGE(TAG, "打开 MP3 失败：%s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(decoder->file, 0, SEEK_END) != 0) {
        mp3_decoder_close(decoder);
        return ESP_FAIL;
    }
    const long file_size = ftell(decoder->file);
    if (file_size <= 0 || fseek(decoder->file, 0, SEEK_SET) != 0) {
        mp3_decoder_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }
    decoder->file_size_bytes = static_cast<uint64_t>(file_size);

    decoder->input_buffer = mp3_alloc_buffer(MP3_INPUT_BUFFER_BYTES);
    decoder->decoded_buffer = mp3_alloc_buffer(MP3_DECODED_BUFFER_BYTES);
    if (decoder->input_buffer == nullptr || decoder->decoded_buffer == nullptr) {
        ESP_LOGE(TAG, "MP3 流缓冲分配失败：输入=%u 输出=%u",
            static_cast<unsigned>(MP3_INPUT_BUFFER_BYTES),
            static_cast<unsigned>(MP3_DECODED_BUFFER_BYTES));
        mp3_decoder_close(decoder);
        return ESP_ERR_NO_MEM;
    }
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

    ESP_LOGI(TAG, "MP3流式解码已就绪：文件=%lluB，输入缓冲=%uB，PCM缓冲=%uB，首块PCM=%uB，工作区使用PSRAM",
        static_cast<unsigned long long>(decoder->file_size_bytes),
        static_cast<unsigned>(decoder->input_capacity),
        static_cast<unsigned>(decoder->decoded_capacity),
        static_cast<unsigned>(decoder->decoded_size));
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
    if (decoder->file != nullptr) {
        fclose(decoder->file);
    }
    mp3_free_buffer(decoder->input_buffer);
    mp3_free_buffer(decoder->decoded_buffer);

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
    return decoder != nullptr && decoder->file != nullptr && decoder->simple_handle != nullptr;
}

bool mp3_decoder_is_eof(const Mp3Decoder *decoder)
{
    return decoder != nullptr && decoder->eof && decoder->decoded_offset >= decoder->decoded_size;
}
