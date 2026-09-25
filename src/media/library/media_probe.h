#pragma once

#include <stdio.h>

#include "esp_err.h"
#include "media_types.h"

// 扫描阶段解析文件头，产出播放阶段可复用的技术参数/关键偏移。
// 当前深度解析 FLAC/MP3/Ogg Opus；其它格式保留为空。
esp_err_t media_probe_file(
    const char *path,
    MediaFormat format,
    MediaTechnicalInfo *out_info
);

// 已由上层打开文件时复用同一 FILE*，避免大目录中重复 fopen(path) 的目录查找开销。
// 调用方负责 TF 锁、文件生命周期；函数不会关闭 file。
esp_err_t media_probe_open_file(
    FILE *file,
    MediaFormat format,
    uint64_t file_size,
    MediaTechnicalInfo *out_info
);
