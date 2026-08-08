#include "pcm_decoder.h"

#include "esp_log.h"

static const char *TAG = "PCM解码";

esp_err_t pcm_decoder_register_backends()
{
    // WAV 是本地无依赖解析器。FLAC 后端启动失败时不拖垮 WAV，
    // 真正选择 FLAC 时 open() 会再次尝试注册并返回明确错误。
    const esp_err_t flac_ret = flac_decoder_register_backend();
    if (flac_ret != ESP_OK) {
        ESP_LOGW(TAG, "FLAC 后端暂未就绪：%s；WAV 播放仍可继续", esp_err_to_name(flac_ret));
    }
    return ESP_OK;
}

esp_err_t pcm_decoder_open(PcmDecoder *decoder, PcmDecoderType type, const char *path)
{
    if (decoder == nullptr || path == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    pcm_decoder_close(decoder);

    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    switch (type) {
        case PcmDecoderType::Wav:
            ret = wav_decoder_open(&decoder->wav, path);
            if (ret == ESP_OK) {
                decoder->info.sample_rate_hz = decoder->wav.sample_rate_hz;
                decoder->info.channels = decoder->wav.channels;
                decoder->info.bits_per_sample = decoder->wav.bits_per_sample;
                decoder->info.total_frames = decoder->wav.total_frames;
            }
            break;
        case PcmDecoderType::Flac:
            ret = flac_decoder_open(&decoder->flac, path);
            if (ret == ESP_OK) {
                decoder->info.sample_rate_hz = decoder->flac.sample_rate_hz;
                decoder->info.channels = decoder->flac.channels;
                decoder->info.bits_per_sample = decoder->flac.bits_per_sample;
                decoder->info.total_frames = decoder->flac.total_frames;
            }
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (ret != ESP_OK) {
        pcm_decoder_close(decoder);
        return ret;
    }

    decoder->type = type;
    ESP_LOGI(TAG, "统一PCM解码器已打开：格式=%s %luHz/%ubit/%u声道，总帧=%llu",
        pcm_decoder_type_name(type),
        static_cast<unsigned long>(decoder->info.sample_rate_hz),
        static_cast<unsigned>(decoder->info.bits_per_sample),
        static_cast<unsigned>(decoder->info.channels),
        static_cast<unsigned long long>(decoder->info.total_frames));
    return ESP_OK;
}

esp_err_t pcm_decoder_read_pcm32(
    PcmDecoder *decoder,
    int32_t *out_interleaved_stereo,
    size_t max_frames,
    size_t *out_frames)
{
    if (decoder == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (decoder->type) {
        case PcmDecoderType::Wav:
            return wav_decoder_read_pcm32(&decoder->wav, out_interleaved_stereo, max_frames, out_frames);
        case PcmDecoderType::Flac:
            return flac_decoder_read_pcm32(&decoder->flac, out_interleaved_stereo, max_frames, out_frames);
        default:
            return ESP_ERR_INVALID_STATE;
    }
}

void pcm_decoder_close(PcmDecoder *decoder)
{
    if (decoder == nullptr) {
        return;
    }
    if (wav_decoder_is_open(&decoder->wav)) {
        wav_decoder_close(&decoder->wav);
    }
    if (flac_decoder_is_open(&decoder->flac)) {
        flac_decoder_close(&decoder->flac);
    }
    decoder->type = PcmDecoderType::None;
    decoder->info = {};
}

bool pcm_decoder_is_open(const PcmDecoder *decoder)
{
    if (decoder == nullptr) {
        return false;
    }
    switch (decoder->type) {
        case PcmDecoderType::Wav:
            return wav_decoder_is_open(&decoder->wav);
        case PcmDecoderType::Flac:
            return flac_decoder_is_open(&decoder->flac);
        default:
            return false;
    }
}

bool pcm_decoder_is_eof(const PcmDecoder *decoder)
{
    if (decoder == nullptr) {
        return false;
    }
    switch (decoder->type) {
        case PcmDecoderType::Wav:
            return wav_decoder_is_eof(&decoder->wav);
        case PcmDecoderType::Flac:
            return flac_decoder_is_eof(&decoder->flac);
        default:
            return false;
    }
}

uint64_t pcm_decoder_position_frames(const PcmDecoder *decoder)
{
    if (decoder == nullptr) {
        return 0;
    }
    switch (decoder->type) {
        case PcmDecoderType::Wav:
            return decoder->wav.frames_read;
        case PcmDecoderType::Flac:
            return decoder->flac.frames_read;
        default:
            return 0;
    }
}

const char *pcm_decoder_type_name(PcmDecoderType type)
{
    switch (type) {
        case PcmDecoderType::Wav: return "WAV";
        case PcmDecoderType::Flac: return "FLAC";
        default: return "NONE";
    }
}
