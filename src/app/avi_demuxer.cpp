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
uint32_t s_debug_chunk_count = 0;  // 临时诊断：已扫描的 chunk 数量

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
    s_debug_chunk_count = 0;

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
                            // 先提升到64位再乘，避免30分钟以上视频的时长在32位乘法中回绕。
                            s_info.duration_ms = s_info.total_frames > 0 && s_info.fps_x100 > 0
                                ? static_cast<uint32_t>(
                                      static_cast<uint64_t>(s_info.total_frames) * 100000ULL /
                                      s_info.fps_x100)
                                : 0U;
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
    // 顶层扫描（含其后的 idx1/HDRL 等 chunk）会移动 s_current_offset，
    // 这里固定回 movi 起点，避免首帧读取直接从文件末尾开始。
    s_current_offset = s_movi_data_offset;
    ESP_LOGI(TAG, "AVI解析成功: %ux%u %u.%02ufps dur=%ums frames=%u audio=%s",
        s_info.width, s_info.height,
        s_info.fps_x100 / 100, s_info.fps_x100 % 100,
        s_info.duration_ms, s_info.total_frames,
        s_info.has_audio ? "yes" : "no");
    ESP_LOGI(TAG, "movi数据区: offset=%lu end=%lu 文件大小=%lu",
        static_cast<unsigned long>(s_movi_data_offset),
        static_cast<unsigned long>(s_movi_data_end),
        static_cast<unsigned long>(file_size));
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

        // 临时诊断：确认扫描起点与 chunk 头解析是否正确。
        ++s_debug_chunk_count;
        if (s_debug_chunk_count <= 6U) {
            ESP_LOGI(TAG, "chunk#%lu @%lu '%c%c%c%c' size=%lu video=%lu audio=%luB",
                static_cast<unsigned long>(s_debug_chunk_count),
                static_cast<unsigned long>(s_current_offset),
                hdr[0], hdr[1], hdr[2], hdr[3],
                static_cast<unsigned long>(chunk_size),
                static_cast<unsigned long>(s_video_frame_index),
                static_cast<unsigned long>(s_audio_bytes_total));
        }

        if (chunk_size == 0U) {
            // AVI 中可能存在 0 字节占位 chunk（如 '00dc' size=0），无法解码，直接跳过。
            // 占位视频帧仍占一个帧位，计数继续推进，保证后续 PTS 与 avih 帧序一致。
            // read_at 读头时已把 s_current_offset 推进到数据起点；size=0 无数据，直接继续。
            if (match_fcc(hdr, "00dc") || match_fcc(hdr, "00db")) {
                s_video_frame_index++;
            }
            continue;
        }

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

    ESP_LOGW(TAG, "到达movi末尾: off=%lu end=%lu chunks=%lu video=%lu audio=%luB",
        static_cast<unsigned long>(s_current_offset),
        static_cast<unsigned long>(s_movi_data_end),
        static_cast<unsigned long>(s_debug_chunk_count),
        static_cast<unsigned long>(s_video_frame_index),
        static_cast<unsigned long>(s_audio_bytes_total));
    return kErrEof;
}

esp_err_t seek_to_offset(uint32_t file_offset, uint32_t video_pts_ms,
    uint32_t audio_bytes_at_seek, uint32_t video_frame_index) {
    if (s_io == nullptr) return ESP_ERR_INVALID_STATE;

    // 侧车索引可能记录 chunk 头偏移，也可能记录数据起点（如 ffprobe 的 pkt_pos）。
    // 这里按「chunk 头 + JPEG 起始标记」校验落点，必要时回退 8 字节。
    auto video_chunk_at = [](uint32_t off) -> bool {
        // 先做减法式边界校验，避免坏索引偏移导致加法溢出或越过 movi。
        if (off < s_movi_data_offset || off >= s_movi_data_end ||
            s_movi_data_end - off < 10U) return false;
        uint8_t hdr[8];
        if (read_at(off, hdr, 8) != ESP_OK) return false;
        if (!(match_fcc(hdr, "00dc") || match_fcc(hdr, "00db"))) return false;
        const uint32_t size = read_u32(hdr + 4);
        if (size < 2U || size > (8U * 1024U * 1024U) ||
            size > s_movi_data_end - off - 8U) return false;
        uint8_t sig[2];
        if (read_at(off + 8U, sig, 2) != ESP_OK) return false;
        return sig[0] == 0xFFU && sig[1] == 0xD8U;
    };

    uint32_t aligned = file_offset;
    if (!video_chunk_at(file_offset)) {
        if (file_offset >= 8U && video_chunk_at(file_offset - 8U)) {
            aligned = file_offset - 8U;
            ESP_LOGW(TAG, "Seek偏移按数据起点解释，回退8字节: %lu -> %lu",
                static_cast<unsigned long>(file_offset),
                static_cast<unsigned long>(aligned));
        } else {
            // 不允许把未经验证的偏移交给解复用器，否则旧索引可能读出随机帧。
            ESP_LOGE(TAG, "Seek偏移无效，拒绝跳转: %lu",
                static_cast<unsigned long>(file_offset));
            return ESP_ERR_INVALID_ARG;
        }
    }

    s_current_offset = aligned;
    // 保持绝对时间轴：用 seek 目标时间恢复视频帧计数与音频字节计数，
    // 这样 PTS 继续在整段视频的原始时间轴上累加（而不是从 0 重来）。
    // 上层（Presenter/seek 目标计算）都以绝对 PTS 为准，无需再维护额外基准。
    // V2 索引直接保存原始帧序号，避免电脑端与设备端的帧率取整差异。
    // V1 索引没有帧序号，继续按时间戳和设备端帧率回推。
    s_video_frame_index = video_frame_index != 0xFFFFFFFFU
        ? video_frame_index
        : (s_info.fps_x100 > 0
            ? static_cast<uint32_t>(static_cast<uint64_t>(video_pts_ms) * s_info.fps_x100 / 100000ULL)
            : 0U);
    s_audio_bytes_total = audio_bytes_at_seek;
    ESP_LOGI(TAG, "seek恢复时间轴: frame_index=%lu fps=%lu.%02lu audio_bytes=%lu",
        static_cast<unsigned long>(s_video_frame_index),
        static_cast<unsigned long>(s_info.fps_x100 / 100U),
        static_cast<unsigned long>(s_info.fps_x100 % 100U),
        static_cast<unsigned long>(s_audio_bytes_total));
    return ESP_OK;
}

esp_err_t scan_forward_to(uint32_t target_ms) {
    if (s_io == nullptr) return ESP_ERR_INVALID_STATE;
    if (s_info.fps_x100 == 0U) return ESP_OK;

    uint32_t last_video_offset = 0U;
    uint32_t last_video_index = 0U;
    uint32_t last_audio_bytes_total = 0U;
    bool have_last = false;
    uint32_t skipped = 0U;

    while (s_current_offset + 8 <= s_movi_data_end) {
        const uint32_t chunk_offset = s_current_offset;
        uint8_t hdr[8];
        if (read_at(s_current_offset, hdr, 8) != ESP_OK) return ESP_FAIL;
        const uint32_t chunk_size = read_u32(hdr + 4);

        if (chunk_size == 0U) {
            // 与 read_frame 一致：占位视频帧占用帧序号，但不参与定位。
            if (match_fcc(hdr, "00dc") || match_fcc(hdr, "00db")) {
                ++s_video_frame_index;
            }
            continue;
        }

        if (match_fcc(hdr, "00dc") || match_fcc(hdr, "00db")) {
            const uint32_t pts_ms = static_cast<uint32_t>(
                static_cast<uint64_t>(s_video_frame_index) * 100000ULL / s_info.fps_x100);
            if (pts_ms >= target_ms) {
                // 回退到 chunk 头，后续 read_frame 会从这里取出该帧。
                s_current_offset = chunk_offset;
                ESP_LOGI(TAG, "seek精确定位: target=%lums frame_index=%lu pts=%lums skipped=%lu",
                    static_cast<unsigned long>(target_ms),
                    static_cast<unsigned long>(s_video_frame_index),
                    static_cast<unsigned long>(pts_ms),
                    static_cast<unsigned long>(skipped));
                return ESP_OK;
            }
            last_video_offset = chunk_offset;
            last_video_index = s_video_frame_index;
            last_audio_bytes_total = s_audio_bytes_total;
            have_last = true;
            ++s_video_frame_index;
            ++skipped;
            s_current_offset = chunk_offset + 8U + chunk_size + (chunk_size & 1U);
            continue;
        }

        if (match_fcc(hdr, "01wb") || match_fcc(hdr, "00wb")) {
            // 跳过的音频帧仍累加字节数，保持音频 PTS 与视频同一时间轴。
            s_audio_bytes_total += chunk_size;
            s_current_offset = chunk_offset + 8U + chunk_size + (chunk_size & 1U);
            continue;
        }

        if (match_fcc(hdr, "LIST")) {
            s_current_offset += 4U;
            continue;
        }

        s_current_offset = chunk_offset + 8U + chunk_size + (chunk_size & 1U);
    }

    if (have_last) {
        // 目标超出最后一个可解码帧：停在最后一帧，避免 seek 到末尾直接触发 EOF 切换视频。
        s_current_offset = last_video_offset;
        s_video_frame_index = last_video_index;
        s_audio_bytes_total = last_audio_bytes_total;
        ESP_LOGW(TAG, "seek目标超出末尾，回退到最后一帧: frame_index=%lu",
            static_cast<unsigned long>(last_video_index));
        return ESP_OK;
    }

    ESP_LOGW(TAG, "seek扫描未找到可解码视频帧: target=%lums",
        static_cast<unsigned long>(target_ms));
    return kErrEof;
}

uint32_t next_video_pts_ms()
{
    if (s_info.fps_x100 == 0U) return 0U;
    return static_cast<uint32_t>(
        static_cast<uint64_t>(s_video_frame_index) * 100000ULL / s_info.fps_x100);
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
