#pragma once

#include <stdio.h>
#include <stdint.h>
#include "audio_source.h"

// 本地 TF/FATFS Source。结构由 PcmDecoder 内嵌持有，不使用动态对象分配。
struct SdFileAudioSource
{
    FILE *file = nullptr;
    uint64_t size_bytes = 0;
};

// 打开本地文件并绑定到通用 AudioSource。成功后文件位置位于 0。
esp_err_t sd_file_audio_source_open(
    AudioSource *out_source,
    SdFileAudioSource *storage,
    const char *path
);
