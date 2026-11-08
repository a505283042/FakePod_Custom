#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "video_media_io.h"

namespace AviDemuxer
{

// esp_err.h 未提供 ESP_ERR_EOF，这里定义专用错误码表示“已到达 movi 末尾”。
constexpr esp_err_t kErrEof = static_cast<esp_err_t>(0x2001);
// 当前 AVI chunk 是视频帧，但调用者暂时没有空闲压缩槽位；帧保持原位，等待槽位后重试。
constexpr esp_err_t kErrNeedVideoSlot = static_cast<esp_err_t>(0x2002);

struct IoStats {
    uint32_t fseek_calls = 0U;
    uint32_t sequential_seek_skips = 0U;
    uint64_t fseek_us_total = 0ULL;
    uint32_t fseek_us_max = 0U;
    uint32_t forward_reads = 0U;
    uint64_t forward_bytes = 0ULL;
    uint32_t audio_chunks = 0U;
    uint64_t audio_bytes = 0ULL;
    uint32_t seek_scan_chunks = 0U;          // 最近一次 SEEK 扫描检查的 chunk 数
    uint64_t seek_scan_skipped_bytes = 0ULL; // 最近一次 SEEK 顺序读过的 payload 字节数
    uint64_t seek_scan_duration_us = 0ULL;   // 最近一次 SEEK 精确扫描耗时
    uint32_t seek_scan_fseek_calls = 0U;     // 最近一次 SEEK 扫描期间的随机定位次数
};

// 输出帧信息，与 esp_extractor_frame_info_t 字段对齐以便复用现有 pipeline。
struct FrameInfo {
    bool is_video;           // true=视频, false=音频
    uint8_t *data;           // 帧数据指针（指向调用者提供的 buffer）
    uint32_t size;           // 帧数据大小
    uint64_t pts;            // 显示时间戳（毫秒）
    uint32_t file_offset;    // 帧在文件中的偏移（诊断用）
};

// 流信息
struct StreamInfo {
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t fps_x100 = 0;      // fps * 100，例如 2400 表示 24fps
    uint32_t duration_ms = 0;    // 全片时长，优先使用视频流 time base
    uint32_t total_frames = 0;   // 全片视频帧数，优先使用视频流 strh.dwLength

    // 音频
    uint16_t audio_sample_rate = 0;
    uint8_t audio_channels = 0;
    uint8_t audio_bits_per_sample = 0;
    uint32_t audio_bitrate = 0;
    uint32_t audio_avg_bps = 0; // 平均字节/秒，用于音频 PTS 估算
    uint16_t audio_format = 0;  // WAVEFORMATEX wFormatTag (0x55=MP3)
    bool has_audio = false;
};

// 打开并解析 AVI 头部。io 必须已由 VideoMediaIo::open 打开。
// open 会解析 RIFF AVI/AVIX 段、hdrl 元数据，并收集全部 movi 区间。
esp_err_t open(VideoMediaIo::Context *io, StreamInfo *out_info);

// 读取下一个视频帧或音频 chunk，音频和视频使用独立缓冲区。
// audio_data 必须始终有效；其数据在下一次 read_frame() 调用前由调用者消费完毕。
// 当前 chunk 为视频而 video_data 为空时，返回 kErrNeedVideoSlot；不推进文件位置、不丢帧。
// 调用者取得空闲视频槽位后重试，Demuxer 使用缓存的 chunk 头继续读取同一帧。
esp_err_t read_frame(
    FrameInfo *frame,
    uint8_t *video_data, uint32_t video_capacity,
    uint8_t *audio_data, uint32_t audio_capacity);
void get_io_stats(IoStats *out_stats);

// seek 到任一 RIFF/AVIX 段的 movi 文件偏移，并重置连续时间轴计数器。
// 用于侧车索引定位后的跳转。
esp_err_t seek_to_offset(uint32_t file_offset, uint32_t video_pts_ms,
    uint32_t audio_bytes_at_seek, uint32_t video_frame_index = 0xFFFFFFFFU);

// 从当前偏移顺序扫描到目标视频帧。跳过的音视频 payload 使用调用者提供的 scratch 顺序读过，
// 避免每个 chunk 都执行 FATFS 随机 fseek；scratch 在调用期间必须保持有效。
// 停在目标视频帧并缓存其 chunk 头，后续 read_frame() 可直接消费该帧。
// 目标超出最后一个可解码帧时回退到最后一帧并返回 ESP_OK；无任何视频帧时返回 kErrEof。
esp_err_t scan_forward_to(uint32_t target_ms, uint8_t *scratch, uint32_t scratch_capacity);

// 返回下一个视频帧的时间戳（ms）。seek 之后用于让音频时钟与其严格同轴。
uint32_t next_video_pts_ms();

// 关闭并释放资源。
void close();

} // namespace AviDemuxer
