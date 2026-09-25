#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "../audio_decode_workspace.h"
#include "../sources/audio_source.h"

// Ogg Opus 解码器负责：Ogg packet 拆包 -> 跳过 OpusHead/OpusTags -> RAW Opus -> PCM。
// Ogg Opus 的播放时钟固定为 48kHz；本轮不实现随机 Seek。
struct OpusDecoder
{
    AudioSource *source = nullptr;
    AudioDecodeWorkspace *workspace = nullptr;
    void *opus_handle = nullptr;

    // 当前 Ogg page 的 lacing 状态。解码热路径只顺序读取，不依赖 Seek。
    uint32_t ogg_serial = 0U;
    uint8_t ogg_lacing[255] = {};
    uint8_t ogg_segment_count = 0U;
    uint8_t ogg_segment_index = 0U;
    uint16_t ogg_segment_remaining = 0U;
    bool ogg_serial_valid = false;
    bool ogg_segment_ends_packet = false;
    bool ogg_packet_active = false;
    bool ogg_packet_ended = false;

    // input_buffer 只保存一个真正的 Opus 音频 packet；巨大的 OpusTags 会流式跳过。
    uint8_t *input_buffer = nullptr;
    size_t input_capacity = 0U;
    size_t input_size = 0U;
    // 首包会在打开解码器前预读，用 TOC 决定乐鑫 Opus decoder 的固定帧时长。
    bool input_packet_ready = false;
    // 单位为 1/4 ms：20ms = 80。避免在公共头文件暴露乐鑫私有枚举类型。
    uint16_t frame_duration_q4_ms = 0U;

    uint8_t *decoded_buffer = nullptr;
    size_t decoded_capacity = 0U;
    size_t decoded_offset = 0U;
    size_t decoded_size = 0U;

    uint32_t sample_rate_hz = 0U;
    uint16_t channels = 0U;
    uint16_t bits_per_sample = 0U;
    uint32_t bitrate = 0U;
    uint16_t pre_skip = 0U;
    uint32_t pre_skip_remaining = 0U;
    uint64_t total_frames = 0ULL;
    uint64_t frames_read = 0ULL;

    bool runtime_info_verified = false;
    bool eof = false;
};

esp_err_t opus_decoder_register_backend();

esp_err_t opus_decoder_open(
    OpusDecoder *decoder,
    AudioSource *source,
    AudioDecodeWorkspace *workspace = nullptr
);

esp_err_t opus_decoder_read_pcm32(
    OpusDecoder *decoder,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames
);

// Catalog 已按 RFC 7845 的 final granule - pre-skip 算出可播放 PCM 总帧。
// 运行时只接收这个现成结果，不重新扫描文件尾；用于精确裁掉最后一个 packet 的 padding。
esp_err_t opus_decoder_set_total_frames_hint(OpusDecoder *decoder, uint64_t total_frames);

void opus_decoder_close(OpusDecoder *decoder);
bool opus_decoder_is_open(const OpusDecoder *decoder);
bool opus_decoder_is_eof(const OpusDecoder *decoder);
