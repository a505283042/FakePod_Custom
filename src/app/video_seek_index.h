#pragma once

#include <stdint.h>

#include "esp_err.h"

namespace VideoMediaIo
{
struct Context;
}

namespace VideoSeekIndex
{

// 侧车索引条目：目标视频帧位置，以及该帧之前累计的音频字节数。
struct Entry {
    uint32_t timestamp_ms;
    uint32_t file_offset;
    uint32_t video_frame_index;
    uint32_t audio_bytes_before;
};

// 加载侧车索引文件。sidecar_path 由 AVI 文件名替换扩展名为 .fvix 得到。
// V2 会校验 AVI 文件长度和抽样 CRC32；同时用源视频总帧数检查索引是否覆盖全片。
// V1 仍可读取，但不具备来源指纹和累计音频字节锚点；不完整索引会被拒绝。
esp_err_t load(const char *sidecar_path, VideoMediaIo::Context *source_io,
    uint32_t source_fps_x100, uint32_t source_total_frames,
    uint32_t source_duration_ms);

// 查找不超过 target_ms 的最近条目。
// 成功返回 ESP_OK，entry 填充为找到的条目；
// 若 target_ms 早于第一个条目，返回第一个条目。
esp_err_t lookup(uint32_t target_ms, Entry *out_entry);

// 返回条目数量。
uint32_t count();

// 返回总时长（最后一个条目的时间戳）。
uint32_t duration_ms();

// 当前索引是否包含精确的视频帧序号和音频累计字节数。
bool has_audio_byte_anchor();

// 当前索引是否已经通过原 AVI 文件的长度和抽样指纹校验。
bool has_source_fingerprint();

// 释放索引。
void unload();

} // namespace VideoSeekIndex
