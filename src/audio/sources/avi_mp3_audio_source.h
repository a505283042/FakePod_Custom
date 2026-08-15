#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "audio_source.h"

// AVI ExtractTask -> AudioTask 的单生产者/单消费者压缩 MP3 byte bridge。
// payload 放 PSRAM；AudioTask 只通过 AudioSource 接口读取，不接触 Extractor/TF。
struct AviMp3AudioSource
{
    bool open = false;
};

struct AviMp3BridgeSnapshot
{
    bool active = false;
    bool eof = false;
    bool cancelled = false;
    size_t buffered_bytes = 0U;
    size_t capacity_bytes = 0U;
    size_t high_water_bytes = 0U;
    uint64_t bytes_pushed = 0ULL;
    uint64_t bytes_read = 0ULL;
    uint32_t push_wait_count = 0U;
    uint32_t read_wait_count = 0U;
    uint64_t read_wait_us_total = 0ULL;
    uint32_t read_wait_us_max = 0U;
    uint32_t read_wait_over_1ms = 0U;
    uint32_t read_wait_over_5ms = 0U;
    uint32_t read_wait_over_10ms = 0U;
    uint32_t read_wait_timeout_count = 0U;
};

// 每次 AVI 启动前重建一个干净 bridge。当前 V1 使用 32KB，约可容纳 2 秒 128kbps MP3。
esp_err_t avi_mp3_bridge_begin(size_t capacity_bytes);

// ExtractTask 写入 MP3 payload；必须完整写入，不允许静默丢音频字节。
esp_err_t avi_mp3_bridge_push(const void *data, size_t bytes, uint32_t timeout_ms);

// 正常 AVI EOS 与取消分开标记；AudioSource 会在 ring 排空后发布 EOF。
void avi_mp3_bridge_mark_eof();
void avi_mp3_bridge_cancel();

// 仅在 ExtractTask 已退出且 AudioSource 已关闭后释放 PSRAM/semaphore。
esp_err_t avi_mp3_bridge_release();
bool avi_mp3_bridge_get_snapshot(AviMp3BridgeSnapshot *out_snapshot);

// AudioTask 打开只读 STREAMING AudioSource；不提供 seek/size，MP3 decoder 走 streaming open。
esp_err_t avi_mp3_audio_source_open(AudioSource *out_source, AviMp3AudioSource *storage);
