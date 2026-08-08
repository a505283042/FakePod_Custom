#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 正式播放器接口：启动 32bit slot / 立体声 / Philips I2S TX。
// Stage 9.2 支持 44.1kHz 与 48kHz，后续解码器复用同一 PCM 输出层。
esp_err_t i2s_output_stream_start_32bit(uint32_t sample_rate_hz);

// 写入已经转换成 32bit slot 的双声道交错 PCM。
// 函数会正确处理 i2s_channel_write() 的“部分写入 + timeout”。
esp_err_t i2s_output_stream_write_pcm32(
    const int32_t *interleaved_stereo,
    size_t frames,
    uint32_t timeout_ms
);

// 播放暂停、启动/关闭过渡期间持续发送全零 PCM，保持 BCLK/LRCK 连续。
esp_err_t i2s_output_stream_write_silence(size_t frames, uint32_t timeout_ms);

// 停止并释放正式播放器 I2S TX；如果旧自检任务存在，也会先等待其自行退出。
esp_err_t i2s_output_stop(void);

// 判断 I2S TX 是否已经启动。
bool i2s_output_is_started(void);
uint32_t i2s_output_sample_rate_hz(void);

// ------------------------------------------------------------
// Stage 8.x 保留的硬件自检接口。
// 正常开机不再调用，但保留给后续“系统诊断”页面复用。
// ------------------------------------------------------------
esp_err_t i2s_output_start_48k_32bit(void);
esp_err_t i2s_output_wait_silence_ms(uint32_t duration_ms);
void i2s_output_set_test_tone(bool enabled);

#ifdef __cplusplus
}
#endif
