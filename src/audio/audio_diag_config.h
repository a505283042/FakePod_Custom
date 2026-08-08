#pragma once

// 音频专项性能诊断必须通过编译期开关控制，验证完成后不让计时、统计和长日志进入正式固件。
// platformio.ini 会显式覆盖这些默认值；保留默认值便于其它构建系统直接编译。
#ifndef APP_DIAG_FLAC_PERFORMANCE
#define APP_DIAG_FLAC_PERFORMANCE 0
#endif

#ifndef APP_DIAG_MP3_PERFORMANCE
#define APP_DIAG_MP3_PERFORMANCE 0
#endif
