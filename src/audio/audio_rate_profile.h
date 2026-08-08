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
// 176.4/192kHz 已通过首轮实时验证；192kHz 在 24 个描述符下长期 over_budget=0，
// Stage 9.4.14 的 20 个描述符已通过约 4 分钟 192kHz 长时间验证：starve=0、over_budget=0，
// Stage 9.4.15 再小步回收为 18 个描述符，192kHz 仍保留约 24ms DMA runway；
// 本轮只回收内部 DMA RAM，不改变解码、预取和 DAC 参数。
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
            profile.i2s_dma_desc_num = 8U;
            profile.flac_stage_enabled = true;
            break;
        case 48000U:
            profile.sample_rate_hz = 48000U;
            profile.cs43131_asp_sprate = 0x02U;
            profile.i2s_dma_desc_num = 8U;
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
            profile.flac_stage_enabled = true;
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
