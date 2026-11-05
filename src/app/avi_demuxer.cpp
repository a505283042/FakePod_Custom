#include "avi_demuxer.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "avi_demuxer";

namespace AviDemuxer
{

namespace
{

VideoMediaIo::Context *s_io = nullptr;
StreamInfo s_info = {};
uint32_t s_movi_data_offset = 0;   // movi LIST 数据区起始偏移（跳过 LIST+size+type 12 字节）
uint32_t s_movi_data_end = 0;      // movi 数据区结束偏移
uint32_t s_current_offset = 0;     // 当前读取位置
uint32_t s_video_frame_index = 0;  // 已读取的视频帧计数（用于 PTS）
uint32_t s_audio_bytes_total = 0;  // 已读取的音频字节数（用于 PTS）

// 小端读取辅助
inline uint16_t read_u16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t read_u32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline bool match_fcc(const uint8_t *p, const char *fcc) {
    return p[0] == fcc[0] && p[1] == fcc[1] && p[2] == fcc[2] && p[3] == fcc[3];
}

// 从文件绝对偏移读取 size 字节到 buf。成功返回 ESP_OK。
esp_err_t read_at(uint32_t offset, void *buf, uint32_t size) {
    if (s_io == nullptr || s_io->file == nullptr) return ESP_ERR_INVALID_STATE;
    if (VideoMediaIo::seek_cb(offset, s_io) < 0) {
        s_io->seek_errors++;
        return ESP_FAIL;
    }
    const int got = VideoMediaIo::read_cb(buf, size, s_io);
    if (got != static_cast<int>(size)) {
        return ESP_FAIL;
    }
    s_current_offset = offset + size;
    return ESP_OK;
}

// 从当前位置读取 size 字节。
esp_err_t read_forward(void *buf, uint32_t size) {
    return read_at(s_current_offset, buf, size);
}

} // namespace

esp_err_t open(VideoMediaIo::Context *io, StreamInfo *out_info) {
    if (io == nullptr || io->file == nullptr || out_info == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    s_io = io;
    s_info = {};
    s_movi_data_offset = 0;
    s_movi_data_end = 0;
    s_current_offset = 0;
    s_video_frame_index = 0;
    s_audio_bytes_total = 0;

    // 读取 RIFF 头 (12 字节)
    uint8_t riff[12];
    if (read_at(0, riff, sizeof(riff)) != ESP_OK) {
        ESP_LOGE(TAG, "读取RIFF头失败");
        return ESP_FAIL;
    }
    if (!match_fcc(riff, "RIFF") || !match_fcc(riff + 8, "AVI ")) {
        ESP_LOGE(TAG, "不是AVI文件");
        return ESP_ERR_NOT_SUPPORTED;
    }

    // 遍历顶层 chunk，找 hdrl 和 movi
    uint32_t pos = 12;
    const uint32_t file_size = io->size;
    bool found_avih = false;
    bool found_video_strl = false;

    while (pos + 8 <= file_size) {
        uint8_t hdr[8];
        if (read_at(pos, hdr, sizeof(hdr)) != ESP_OK) break;
        const uint32_t chunk_size = read_u32(hdr + 4);
        const uint32_t chunk_data_start = pos + 8;

        if (match_fcc(hdr, "LIST")) {
            // LIST 后面 4 字节是类型
            uint8_t list_type[4];
            if (read_at(chunk_data_start, list_type, 4) != ESP_OK) break;

            if (match_fcc(list_type, "hdrl")) {
                // 解析 hdrl：avih + 多个 strl
                uint32_t p = chunk_data_start + 4;
                const uint32_t list_end = chunk_data_start + chunk_size;
                while (p + 8 <= list_end) {
                    uint8_t sub[8];
                    if (read_at(p, sub, sizeof(sub)) != ESP_OK) break;
                    const uint32_t sub_size = read_u32(sub + 4);
                    const uint32_t sub_data = p + 8;

                    if (match_fcc(sub, "avih")) {
                        // 主 AVI 头 (56 字节)
                        uint8_t avih[56];
                        if (sub_size >= 56 && read_at(sub_data, avih, 56) == ESP_OK) {
                            const uint32_t us_per_frame = read_u32(avih + 0);
                            if (us_per_frame > 0) {
                                // fps = 1000000 / us_per_frame, 存为 fps_x100
                                s_info.fps_x100 = static_cast<uint32_t>(100000000ULL / us_per_frame);
                            }
                            s_info.total_frames = read_u32(avih + 16);
                            s_info.duration_ms = s_info.total_frames > 0 && s_info.fps_x100 > 0
                                ? s_info.total_frames * 100000U / s_info.fps_x100
                                : 0;
                            found_avih = true;
                        }
                    } else if (match_fcc(sub, "LIST")) {
                        // strl LIST
                        uint8_t strl_type[4];
                        if (read_at(sub_data, strl_type, 4) != ESP_OK) break;
                        if (match_fcc(strl_type, "strl")) {
                            uint32_t sp = sub_data + 4;
                            const uint32_t strl_end = sub_data + sub_size;
                            bool has_strh = false;
                            uint8_t strh[56] = {};
                            uint8_t strf[256] = {};
                            uint32_t strf_size = 0;
                            while (sp + 8 <= strl_end) {
                                uint8_t sh[8];
                                if (read_at(sp, sh, sizeof(sh)) != ESP_OK) break;
                                const uint32_t sh_size = read_u32(sh + 4);
                                const uint32_t sh_data = sp + 8;
                                if (match_fcc(sh, "strh") && sh_size >= 56) {
                                    if (read_at(sh_data, strh, 56) == ESP_OK) has_strh = true;
                                } else if (match_fcc(sh, "strf")) {
                                    strf_size = sh_size < sizeof(strf) ? sh_size : sizeof(strf);
                                    if (strf_size > 0) read_at(sh_data, strf, strf_size);
                                }
                                sp = sh_data + sh_size + (sh_size & 1); // 奇数补齐
                            }
                            if (has_strh) {
                                const char *stype = reinterpret_cast<const char *>(strh);
                                if (stype[0] == 'v' && stype[1] == 'i' && stype[2] == 'd' && stype[3] == 's') {
                                    // 视频流
                                    if (strf_size >= 40) {
                                        s_info.width = read_u32(strf + 4);
                                        s_info.height = read_u32(strf + 8);
                                    }
                                    found_video_strl = true;
                                } else if (stype[0] == 'a' && stype[1] == 'u' && stype[2] == 'd' && stype[3] == 's') {
                                    // 音频流
                                    if (strf_size >= 18) {
                                        s_info.audio_format = read_u16(strf + 0);
                                        s_info.audio_channels = strf[2];
                                        s_info.audio_sample_rate = read_u32(strf + 4);
                                        s_info.audio_avg_bps = read_u32(strf + 8);
                                        s_info.audio_bits_per_sample = strf[14];
                                        s_info.audio_bitrate = s_info.audio_avg_bps * 8;
                                        s_info.has_audio = true;
                                    }
                                }
                            }
                        }
                    }
                    p = sub_data + sub_size + (sub_size & 1);
                }
            } else if (match_fcc(list_type, "movi")) {
                // movi 数据区：跳过 LIST(4)+size(4)+type(4)=12 字节
                s_movi_data_offset = chunk_data_start + 4;
                s_movi_data_end = chunk_data_start + chunk_size;
                s_current_offset = s_movi_data_offset;
            }
            pos = chunk_data_start + chunk_size + (chunk_size & 1);
        } else {
            // idx1 或其他顶层 chunk，跳过
            pos = chunk_data_start + chunk_size + (chunk_size & 1);
        }
    }

    if (!found_avih || !found_video_strl || s_movi_data_offset == 0) {
        ESP_LOGE(TAG, "AVI头部解析失败: avih=%d video_strl=%d movi=%d",
            found_avih, found_video_strl, s_movi_data_offset);
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out_info = s_info;
    ESP_LOGI(TAG, "AVI解析成功: %ux%u %u.%02ufps dur=%ums frames=%u audio=%s",
        s_info.width, s_info.height,
        s_info.fps_x100 / 100, s_info.fps_x100 % 100,
        s_info.duration_ms, s_info.total_frames,
        s_info.has_audio ? "yes" : "no");
    return ESP_OK;
}

esp_err_t read_frame(FrameInfo *frame, uint8_t *data, uint32_t capacity) {
    if (frame == nullptr || data == nullptr || s_io == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    while (s_current_offset + 8 <= s_movi_data_end) {
        uint8_t hdr[8];
        if (read_at(s_current_offset, hdr, 8) != ESP_OK) {
            return ESP_FAIL;
        }
        const uint32_t chunk_size = read_u32(hdr + 4);

        if (match_fcc(hdr, "00dc") || match_fcc(hdr, "00db")) {
            // 视频帧
            if (chunk_size > capacity) {
                ESP_LOGE(TAG, "视频帧过大: %u > %u", chunk_size, capacity);
                return ESP_ERR_NO_MEM;
            }
            if (read_forward(data, chunk_size) != ESP_OK) return ESP_FAIL;
            frame->is_video = true;
            frame->data = data;
            frame->size = chunk_size;
            frame->file_offset = s_current_offset;
            // PTS = 视频帧序号 * 1000 / fps
            frame->pts = s_info.fps_x100 > 0
                ? static_cast<uint64_t>(s_video_frame_index) * 100000ULL / s_info.fps_x100
                : 0;
            s_video_frame_index++;
            // read_forward 已推进 s_current_offset 到数据末尾，只需补齐字节。
            s_current_offset += (chunk_size & 1);
            return ESP_OK;
        } else if (match_fcc(hdr, "01wb") || match_fcc(hdr, "00wb")) {
            // 音频帧
            if (chunk_size > capacity) {
                return ESP_ERR_NO_MEM;
            }
            if (read_forward(data, chunk_size) != ESP_OK) return ESP_FAIL;
            frame->is_video = false;
            frame->data = data;
            frame->size = chunk_size;
            frame->file_offset = s_current_offset;
            // PTS = 已读音频字节 * 1000 / 平均字节率
            frame->pts = s_info.audio_avg_bps > 0
                ? static_cast<uint64_t>(s_audio_bytes_total) * 1000ULL / s_info.audio_avg_bps
                : 0;
            s_audio_bytes_total += chunk_size;
            s_current_offset += (chunk_size & 1);
            return ESP_OK;
        } else if (match_fcc(hdr, "LIST")) {
            // movi 内嵌的 rec LIST：跳过 4 字节类型字段
            s_current_offset += 4;
            continue;
        } else {
            // 未知 chunk，跳过数据+补齐字节
            s_current_offset += chunk_size + (chunk_size & 1);
        }
    }

    return kErrEof;
}

esp_err_t seek_to_offset(uint32_t file_offset, uint32_t video_pts_ms, uint32_t audio_bytes_at_seek) {
    if (s_io == nullptr) return ESP_ERR_INVALID_STATE;
    s_current_offset = file_offset;
    // seek 后音视频 PTS 都从 0 开始，与重置后的音频时钟对齐。
    // video_pts_ms / audio_bytes_at_seek 仅用于诊断，不参与 PTS 计算。
    (void)video_pts_ms;
    (void)audio_bytes_at_seek;
    s_video_frame_index = 0;
    s_audio_bytes_total = 0;
    return ESP_OK;
}

void close() {
    s_io = nullptr;
    s_info = {};
    s_movi_data_offset = 0;
    s_movi_data_end = 0;
    s_current_offset = 0;
    s_video_frame_index = 0;
    s_audio_bytes_total = 0;
}

} // namespace AviDemuxer
