#pragma once

#include "esp_err.h"
#include "media_types.h"

// 扫描阶段解析文件头，产出播放阶段可复用的技术参数/关键偏移。
// 当前 Stage 10.0 深度解析 FLAC/MP3；其它格式保留为空，后续逐步补齐。
esp_err_t media_probe_file(
    const char *path,
    MediaFormat format,
    MediaTechnicalInfo *out_info
);
