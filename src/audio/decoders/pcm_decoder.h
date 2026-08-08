#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "flac_decoder.h"
#include "mp3_decoder.h"
#include "wav_decoder.h"
#include "../audio_decode_workspace.h"
#include "../sources/audio_source.h"
#include "../sources/sd_file_audio_source.h"
#include "media_types.h"

enum class PcmDecoderType : uint8_t
{
    None = 0,
    Wav,
    Flac,
    Mp3
};


enum class PcmSeekMethod : uint8_t
{
    None = 0,
    WavExact,
    Mp3XingToc,
    Mp3Vbri,
    Mp3CbrLinear,
    Mp3VbrLinearFallback,
    FlacSeektable,
    RestartFromBeginning,
};

struct PcmSeekResult
{
    uint64_t requested_frame = 0;
    uint64_t actual_frame = 0;
    uint64_t source_offset = 0;
    PcmSeekMethod method = PcmSeekMethod::None;
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
    // Source 生命周期归统一 PCM Decoder 所有；各 codec 只借用 AudioSource*。
    AudioSource source = {};
    SdFileAudioSource sd_file_source = {};

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
// 在 I2S/DAC 尚未重新启动前执行 codec 定位。WAV 为帧精确；MP3 使用 Xing/VBRI/线性估算后做 MPEG 帧重同步；
// FLAC 在存在有效 SEEKTABLE 时用 seekpoint 粗定位，再通过 PCM discard 精确落到目标帧。
esp_err_t pcm_decoder_seek_frame(
    PcmDecoder *decoder,
    uint64_t target_frame,
    const MediaTechnicalInfo *technical_info,
    PcmSeekResult *out_result
);

// 查询当前已打开实例是否能执行目标 Seek。FLAC 非零 Seek 需要当前文件存在有效 SEEKTABLE。
bool pcm_decoder_seek_supported(const PcmDecoder *decoder, uint64_t target_frame);
bool pcm_decoder_is_eof(const PcmDecoder *decoder);
uint64_t pcm_decoder_position_frames(const PcmDecoder *decoder);
const char *pcm_decoder_type_name(PcmDecoderType type);
const char *pcm_seek_method_name(PcmSeekMethod method);
