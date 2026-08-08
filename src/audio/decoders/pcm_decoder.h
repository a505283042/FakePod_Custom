#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "flac_decoder.h"
#include "mp3_decoder.h"
#include "wav_decoder.h"
#include "../audio_decode_workspace.h"

enum class PcmDecoderType : uint8_t
{
    None = 0,
    Wav,
    Flac,
    Mp3
};

struct PcmDecoderInfo
{
    uint32_t sample_rate_hz = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint64_t total_frames = 0;
};

// AudioTask 唯一持有的统一解码器对象。WAV/FLAC/MP3 后端都只产出 PCM，
// AudioTask -> I2S -> CS43131 的播放链路不因源格式复制。
struct PcmDecoder
{
    PcmDecoderType type = PcmDecoderType::None;
    PcmDecoderInfo info = {};
    WavDecoder wav = {};
    FlacDecoder flac = {};
    Mp3Decoder mp3 = {};
};

esp_err_t pcm_decoder_register_backends();
esp_err_t pcm_decoder_open(
    PcmDecoder *decoder,
    PcmDecoderType type,
    const char *path,
    AudioDecodeWorkspace *workspace = nullptr
);
esp_err_t pcm_decoder_read_pcm32(
    PcmDecoder *decoder,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames
);
void pcm_decoder_close(PcmDecoder *decoder);
bool pcm_decoder_is_open(const PcmDecoder *decoder);
bool pcm_decoder_is_eof(const PcmDecoder *decoder);
uint64_t pcm_decoder_position_frames(const PcmDecoder *decoder);
const char *pcm_decoder_type_name(PcmDecoderType type);
