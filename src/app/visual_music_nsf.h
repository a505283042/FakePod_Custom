#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace VisualMusicNsf
{

enum class LoadState : uint8_t
{
    Idle = 0,
    Ready,
    Failed,
};

// NSF V1 容器镜像。完整文件保存在 PSRAM，PRG 直接指向 file_data + 128，
// 下一阶段 6502/APU Core 可直接接管，不需要再次读取或复制 TF 文件。
struct Image
{
    uint8_t *file_data = nullptr; // PSRAM，所有权属于 Image
    size_t file_size = 0U;
    const uint8_t *prg_data = nullptr;
    size_t prg_size = 0U;

    uint16_t load_address = 0U;
    uint16_t init_address = 0U;
    uint16_t play_address = 0U;
    uint16_t ntsc_speed_us = 0U;
    uint16_t pal_speed_us = 0U;
    uint8_t banks[8] = {};

    uint8_t version = 0U;
    uint8_t track_count = 0U;
    uint8_t initial_track = 0U; // 0-based
    uint8_t pal_ntsc_bits = 0U;
    uint8_t expansion_chips = 0U;

    char song_name[33] = {};
    char artist[33] = {};
    char copyright[33] = {};
};

struct LoadResult
{
    LoadState state = LoadState::Idle;
    esp_err_t result = ESP_OK;
    uint32_t generation = 0U;
    Image image = {};
};

esp_err_t init();

// VM09 先支持传统 NESM NSF；NSFE/NSF2 容器在后续阶段单独接入。
esp_err_t start(const char *path);
void cancel();

// USB MSC 接管前取消所有旧 NSF Loader，并等待其 FILE 真正关闭。
bool prepare_storage_handoff(TickType_t timeout_ticks);

bool take_result(LoadResult *out_result);
void release_image(Image *image);

} // namespace VisualMusicNsf
