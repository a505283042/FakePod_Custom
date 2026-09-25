#pragma once

#include <stdint.h>

// 音乐库与音频服务共享的稳定媒体类型。放在独立头文件中，避免 Catalog/Audio 反向依赖扫描实现。
enum class MediaFormat : uint8_t
{
    Unknown,
    MP3,
    FLAC,
    WAV,
    NSF,
    NSFE,
    // 追加到末尾以保持旧 Catalog 中既有格式枚举值稳定。
    OPUS
};

// 媒体格式名称属于稳定媒体类型层，供 Catalog / Probe / Audio / UI 共用。
// 保持为 inline，避免为了日志字符串重新依赖完整 media_library 扫描模块。
inline const char *media_format_name(MediaFormat format)
{
    switch (format) {
        case MediaFormat::Unknown: return "未知";
        case MediaFormat::MP3: return "MP3";
        case MediaFormat::FLAC: return "FLAC";
        case MediaFormat::WAV: return "WAV";
        case MediaFormat::NSF: return "NSF";
        case MediaFormat::NSFE: return "NSFE";
        case MediaFormat::OPUS: return "OPUS";
    }
    return "未知";
}

// 持久化索引中的技术信息标志。
// Stage 12.0 统一封面来源/格式。Catalog 只保存 locator，图片正文始终留在原文件。
enum class MediaArtworkSourceV2 : uint8_t
{
    None = 0,
    Mp3Apic = 1,
    FlacPicture = 2,
    ExternalFile = 3,
};

enum class MediaArtworkFormatV2 : uint8_t
{
    Unknown = 0,
    Jpeg = 1,
    Png = 2,
};

enum MediaArtworkRefFlagsV2 : uint32_t
{
    MEDIA_ARTWORK_REF_NONE_V2 = 0,
    // ID3 unsynchronisation 会在原始图片字节中插入 0x00；12.1 读取时必须反转义。
    MEDIA_ARTWORK_REF_NEEDS_ID3_UNSYNC_V2 = 1U << 0,
};

enum MediaTechnicalFlags : uint32_t
{
    MEDIA_TECH_NONE = 0,
    MEDIA_TECH_PARSED = 1U << 0,
    MEDIA_TECH_HAS_ARTWORK = 1U << 1,
    MEDIA_TECH_HAS_VBR_HEADER = 1U << 2,
    MEDIA_TECH_DURATION_ESTIMATED = 1U << 3,
};

// 扫描阶段解析并持久化的播放技术参数。
// audio_data_offset 指向真正音频帧起点；metadata_end_offset 标记文件头元数据结束位置。
struct MediaTechnicalInfo
{
    uint32_t flags = MEDIA_TECH_NONE;
    uint32_t sample_rate_hz = 0;
    uint32_t bitrate_kbps = 0;
    uint32_t duration_ms = 0;
    uint64_t total_frames = 0;
    uint64_t audio_data_offset = 0;
    uint64_t metadata_end_offset = 0;
    uint64_t artwork_offset = 0;
    uint32_t artwork_size = 0;
    uint32_t max_frame_size = 0;
    uint16_t max_block_size = 0;
    uint16_t samples_per_frame = 0;
    uint8_t channels = 0;
    uint8_t bits_per_sample = 0;
};
