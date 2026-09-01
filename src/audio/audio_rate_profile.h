#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t sample_rate_hz;
    uint8_t cs43131_asp_sprate;
    uint8_t i2s_dma_desc_num;
    bool flac_stage_enabled;
} AudioRateProfile;

// 统一维护 FakePod PCM 硬件采样率档位，避免 DAC、I2S 和解码器各自散落判断。
// PCM 硬件仍保留 192kHz 档位，供未来其它格式/专项测试使用。
// P1.3.5.4.2 起，192kHz FLAC 因与 460x460 UI 同时运行时实时余量不足而正式禁用；
// 44.1/48/88.2/96/176.4kHz FLAC 保持现有路径。稳定播放优先于极限规格。
static inline bool audio_rate_profile_get(uint32_t sample_rate_hz, AudioRateProfile *out_profile)
{
    // 显式初始化全部字段，避免 C++ 编译器把 {0} 视为仅初始化首字段并产生告警。
    AudioRateProfile profile;
    profile.sample_rate_hz = 0U;
    profile.cs43131_asp_sprate = 0U;
    profile.i2s_dma_desc_num = 0U;
    profile.flac_stage_enabled = false;
    switch (sample_rate_hz) {
        case 44100U:
            profile.sample_rate_hz = 44100U;
            profile.cs43131_asp_sprate = 0x01U;
            // 48kHz 档加大 DMA 余量（16×256≈85ms）：电子音流 NSF 渲染与
            // 瀑布 SPI 刷屏争抢总线时，避免瞬时下溢导致音频卡顿。
            profile.i2s_dma_desc_num = 16U;
            profile.flac_stage_enabled = true;
            break;
        case 48000U:
            profile.sample_rate_hz = 48000U;
            profile.cs43131_asp_sprate = 0x02U;
            profile.i2s_dma_desc_num = 16U;
            profile.flac_stage_enabled = true;
            break;
        case 88200U:
            profile.sample_rate_hz = 88200U;
            profile.cs43131_asp_sprate = 0x03U;
            profile.i2s_dma_desc_num = 12U;
            profile.flac_stage_enabled = true;
            break;
        case 96000U:
            profile.sample_rate_hz = 96000U;
            profile.cs43131_asp_sprate = 0x04U;
            profile.i2s_dma_desc_num = 12U;
            profile.flac_stage_enabled = true;
            break;
        case 176400U:
            profile.sample_rate_hz = 176400U;
            profile.cs43131_asp_sprate = 0x05U;
            profile.i2s_dma_desc_num = 18U;
            profile.flac_stage_enabled = true;
            break;
        case 192000U:
            profile.sample_rate_hz = 192000U;
            profile.cs43131_asp_sprate = 0x06U;
            profile.i2s_dma_desc_num = 18U;
            // P1.3.5.4.2：正式关闭 192kHz FLAC。硬件档位仍保留，
            // 但 FLAC decoder 的统一能力门禁会在打开 pipeline 前拒绝该采样率。
            profile.flac_stage_enabled = false;
            break;
        default:
            return false;
    }

    if (out_profile != 0) {
        *out_profile = profile;
    }
    return true;
}

static inline bool audio_rate_profile_flac_enabled(uint32_t sample_rate_hz)
{
    AudioRateProfile profile;
    profile.sample_rate_hz = 0U;
    profile.cs43131_asp_sprate = 0U;
    profile.i2s_dma_desc_num = 0U;
    profile.flac_stage_enabled = false;
    return audio_rate_profile_get(sample_rate_hz, &profile) && profile.flac_stage_enabled;
}
