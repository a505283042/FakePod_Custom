#include "pcm_decoder.h"

#include "esp_log.h"
#include "app_diag_config.h"

static const char *TAG = "PCM解码";

esp_err_t pcm_decoder_register_backends()
{
    // WAV 是本地无依赖解析器。FLAC 后端启动失败时不拖垮 WAV，
    // 真正选择 FLAC 时 open() 会再次尝试注册并返回明确错误。
    const esp_err_t flac_ret = flac_decoder_register_backend();
    if (flac_ret != ESP_OK) {
        ESP_LOGW(TAG, "FLAC 后端暂未就绪：%s；其他格式仍可继续", esp_err_to_name(flac_ret));
    }
    const esp_err_t mp3_ret = mp3_decoder_register_backend();
    if (mp3_ret != ESP_OK) {
        ESP_LOGW(TAG, "MP3 后端暂未就绪：%s；其他格式仍可继续", esp_err_to_name(mp3_ret));
    }
    return ESP_OK;
}

esp_err_t pcm_decoder_open(
    PcmDecoder *decoder,
    PcmDecoderType type,
    const char *path,
    AudioDecodeWorkspace *workspace)
{
    if (decoder == nullptr || path == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    pcm_decoder_close(decoder);

    // Stage 10.9 第一版只有本地 SD Source。Codec 不再 fopen(path)，只消费统一 AudioSource。
    esp_err_t ret = sd_file_audio_source_open(&decoder->source, &decoder->sd_file_source, path);
    if (ret != ESP_OK) {
        return ret;
    }

    switch (type) {
        case PcmDecoderType::Wav:
            ret = wav_decoder_open(&decoder->wav, &decoder->source);
            if (ret == ESP_OK) {
                decoder->info.sample_rate_hz = decoder->wav.sample_rate_hz;
                decoder->info.channels = decoder->wav.channels;
                decoder->info.bits_per_sample = decoder->wav.bits_per_sample;
                decoder->info.total_frames = decoder->wav.total_frames;
            }
            break;
        case PcmDecoderType::Flac:
            ret = flac_decoder_open(&decoder->flac, &decoder->source, workspace);
            if (ret == ESP_OK) {
                decoder->info.sample_rate_hz = decoder->flac.sample_rate_hz;
                decoder->info.channels = decoder->flac.channels;
                decoder->info.bits_per_sample = decoder->flac.bits_per_sample;
                decoder->info.total_frames = decoder->flac.total_frames;
            }
            break;
        case PcmDecoderType::Mp3:
            ret = mp3_decoder_open(&decoder->mp3, &decoder->source, workspace);
            if (ret == ESP_OK) {
                decoder->info.sample_rate_hz = decoder->mp3.sample_rate_hz;
                decoder->info.channels = decoder->mp3.channels;
                decoder->info.bits_per_sample = decoder->mp3.bits_per_sample;
                decoder->info.total_frames = decoder->mp3.total_frames;
            }
            break;
        default:
            audio_source_close(&decoder->source);
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (ret != ESP_OK) {
        pcm_decoder_close(decoder);
        return ret;
    }

    decoder->type = type;
#if APP_DIAG_AUDIO_CODEC
    ESP_LOGI(TAG, "统一PCM解码器已打开：格式=%s %luHz/%ubit/%u声道，总帧=%llu",
        pcm_decoder_type_name(type),
        static_cast<unsigned long>(decoder->info.sample_rate_hz),
        static_cast<unsigned>(decoder->info.bits_per_sample),
        static_cast<unsigned>(decoder->info.channels),
        static_cast<unsigned long long>(decoder->info.total_frames));
#endif
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
        case PcmDecoderType::Mp3:
            return mp3_decoder_read_pcm32(&decoder->mp3, out_interleaved_stereo, max_frames, out_frames);
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
    if (mp3_decoder_is_open(&decoder->mp3)) {
        mp3_decoder_close(&decoder->mp3);
    }

    // FLAC close 会先停止 Core1 PrefetchTask；确认所有 Codec 都不再访问 Source 后，最后关闭底层文件。
    if (audio_source_is_open(&decoder->source)) {
#if APP_DIAG_AUDIO_SOURCE
        const AudioSourceStats *stats = audio_source_stats(&decoder->source);
        ESP_LOGI(TAG,
            "SOURCE_TRACE: CLOSE type=%s reads=%lu bytes=%llu seeks=%lu tells=%lu",
            audio_source_name(&decoder->source),
            static_cast<unsigned long>(stats != nullptr ? stats->read_calls : 0U),
            static_cast<unsigned long long>(stats != nullptr ? stats->bytes_read : 0ULL),
            static_cast<unsigned long>(stats != nullptr ? stats->seek_calls : 0U),
            static_cast<unsigned long>(stats != nullptr ? stats->tell_calls : 0U));
#endif
        audio_source_close(&decoder->source);
    }
    decoder->sd_file_source = {};
    decoder->type = PcmDecoderType::None;
    decoder->info = {};
}

esp_err_t pcm_decoder_seek_frame(
    PcmDecoder *decoder,
    uint64_t target_frame,
    const MediaTechnicalInfo *technical_info,
    PcmSeekResult *out_result)
{
    if (out_result != nullptr) {
        *out_result = {};
        out_result->requested_frame = target_frame;
    }
    if (decoder == nullptr || !pcm_decoder_is_open(decoder)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t actual_frame = 0;
    uint64_t source_offset = 0;
    PcmSeekMethod method = PcmSeekMethod::None;
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;

    switch (decoder->type) {
        case PcmDecoderType::Wav:
            ret = wav_decoder_seek_frame(&decoder->wav, target_frame, &actual_frame);
            method = PcmSeekMethod::WavExact;
            if (ret == ESP_OK) {
                source_offset = decoder->wav.data_offset_bytes +
                    actual_frame * decoder->wav.block_align;
            }
            break;

        case PcmDecoderType::Mp3: {
            Mp3SeekMethod mp3_method = Mp3SeekMethod::None;
            ret = mp3_decoder_seek_frame(
                &decoder->mp3,
                target_frame,
                technical_info,
                &actual_frame,
                &source_offset,
                &mp3_method);
            if (ret == ESP_OK) {
                switch (mp3_method) {
                    case Mp3SeekMethod::XingToc: method = PcmSeekMethod::Mp3XingToc; break;
                    case Mp3SeekMethod::Vbri: method = PcmSeekMethod::Mp3Vbri; break;
                    case Mp3SeekMethod::CbrLinear: method = PcmSeekMethod::Mp3CbrLinear; break;
                    case Mp3SeekMethod::VbrLinearFallback: method = PcmSeekMethod::Mp3VbrLinearFallback; break;
                    default: method = PcmSeekMethod::None; break;
                }
            }
            break;
        }

        case PcmDecoderType::Flac:
            ret = flac_decoder_seek_frame(
                &decoder->flac,
                target_frame,
                &actual_frame,
                &source_offset);
            if (ret == ESP_OK) {
                decoder->info.sample_rate_hz = decoder->flac.sample_rate_hz;
                decoder->info.channels = decoder->flac.channels;
                decoder->info.bits_per_sample = decoder->flac.bits_per_sample;
                decoder->info.total_frames = decoder->flac.total_frames;
                method = target_frame == 0
                    ? PcmSeekMethod::RestartFromBeginning
                    : PcmSeekMethod::FlacSeektable;
            }
            break;

        default:
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
    }

    if (ret == ESP_OK && out_result != nullptr) {
        out_result->actual_frame = actual_frame;
        out_result->source_offset = source_offset;
        out_result->method = method;
    }
    return ret;
}

bool pcm_decoder_seek_supported(const PcmDecoder *decoder, uint64_t target_frame)
{
    if (decoder == nullptr || !pcm_decoder_is_open(decoder)) {
        return false;
    }
    switch (decoder->type) {
        case PcmDecoderType::Wav:
        case PcmDecoderType::Mp3:
            return true;
        case PcmDecoderType::Flac:
            return target_frame == 0 || flac_decoder_has_seektable(&decoder->flac);
        default:
            return false;
    }
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
        case PcmDecoderType::Mp3:
            return mp3_decoder_is_open(&decoder->mp3);
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
        case PcmDecoderType::Mp3:
            return mp3_decoder_is_eof(&decoder->mp3);
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
        case PcmDecoderType::Mp3:
            return decoder->mp3.frames_read;
        default:
            return 0;
    }
}

const char *pcm_decoder_type_name(PcmDecoderType type)
{
    switch (type) {
        case PcmDecoderType::Wav: return "WAV";
        case PcmDecoderType::Flac: return "FLAC";
        case PcmDecoderType::Mp3: return "MP3";
        default: return "NONE";
    }
}

const char *pcm_seek_method_name(PcmSeekMethod method)
{
    switch (method) {
        case PcmSeekMethod::WavExact: return "WAV_EXACT";
        case PcmSeekMethod::Mp3XingToc: return "MP3_XING_TOC";
        case PcmSeekMethod::Mp3Vbri: return "MP3_VBRI";
        case PcmSeekMethod::Mp3CbrLinear: return "MP3_CBR_LINEAR";
        case PcmSeekMethod::Mp3VbrLinearFallback: return "MP3_VBR_LINEAR";
        case PcmSeekMethod::FlacSeektable: return "FLAC_SEEKTABLE";
        case PcmSeekMethod::RestartFromBeginning: return "RESTART_BEGIN";
        default: return "NONE";
    }
}
