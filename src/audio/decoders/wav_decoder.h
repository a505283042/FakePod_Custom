#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "../sources/audio_source.h"

// Stage 9.2 第一版只接入最常见的未压缩 PCM WAV：
// 16bit、双声道、44.1kHz 或 48kHz。
struct WavDecoder
{
    AudioSource *source = nullptr;
    uint32_t sample_rate_hz = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint16_t block_align = 0;
    uint32_t byte_rate = 0;
    uint32_t data_size_bytes = 0;
    uint32_t data_remaining_bytes = 0;
    uint64_t total_frames = 0;
    uint64_t frames_read = 0;
};

// 从统一 AudioSource 打开并解析 RIFF/WAVE、fmt 和 data chunk。
// 不支持压缩 WAV、单声道、24/32bit PCM 或 WAVE_FORMAT_EXTENSIBLE。
esp_err_t wav_decoder_open(WavDecoder *decoder, AudioSource *source);

// 将 16bit little-endian 立体声 PCM 转为 32bit I2S 容器。
// 每个 16bit 样本左移到 32bit 高 16 位，保持原始幅度关系。
esp_err_t wav_decoder_read_pcm32(
    WavDecoder *decoder,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames
);

void wav_decoder_close(WavDecoder *decoder);
bool wav_decoder_is_open(const WavDecoder *decoder);
bool wav_decoder_is_eof(const WavDecoder *decoder);
