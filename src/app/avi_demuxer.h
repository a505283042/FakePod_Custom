#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "video_media_io.h"

namespace AviDemuxer
{

// esp_err.h 未提供 ESP_ERR_EOF，这里定义专用错误码表示“已到达 movi 末尾”。
constexpr esp_err_t kErrEof = static_cast<esp_err_t>(0x2001);

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
    uint32_t duration_ms = 0;
    uint32_t total_frames = 0;

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
// parse_header 会读取文件偏移 0 处的 RIFF/hdrl 结构。
esp_err_t open(VideoMediaIo::Context *io, StreamInfo *out_info);

// 读取下一帧。data 指向调用者提供的 buffer，capacity 为 buffer 大小。
// 返回：
//   ESP_OK         - 成功，frame 已填充
//   kErrEof        - 到达 movi 末尾
//   ESP_ERR_NO_MEM - buffer 太小
//   其他           - 读取/解析错误
esp_err_t read_frame(FrameInfo *frame, uint8_t *data, uint32_t capacity);

// seek 到指定文件偏移，并重置帧计数器。
// 用于侧车索引定位后的跳转。
esp_err_t seek_to_offset(uint32_t file_offset, uint32_t video_pts_ms, uint32_t audio_bytes_at_seek);

// 关闭并释放资源。
void close();

} // namespace AviDemuxer
