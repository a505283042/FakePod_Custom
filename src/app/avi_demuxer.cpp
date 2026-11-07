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

static constexpr uint32_t kMaxMoviRanges = 16U;

struct MoviRange
{
    uint32_t begin = 0U;
    uint32_t end = 0U;
};

struct PendingVideoChunk
{
    bool valid = false;
    uint32_t file_offset = 0U;
    uint32_t size = 0U;
    uint32_t range_index = 0U;
    uint8_t header[8] = {};
};

MoviRange s_movi_ranges[kMaxMoviRanges] = {};
PendingVideoChunk s_pending_video_chunk = {};
uint32_t s_movi_range_count = 0U;
uint32_t s_current_movi_range = 0U;
uint32_t s_current_offset = 0U;
uint32_t s_video_frame_index = 0U;
uint32_t s_audio_bytes_total = 0U;
uint32_t s_debug_chunk_count = 0U;
uint32_t s_file_cursor_offset = 0U;
bool s_file_cursor_valid = false;
IoStats s_io_stats = {};

uint32_t s_avih_total_frames = 0U;
uint32_t s_dmlh_total_frames = 0U;
uint32_t s_video_strh_length = 0U;
uint32_t s_video_strh_scale = 0U;
uint32_t s_video_strh_rate = 0U;

// 小端读取辅助。
inline uint16_t read_u16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) |
        (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_u32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
        (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24);
}

inline bool match_fcc(const uint8_t *p, const char *fcc)
{
    return p[0] == fcc[0] && p[1] == fcc[1] &&
        p[2] == fcc[2] && p[3] == fcc[3];
}

// 从文件绝对偏移读取 size 字节到 buf。连续读取时复用 FILE 当前游标，避免每个 AVI chunk 都 fseeko。
esp_err_t read_at(uint32_t offset, void *buf, uint32_t size)
{
    if (s_io == nullptr || s_io->file == nullptr) return ESP_ERR_INVALID_STATE;
    if (offset > s_io->size || size > s_io->size - offset) return ESP_ERR_INVALID_SIZE;

    if (s_file_cursor_valid && s_file_cursor_offset == offset) {
        ++s_io_stats.sequential_seek_skips;
    } else {
        const int64_t seek_started_us = esp_timer_get_time();
        if (VideoMediaIo::seek_cb(offset, s_io) < 0) {
            s_io->seek_errors++;
            s_file_cursor_valid = false;
            return ESP_FAIL;
        }
        const int64_t seek_elapsed_signed = esp_timer_get_time() - seek_started_us;
        const uint32_t seek_elapsed_us = seek_elapsed_signed > 0LL
            ? static_cast<uint32_t>(seek_elapsed_signed) : 0U;
        ++s_io_stats.fseek_calls;
        s_io_stats.fseek_us_total += seek_elapsed_us;
        if (seek_elapsed_us > s_io_stats.fseek_us_max) {
            s_io_stats.fseek_us_max = seek_elapsed_us;
        }
    }

    const int got = VideoMediaIo::read_cb(buf, size, s_io);
    if (got != static_cast<int>(size)) {
        s_file_cursor_valid = false;
        return ESP_FAIL;
    }
    s_file_cursor_offset = offset + size;
    s_file_cursor_valid = true;
    s_current_offset = offset + size;
    return ESP_OK;
}

// 从当前位置读取 size 字节。read_at 会在游标已连续时跳过 fseeko。
esp_err_t read_forward(void *buf, uint32_t size)
{
    const esp_err_t ret = read_at(s_current_offset, buf, size);
    if (ret == ESP_OK) {
        ++s_io_stats.forward_reads;
        s_io_stats.forward_bytes += size;
    }
    return ret;
}

// 顺序消费不需要解码的 payload，保持 FILE 游标连续，避免长文件 SEEK 时逐 chunk 随机定位。
esp_err_t skip_forward_bytes(
    uint32_t size, uint8_t *scratch, uint32_t scratch_capacity, uint64_t *out_skipped_bytes)
{
    if (size == 0U) return ESP_OK;
    if (scratch == nullptr || scratch_capacity == 0U) return ESP_ERR_INVALID_ARG;

    uint32_t remaining = size;
    while (remaining > 0U) {
        const uint32_t amount = remaining > scratch_capacity ? scratch_capacity : remaining;
        const esp_err_t ret = read_forward(scratch, amount);
        if (ret != ESP_OK) return ret;
        remaining -= amount;
        if (out_skipped_bytes != nullptr) *out_skipped_bytes += amount;
    }
    return ESP_OK;
}

bool add_movi_range(uint32_t begin, uint32_t end)
{
    if (begin >= end || s_movi_range_count >= kMaxMoviRanges) return false;
    if (s_movi_range_count > 0U &&
        begin < s_movi_ranges[s_movi_range_count - 1U].end) {
        return false;
    }
    s_movi_ranges[s_movi_range_count].begin = begin;
    s_movi_ranges[s_movi_range_count].end = end;
    ++s_movi_range_count;
    return true;
}

int32_t find_movi_range(uint32_t offset)
{
    for (uint32_t i = 0U; i < s_movi_range_count; ++i) {
        if (offset >= s_movi_ranges[i].begin && offset < s_movi_ranges[i].end) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// 当前 movi 段读完后，自动切换到下一个 RIFF/AVIX 段。
bool select_readable_movi_range()
{
    while (s_current_movi_range < s_movi_range_count) {
        const MoviRange &range = s_movi_ranges[s_current_movi_range];
        if (s_current_offset < range.begin || s_current_offset > range.end) {
            s_current_offset = range.begin;
        }
        if (s_current_offset < range.end &&
            range.end - s_current_offset >= 8U) {
            return true;
        }
        ++s_current_movi_range;
        if (s_current_movi_range < s_movi_range_count) {
            s_current_offset = s_movi_ranges[s_current_movi_range].begin;
        }
    }
    return false;
}

esp_err_t parse_stream_list(uint32_t list_data, uint32_t list_size,
    bool *out_found_video)
{
    if (out_found_video == nullptr || list_size < 4U) return ESP_ERR_INVALID_ARG;

    uint32_t pos = list_data + 4U;
    const uint32_t list_end = list_data + list_size;
    bool has_strh = false;
    uint8_t strh[56] = {};
    uint8_t strf[256] = {};
    uint32_t strf_size = 0U;

    while (pos <= list_end && list_end - pos >= 8U) {
        uint8_t hdr[8];
        if (read_at(pos, hdr, sizeof(hdr)) != ESP_OK) return ESP_FAIL;
        const uint32_t chunk_size = read_u32(hdr + 4U);
        const uint32_t data = pos + 8U;
        if (chunk_size > list_end - data) return ESP_ERR_INVALID_SIZE;

        if (match_fcc(hdr, "strh")) {
            if (chunk_size < sizeof(strh)) return ESP_ERR_INVALID_SIZE;
            if (read_at(data, strh, sizeof(strh)) != ESP_OK) return ESP_FAIL;
            has_strh = true;
        } else if (match_fcc(hdr, "strf")) {
            strf_size = chunk_size < sizeof(strf) ? chunk_size : sizeof(strf);
            if (strf_size > 0U && read_at(data, strf, strf_size) != ESP_OK) {
                return ESP_FAIL;
            }
        }

        const uint64_t next = static_cast<uint64_t>(data) +
            chunk_size + (chunk_size & 1U);
        if (next > list_end) return ESP_ERR_INVALID_SIZE;
        pos = static_cast<uint32_t>(next);
    }

    if (!has_strh) return ESP_OK;

    if (match_fcc(strh, "vids")) {
        if (!*out_found_video) {
            if (strf_size >= 40U) {
                s_info.width = static_cast<uint16_t>(read_u32(strf + 4U));
                s_info.height = static_cast<uint16_t>(read_u32(strf + 8U));
            }
            s_video_strh_scale = read_u32(strh + 20U);
            s_video_strh_rate = read_u32(strh + 24U);
            s_video_strh_length = read_u32(strh + 32U);
            *out_found_video = true;
        }
    } else if (match_fcc(strh, "auds")) {
        if (strf_size >= 18U) {
            s_info.audio_format = read_u16(strf + 0U);
            s_info.audio_channels = static_cast<uint8_t>(read_u16(strf + 2U));
            s_info.audio_sample_rate = static_cast<uint16_t>(read_u32(strf + 4U));
            s_info.audio_avg_bps = read_u32(strf + 8U);
            s_info.audio_bits_per_sample = static_cast<uint8_t>(read_u16(strf + 14U));
            s_info.audio_bitrate = s_info.audio_avg_bps * 8U;
            s_info.has_audio = true;
        }
    }

    return ESP_OK;
}

esp_err_t parse_odml_list(uint32_t list_data, uint32_t list_size)
{
    if (list_size < 4U) return ESP_ERR_INVALID_SIZE;
    uint32_t pos = list_data + 4U;
    const uint32_t list_end = list_data + list_size;

    while (pos <= list_end && list_end - pos >= 8U) {
        uint8_t hdr[8];
        if (read_at(pos, hdr, sizeof(hdr)) != ESP_OK) return ESP_FAIL;
        const uint32_t chunk_size = read_u32(hdr + 4U);
        const uint32_t data = pos + 8U;
        if (chunk_size > list_end - data) return ESP_ERR_INVALID_SIZE;

        if (match_fcc(hdr, "dmlh") && chunk_size >= 4U) {
            uint8_t total_frames[4];
            if (read_at(data, total_frames, sizeof(total_frames)) != ESP_OK) {
                return ESP_FAIL;
            }
            s_dmlh_total_frames = read_u32(total_frames);
        }

        const uint64_t next = static_cast<uint64_t>(data) +
            chunk_size + (chunk_size & 1U);
        if (next > list_end) return ESP_ERR_INVALID_SIZE;
        pos = static_cast<uint32_t>(next);
    }
    return ESP_OK;
}

esp_err_t parse_hdrl_list(uint32_t list_data, uint32_t list_size,
    bool *out_found_avih, bool *out_found_video)
{
    if (out_found_avih == nullptr || out_found_video == nullptr || list_size < 4U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pos = list_data + 4U;
    const uint32_t list_end = list_data + list_size;

    while (pos <= list_end && list_end - pos >= 8U) {
        uint8_t hdr[8];
        if (read_at(pos, hdr, sizeof(hdr)) != ESP_OK) return ESP_FAIL;
        const uint32_t chunk_size = read_u32(hdr + 4U);
        const uint32_t data = pos + 8U;
        if (chunk_size > list_end - data) return ESP_ERR_INVALID_SIZE;

        if (match_fcc(hdr, "avih")) {
            if (chunk_size < 56U) return ESP_ERR_INVALID_SIZE;
            uint8_t avih[56];
            if (read_at(data, avih, sizeof(avih)) != ESP_OK) return ESP_FAIL;
            const uint32_t us_per_frame = read_u32(avih + 0U);
            if (us_per_frame > 0U) {
                s_info.fps_x100 = static_cast<uint32_t>(
                    100000000ULL / us_per_frame);
            }
            s_avih_total_frames = read_u32(avih + 16U);
            *out_found_avih = true;
        } else if (match_fcc(hdr, "LIST") && chunk_size >= 4U) {
            uint8_t list_type[4];
            if (read_at(data, list_type, sizeof(list_type)) != ESP_OK) return ESP_FAIL;
            if (match_fcc(list_type, "strl")) {
                const esp_err_t ret = parse_stream_list(data, chunk_size, out_found_video);
                if (ret != ESP_OK) return ret;
            } else if (match_fcc(list_type, "odml")) {
                const esp_err_t ret = parse_odml_list(data, chunk_size);
                if (ret != ESP_OK) return ret;
            }
        }

        const uint64_t next = static_cast<uint64_t>(data) +
            chunk_size + (chunk_size & 1U);
        if (next > list_end) return ESP_ERR_INVALID_SIZE;
        pos = static_cast<uint32_t>(next);
    }
    return ESP_OK;
}

} // namespace

esp_err_t open(VideoMediaIo::Context *io, StreamInfo *out_info)
{
    if (io == nullptr || io->file == nullptr || out_info == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (io->size < 12U) return ESP_ERR_INVALID_SIZE;

    s_io = io;
    s_info = {};
    s_file_cursor_offset = 0U;
    s_file_cursor_valid = false;
    s_io_stats = {};
    memset(s_movi_ranges, 0, sizeof(s_movi_ranges));
    s_movi_range_count = 0U;
    s_current_movi_range = 0U;
    s_current_offset = 0U;
    s_pending_video_chunk = {};
    s_video_frame_index = 0U;
    s_audio_bytes_total = 0U;
    s_debug_chunk_count = 0U;
    s_avih_total_frames = 0U;
    s_dmlh_total_frames = 0U;
    s_video_strh_length = 0U;
    s_video_strh_scale = 0U;
    s_video_strh_rate = 0U;

    bool found_avih = false;
    bool found_video_strl = false;
    bool found_first_riff = false;
    uint32_t riff_count = 0U;
    uint32_t riff_pos = 0U;
    const uint32_t file_size = io->size;

    // OpenDML AVI 可以由一个 RIFF AVI 段和多个 RIFF AVIX 段组成。
    // 每个段分别包含自己的 movi；必须全部记录，不能只播放第一个约 1GB 段。
    while (riff_pos <= file_size && file_size - riff_pos >= 12U) {
        uint8_t riff[12];
        if (read_at(riff_pos, riff, sizeof(riff)) != ESP_OK) {
            if (!found_first_riff) return ESP_FAIL;
            break;
        }
        if (!match_fcc(riff, "RIFF")) {
            if (!found_first_riff) {
                ESP_LOGE(TAG, "AVI文件头不是 RIFF");
                return ESP_ERR_NOT_SUPPORTED;
            }
            break;
        }

        const uint32_t riff_size = read_u32(riff + 4U);
        if (riff_size < 4U || riff_size > file_size - riff_pos - 8U) {
            ESP_LOGE(TAG, "RIFF段长度非法：segment=%lu size=%lu file=%lu",
                static_cast<unsigned long>(riff_count),
                static_cast<unsigned long>(riff_size),
                static_cast<unsigned long>(file_size));
            return ESP_ERR_INVALID_SIZE;
        }

        const bool is_avi = match_fcc(riff + 8U, "AVI ");
        const bool is_avix = match_fcc(riff + 8U, "AVIX");
        if ((!found_first_riff && !is_avi) ||
            (found_first_riff && !is_avi && !is_avix)) {
            ESP_LOGE(TAG, "不支持的 RIFF 类型：segment=%lu type=%c%c%c%c",
                static_cast<unsigned long>(riff_count),
                riff[8], riff[9], riff[10], riff[11]);
            return ESP_ERR_NOT_SUPPORTED;
        }
        found_first_riff = true;

        const uint32_t riff_end = riff_pos + 8U + riff_size;
        uint32_t chunk_pos = riff_pos + 12U;

        while (chunk_pos <= riff_end && riff_end - chunk_pos >= 8U) {
            uint8_t hdr[8];
            if (read_at(chunk_pos, hdr, sizeof(hdr)) != ESP_OK) return ESP_FAIL;
            const uint32_t chunk_size = read_u32(hdr + 4U);
            const uint32_t data = chunk_pos + 8U;
            if (chunk_size > riff_end - data) {
                ESP_LOGE(TAG, "RIFF子chunk越界：riff=%lu offset=%lu size=%lu",
                    static_cast<unsigned long>(riff_count),
                    static_cast<unsigned long>(chunk_pos),
                    static_cast<unsigned long>(chunk_size));
                return ESP_ERR_INVALID_SIZE;
            }

            if (match_fcc(hdr, "LIST") && chunk_size >= 4U) {
                uint8_t list_type[4];
                if (read_at(data, list_type, sizeof(list_type)) != ESP_OK) return ESP_FAIL;

                if (match_fcc(list_type, "hdrl") && !found_avih) {
                    const esp_err_t ret = parse_hdrl_list(
                        data, chunk_size, &found_avih, &found_video_strl);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "AVI hdrl解析失败：%s", esp_err_to_name(ret));
                        return ret;
                    }
                } else if (match_fcc(list_type, "movi")) {
                    const uint32_t movi_begin = data + 4U;
                    const uint32_t movi_end = data + chunk_size;
                    if (!add_movi_range(movi_begin, movi_end)) {
                        ESP_LOGE(TAG, "AVI movi段数量/顺序非法：max=%lu count=%lu",
                            static_cast<unsigned long>(kMaxMoviRanges),
                            static_cast<unsigned long>(s_movi_range_count));
                        return ESP_ERR_INVALID_SIZE;
                    }
                }
            }

            const uint64_t next = static_cast<uint64_t>(data) +
                chunk_size + (chunk_size & 1U);
            if (next > riff_end) return ESP_ERR_INVALID_SIZE;
            chunk_pos = static_cast<uint32_t>(next);
        }

        const uint64_t next_riff = static_cast<uint64_t>(riff_end) + (riff_size & 1U);
        if (next_riff > file_size || next_riff <= riff_pos) break;
        if (next_riff == file_size) break;
        riff_pos = static_cast<uint32_t>(next_riff);
        ++riff_count;
    }

    if (!found_first_riff || !found_avih || !found_video_strl ||
        s_movi_range_count == 0U) {
        ESP_LOGE(TAG, "AVI头部解析失败: riff=%d avih=%d video_strl=%d movi_segments=%lu",
            found_first_riff, found_avih, found_video_strl,
            static_cast<unsigned long>(s_movi_range_count));
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_info.fps_x100 == 0U && s_video_strh_scale > 0U && s_video_strh_rate > 0U) {
        s_info.fps_x100 = static_cast<uint32_t>(
            (static_cast<uint64_t>(s_video_strh_rate) * 100ULL +
             s_video_strh_scale / 2U) / s_video_strh_scale);
    }
    if (s_info.fps_x100 == 0U) {
        ESP_LOGE(TAG, "AVI帧率无效：avih/strh均未提供有效帧率");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // dwLength 属于具体视频流，单位是该流的样本数（视频流即视频帧）。
    // 部分封装器会把 dmlh 写成所有媒体 chunk 数，或让 avih 只保留首个 RIFF 段帧数；
    // 因此不能对三个字段取最大值，优先使用视频 strh.dwLength。
    const char *frame_count_source = nullptr;
    if (s_video_strh_length > 0U) {
        s_info.total_frames = s_video_strh_length;
        frame_count_source = "video_strh.dwLength";
    } else if (s_dmlh_total_frames > 0U) {
        s_info.total_frames = s_dmlh_total_frames;
        frame_count_source = "odml.dmlh";
    } else {
        s_info.total_frames = s_avih_total_frames;
        frame_count_source = "avih.dwTotalFrames";
    }

    if (s_video_strh_length > 0U &&
        ((s_dmlh_total_frames > 0U && s_dmlh_total_frames != s_video_strh_length) ||
         (s_avih_total_frames > 0U && s_avih_total_frames != s_video_strh_length))) {
        ESP_LOGW(TAG,
            "AVI帧数元数据不一致：avih=%lu video_strh=%lu dmlh=%lu；采用视频流长度",
            static_cast<unsigned long>(s_avih_total_frames),
            static_cast<unsigned long>(s_video_strh_length),
            static_cast<unsigned long>(s_dmlh_total_frames));
    }

    uint64_t duration_ms = 0ULL;
    if (s_info.total_frames > 0U) {
        if (s_video_strh_scale > 0U && s_video_strh_rate > 0U) {
            duration_ms = static_cast<uint64_t>(s_info.total_frames) *
                s_video_strh_scale * 1000ULL / s_video_strh_rate;
        } else {
            duration_ms = static_cast<uint64_t>(s_info.total_frames) *
                100000ULL / s_info.fps_x100;
        }
    }
    if (duration_ms == 0ULL || duration_ms > UINT32_MAX) {
        ESP_LOGE(TAG, "AVI总时长无效：frames=%lu duration=%llu",
            static_cast<unsigned long>(s_info.total_frames),
            static_cast<unsigned long long>(duration_ms));
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_info.duration_ms = static_cast<uint32_t>(duration_ms);

    *out_info = s_info;
    s_current_movi_range = 0U;
    s_current_offset = s_movi_ranges[0].begin;
    // 后续 FVIX 加载会直接使用同一个 VideoMediaIo 文件句柄，首个播放读取必须重新定位。
    s_file_cursor_valid = false;
    s_io_stats = {};

    ESP_LOGI(TAG,
        "AVI时长来源：avih_frames=%lu strh_frames=%lu dmlh_frames=%lu selected=%lu source=%s",
        static_cast<unsigned long>(s_avih_total_frames),
        static_cast<unsigned long>(s_video_strh_length),
        static_cast<unsigned long>(s_dmlh_total_frames),
        static_cast<unsigned long>(s_info.total_frames),
        frame_count_source != nullptr ? frame_count_source : "unknown");
    ESP_LOGI(TAG,
        "AVI解析成功: %ux%u %u.%02ufps dur=%ums frames=%u audio=%s movi_segments=%lu",
        s_info.width, s_info.height,
        s_info.fps_x100 / 100U, s_info.fps_x100 % 100U,
        s_info.duration_ms, s_info.total_frames,
        s_info.has_audio ? "yes" : "no",
        static_cast<unsigned long>(s_movi_range_count));
    for (uint32_t i = 0U; i < s_movi_range_count; ++i) {
        ESP_LOGI(TAG, "AVI movi段[%lu]: offset=%lu end=%lu bytes=%lu",
            static_cast<unsigned long>(i),
            static_cast<unsigned long>(s_movi_ranges[i].begin),
            static_cast<unsigned long>(s_movi_ranges[i].end),
            static_cast<unsigned long>(s_movi_ranges[i].end - s_movi_ranges[i].begin));
    }
    return ESP_OK;
}

esp_err_t read_frame(
    FrameInfo *frame,
    uint8_t *video_data, uint32_t video_capacity,
    uint8_t *audio_data, uint32_t audio_capacity)
{
    if (frame == nullptr || s_io == nullptr || audio_data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    while (true) {
        // 无空闲视频槽位时，保留当前视频 chunk 的头部和文件位置；取得槽位后重试同一帧。
        const bool pending_video = s_pending_video_chunk.valid;
        if (pending_video) {
            if (s_pending_video_chunk.range_index >= s_movi_range_count) {
                return ESP_ERR_INVALID_STATE;
            }
            s_current_movi_range = s_pending_video_chunk.range_index;
        } else if (!select_readable_movi_range()) {
            break;
        }
        const MoviRange &range = s_movi_ranges[s_current_movi_range];
        const uint32_t chunk_offset = pending_video
            ? s_pending_video_chunk.file_offset : s_current_offset;
        uint8_t hdr[8] = {};
        uint32_t chunk_size = 0U;
        if (pending_video) {
            memcpy(hdr, s_pending_video_chunk.header, sizeof(hdr));
            chunk_size = s_pending_video_chunk.size;
        } else {
            if (read_at(chunk_offset, hdr, sizeof(hdr)) != ESP_OK) return ESP_FAIL;
            chunk_size = read_u32(hdr + 4U);
        }

        const uint32_t padded_chunk_size = chunk_size + (chunk_size & 1U);
        const uint32_t payload_offset = chunk_offset + 8U;
        if (payload_offset < chunk_offset || payload_offset > range.end ||
            padded_chunk_size < chunk_size ||
            padded_chunk_size > range.end - payload_offset) {
            ESP_LOGE(TAG, "movi chunk越界：range=%lu offset=%lu size=%lu end=%lu",
                static_cast<unsigned long>(s_current_movi_range),
                static_cast<unsigned long>(chunk_offset),
                static_cast<unsigned long>(chunk_size),
                static_cast<unsigned long>(range.end));
            return ESP_ERR_INVALID_SIZE;
        }

        if (!pending_video) {
            ++s_debug_chunk_count;
            if (s_debug_chunk_count <= 6U) {
                ESP_LOGI(TAG, "chunk#%lu @%lu '%c%c%c%c' size=%lu video=%lu audio=%luB",
                    static_cast<unsigned long>(s_debug_chunk_count),
                    static_cast<unsigned long>(chunk_offset),
                    hdr[0], hdr[1], hdr[2], hdr[3],
                    static_cast<unsigned long>(chunk_size),
                    static_cast<unsigned long>(s_video_frame_index),
                    static_cast<unsigned long>(s_audio_bytes_total));
            }
        }

        if (chunk_size == 0U) {
            if (match_fcc(hdr, "00dc") || match_fcc(hdr, "00db")) {
                ++s_video_frame_index;
            }
            s_pending_video_chunk = {};
            continue;
        }

        if (match_fcc(hdr, "00dc") || match_fcc(hdr, "00db")) {
            if (video_data == nullptr || video_capacity == 0U) {
                if (!pending_video) {
                    s_pending_video_chunk.valid = true;
                    s_pending_video_chunk.file_offset = chunk_offset;
                    s_pending_video_chunk.size = chunk_size;
                    s_pending_video_chunk.range_index = s_current_movi_range;
                    memcpy(s_pending_video_chunk.header, hdr, sizeof(hdr));
                }
                return kErrNeedVideoSlot;
            }
            if (chunk_size > video_capacity) {
                ESP_LOGE(TAG, "视频帧过大: %lu > %lu",
                    static_cast<unsigned long>(chunk_size),
                    static_cast<unsigned long>(video_capacity));
                return ESP_ERR_NO_MEM;
            }
            // 读头时已将 FILE 游标推进到 payload 起点；无论首次读取还是 pending 重试，都顺序取帧。
            if (s_current_offset != payload_offset) return ESP_ERR_INVALID_STATE;
            if (read_forward(video_data, chunk_size) != ESP_OK) return ESP_FAIL;
            if ((chunk_size & 1U) != 0U) {
                uint8_t padding = 0U;
                if (read_forward(&padding, 1U) != ESP_OK) return ESP_FAIL;
            }
            frame->is_video = true;
            frame->data = video_data;
            frame->size = chunk_size;
            frame->file_offset = chunk_offset;
            frame->pts = static_cast<uint64_t>(s_video_frame_index) *
                100000ULL / s_info.fps_x100;
            ++s_video_frame_index;
            s_pending_video_chunk = {};
            return ESP_OK;
        }

        if (match_fcc(hdr, "01wb") || match_fcc(hdr, "00wb")) {
            if (chunk_size > audio_capacity) {
                ESP_LOGE(TAG, "MP3音频chunk过大: %lu > %lu",
                    static_cast<unsigned long>(chunk_size),
                    static_cast<unsigned long>(audio_capacity));
                return ESP_ERR_NO_MEM;
            }
            if (read_forward(audio_data, chunk_size) != ESP_OK) return ESP_FAIL;
            if ((chunk_size & 1U) != 0U) {
                uint8_t padding = 0U;
                if (read_forward(&padding, 1U) != ESP_OK) return ESP_FAIL;
            }
            frame->is_video = false;
            frame->data = audio_data;
            frame->size = chunk_size;
            frame->file_offset = chunk_offset;
            frame->pts = s_info.audio_avg_bps > 0U
                ? static_cast<uint64_t>(s_audio_bytes_total) * 1000ULL /
                    s_info.audio_avg_bps
                : 0ULL;
            s_audio_bytes_total += chunk_size;
            ++s_io_stats.audio_chunks;
            s_io_stats.audio_bytes += chunk_size;
            return ESP_OK;
        }

        if (match_fcc(hdr, "LIST") && chunk_size >= 4U) {
            uint8_t list_type[4];
            if (read_forward(list_type, sizeof(list_type)) != ESP_OK) return ESP_FAIL;
            if (match_fcc(list_type, "rec ")) continue;
            const uint32_t remaining = padded_chunk_size - 4U;
            if (skip_forward_bytes(remaining, audio_data, audio_capacity, nullptr) != ESP_OK) {
                return ESP_FAIL;
            }
            continue;
        }

        // 未知 chunk 按 payload 顺序读过，保持 FILE 游标连续，不再通过下一轮 fseek 跳过。
        if (skip_forward_bytes(padded_chunk_size, audio_data, audio_capacity, nullptr) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    ESP_LOGW(TAG, "到达全部movi末尾：segments=%lu chunks=%lu video=%lu audio=%luB",
        static_cast<unsigned long>(s_movi_range_count),
        static_cast<unsigned long>(s_debug_chunk_count),
        static_cast<unsigned long>(s_video_frame_index),
        static_cast<unsigned long>(s_audio_bytes_total));
    return kErrEof;
}

esp_err_t seek_to_offset(uint32_t file_offset, uint32_t video_pts_ms,
    uint32_t audio_bytes_at_seek, uint32_t video_frame_index)
{
    if (s_io == nullptr) return ESP_ERR_INVALID_STATE;

    auto video_chunk_at = [](uint32_t off, int32_t *out_range) -> bool {
        if (out_range == nullptr) return false;
        const int32_t range_index = find_movi_range(off);
        if (range_index < 0) return false;
        const MoviRange &range = s_movi_ranges[static_cast<uint32_t>(range_index)];
        if (range.end - off < 10U) return false;

        uint8_t hdr[8];
        if (read_at(off, hdr, sizeof(hdr)) != ESP_OK) return false;
        if (!(match_fcc(hdr, "00dc") || match_fcc(hdr, "00db"))) return false;
        const uint32_t size = read_u32(hdr + 4U);
        const uint32_t padded_size = size + (size & 1U);
        if (size < 2U || size > (8U * 1024U * 1024U) ||
            padded_size < size || padded_size > range.end - off - 8U) return false;
        uint8_t sig[2];
        if (read_at(off + 8U, sig, sizeof(sig)) != ESP_OK) return false;
        if (sig[0] != 0xFFU || sig[1] != 0xD8U) return false;
        *out_range = range_index;
        return true;
    };

    uint32_t aligned = file_offset;
    int32_t range_index = -1;
    if (!video_chunk_at(file_offset, &range_index)) {
        if (file_offset >= 8U &&
            video_chunk_at(file_offset - 8U, &range_index)) {
            aligned = file_offset - 8U;
            ESP_LOGW(TAG, "Seek偏移按数据起点解释，回退8字节: %lu -> %lu",
                static_cast<unsigned long>(file_offset),
                static_cast<unsigned long>(aligned));
        } else {
            ESP_LOGE(TAG, "Seek偏移无效，拒绝跳转: %lu",
                static_cast<unsigned long>(file_offset));
            return ESP_ERR_INVALID_ARG;
        }
    }

    s_current_movi_range = static_cast<uint32_t>(range_index);
    s_current_offset = aligned;
    s_pending_video_chunk = {};
    s_video_frame_index = video_frame_index != 0xFFFFFFFFU
        ? video_frame_index
        : static_cast<uint32_t>(
            static_cast<uint64_t>(video_pts_ms) * s_info.fps_x100 / 100000ULL);
    s_audio_bytes_total = audio_bytes_at_seek;
    s_file_cursor_valid = false;

    ESP_LOGI(TAG, "seek恢复时间轴: segment=%lu frame_index=%lu fps=%lu.%02lu audio_bytes=%lu",
        static_cast<unsigned long>(s_current_movi_range),
        static_cast<unsigned long>(s_video_frame_index),
        static_cast<unsigned long>(s_info.fps_x100 / 100U),
        static_cast<unsigned long>(s_info.fps_x100 % 100U),
        static_cast<unsigned long>(s_audio_bytes_total));
    return ESP_OK;
}

esp_err_t scan_forward_to(uint32_t target_ms, uint8_t *scratch, uint32_t scratch_capacity)
{
    if (s_io == nullptr) return ESP_ERR_INVALID_STATE;
    if (s_info.fps_x100 == 0U) return ESP_OK;
    if (scratch == nullptr || scratch_capacity == 0U) return ESP_ERR_INVALID_ARG;

    const int64_t scan_started_us = esp_timer_get_time();
    const uint32_t fseek_before = s_io_stats.fseek_calls;
    uint32_t scan_chunks = 0U;
    uint64_t skipped_bytes = 0ULL;

    auto finish_scan = [&](esp_err_t result) -> esp_err_t {
        const int64_t elapsed = esp_timer_get_time() - scan_started_us;
        const uint64_t elapsed_us = elapsed > 0LL ? static_cast<uint64_t>(elapsed) : 0ULL;
        s_io_stats.seek_scan_chunks = scan_chunks;
        s_io_stats.seek_scan_skipped_bytes = skipped_bytes;
        s_io_stats.seek_scan_duration_us = elapsed_us;
        s_io_stats.seek_scan_fseek_calls = s_io_stats.fseek_calls - fseek_before;
        ESP_LOGI(TAG,
            "SEEK精确扫描统计：结果=%s 耗时=%llums chunks=%lu 顺序跳过=%lluB fseek=%lu",
            esp_err_to_name(result),
            static_cast<unsigned long long>(elapsed_us / 1000ULL),
            static_cast<unsigned long>(scan_chunks),
            static_cast<unsigned long long>(skipped_bytes),
            static_cast<unsigned long>(s_io_stats.seek_scan_fseek_calls));
        return result;
    };

    s_pending_video_chunk = {};
    uint32_t last_video_offset = 0U;
    uint32_t last_video_index = 0U;
    uint32_t last_audio_bytes_total = 0U;
    uint32_t last_range_index = 0U;
    bool have_last = false;
    uint32_t skipped_frames = 0U;

    while (select_readable_movi_range()) {
        const MoviRange &range = s_movi_ranges[s_current_movi_range];
        const uint32_t chunk_offset = s_current_offset;
        uint8_t hdr[8];
        if (read_at(chunk_offset, hdr, sizeof(hdr)) != ESP_OK) {
            return finish_scan(ESP_FAIL);
        }
        ++scan_chunks;
        const uint32_t chunk_size = read_u32(hdr + 4U);
        const uint32_t padded_chunk_size = chunk_size + (chunk_size & 1U);
        const uint32_t payload_offset = chunk_offset + 8U;
        if (payload_offset < chunk_offset || payload_offset > range.end ||
            padded_chunk_size < chunk_size ||
            padded_chunk_size > range.end - payload_offset) {
            return finish_scan(ESP_ERR_INVALID_SIZE);
        }

        if (chunk_size == 0U) {
            if (match_fcc(hdr, "00dc") || match_fcc(hdr, "00db")) {
                ++s_video_frame_index;
            }
            continue;
        }

        if (match_fcc(hdr, "00dc") || match_fcc(hdr, "00db")) {
            const uint32_t pts_ms = static_cast<uint32_t>(
                static_cast<uint64_t>(s_video_frame_index) *
                100000ULL / s_info.fps_x100);
            if (pts_ms >= target_ms) {
                s_pending_video_chunk.valid = true;
                s_pending_video_chunk.file_offset = chunk_offset;
                s_pending_video_chunk.size = chunk_size;
                s_pending_video_chunk.range_index = s_current_movi_range;
                memcpy(s_pending_video_chunk.header, hdr, sizeof(hdr));
                ESP_LOGI(TAG, "seek精确定位: target=%lums segment=%lu frame_index=%lu pts=%lums skipped=%lu",
                    static_cast<unsigned long>(target_ms),
                    static_cast<unsigned long>(s_current_movi_range),
                    static_cast<unsigned long>(s_video_frame_index),
                    static_cast<unsigned long>(pts_ms),
                    static_cast<unsigned long>(skipped_frames));
                return finish_scan(ESP_OK);
            }

            last_video_offset = chunk_offset;
            last_video_index = s_video_frame_index;
            last_audio_bytes_total = s_audio_bytes_total;
            last_range_index = s_current_movi_range;
            have_last = true;
            ++s_video_frame_index;
            ++skipped_frames;
            if (skip_forward_bytes(padded_chunk_size, scratch, scratch_capacity, &skipped_bytes) != ESP_OK) {
                return finish_scan(ESP_FAIL);
            }
            continue;
        }

        if (match_fcc(hdr, "01wb") || match_fcc(hdr, "00wb")) {
            s_audio_bytes_total += chunk_size;
            if (skip_forward_bytes(padded_chunk_size, scratch, scratch_capacity, &skipped_bytes) != ESP_OK) {
                return finish_scan(ESP_FAIL);
            }
            continue;
        }

        if (match_fcc(hdr, "LIST") && chunk_size >= 4U) {
            uint8_t list_type[4];
            if (read_forward(list_type, sizeof(list_type)) != ESP_OK) {
                return finish_scan(ESP_FAIL);
            }
            if (match_fcc(list_type, "rec ")) continue;
            if (skip_forward_bytes(padded_chunk_size - 4U, scratch, scratch_capacity, &skipped_bytes) != ESP_OK) {
                return finish_scan(ESP_FAIL);
            }
            continue;
        }

        if (skip_forward_bytes(padded_chunk_size, scratch, scratch_capacity, &skipped_bytes) != ESP_OK) {
            return finish_scan(ESP_FAIL);
        }
    }

    if (have_last) {
        s_current_movi_range = last_range_index;
        s_current_offset = last_video_offset;
        s_pending_video_chunk = {};
        s_video_frame_index = last_video_index;
        s_audio_bytes_total = last_audio_bytes_total;
        s_file_cursor_valid = false;
        ESP_LOGW(TAG, "seek目标超出末尾，回退到最后一帧: segment=%lu frame_index=%lu",
            static_cast<unsigned long>(last_range_index),
            static_cast<unsigned long>(last_video_index));
        return finish_scan(ESP_OK);
    }

    ESP_LOGW(TAG, "seek扫描未找到可解码视频帧: target=%lums",
        static_cast<unsigned long>(target_ms));
    return finish_scan(kErrEof);
}

uint32_t next_video_pts_ms()
{
    if (s_info.fps_x100 == 0U) return 0U;
    return static_cast<uint32_t>(
        static_cast<uint64_t>(s_video_frame_index) * 100000ULL / s_info.fps_x100);
}

void get_io_stats(IoStats *out_stats)
{
    if (out_stats == nullptr) return;
    *out_stats = s_io_stats;
}

void close()
{
    s_io = nullptr;
    s_info = {};
    memset(s_movi_ranges, 0, sizeof(s_movi_ranges));
    s_movi_range_count = 0U;
    s_current_movi_range = 0U;
    s_current_offset = 0U;
    s_pending_video_chunk = {};
    s_video_frame_index = 0U;
    s_audio_bytes_total = 0U;
    s_debug_chunk_count = 0U;
    s_file_cursor_offset = 0U;
    s_file_cursor_valid = false;
    s_io_stats = {};
    s_avih_total_frames = 0U;
    s_dmlh_total_frames = 0U;
    s_video_strh_length = 0U;
    s_video_strh_scale = 0U;
    s_video_strh_rate = 0U;
}

} // namespace AviDemuxer
