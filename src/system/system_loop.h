#pragma once

#include <stdint.h>

// 当前曲若因确定性的封面尺寸/PSRAM预算拒绝进入默认封面终态，UI可直接跳过 dimmed Surface 等待。
bool system_artwork_current_uses_default_fallback(uint32_t track_index);

// 系统主循环
void system_loop_update();