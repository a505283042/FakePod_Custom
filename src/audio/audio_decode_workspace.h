#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// AudioTask 串行驱动所有 codec，因此压缩输入窗口和 codec PCM 输出缓冲可以跨格式复用。
// 这里只管理 decoder 自己的大块 PSRAM 工作区；FLAC Prefetch Ring / I2S DMA 仍保持独立生命周期。
struct AudioDecodeWorkspace
{
    uint8_t *input = nullptr;
    size_t input_capacity = 0;

    uint8_t *decoded = nullptr;
    size_t decoded_capacity = 0;
};

// 按需扩容，永不回退内部 RAM。扩容成功后旧缓冲会释放；调用方应重新获取返回指针。
esp_err_t audio_decode_workspace_reserve_input(
    AudioDecodeWorkspace *workspace,
    size_t required_bytes,
    uint8_t **out_buffer
);

esp_err_t audio_decode_workspace_reserve_decoded(
    AudioDecodeWorkspace *workspace,
    size_t required_bytes,
    uint8_t **out_buffer
);

// AudioTask 正常寿命内通常不释放 workspace，以便连续切歌复用分配；仅任务退出/初始化失败时调用。
// 释放异常膨胀的大工作区；常规 32KB input / 数十KB PCM 会继续跨曲复用。
void audio_decode_workspace_trim(
    AudioDecodeWorkspace *workspace,
    size_t max_retained_input_bytes,
    size_t max_retained_decoded_bytes
);

void audio_decode_workspace_destroy(AudioDecodeWorkspace *workspace);
size_t audio_decode_workspace_total_bytes(const AudioDecodeWorkspace *workspace);
