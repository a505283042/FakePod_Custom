#pragma once

// FakePod 运行期诊断总开关。
// 正式固件默认关闭高频/长格式 Trace；需要专项回归时由 platformio.ini 显式改为 1。
// 这些开关只影响日志和诊断采样，不改变播放器状态机、解码参数或硬件时序。

#ifndef APP_DIAG_AUDIO_RAM
#define APP_DIAG_AUDIO_RAM 0
#endif

#ifndef APP_DIAG_AUDIO_POP
#define APP_DIAG_AUDIO_POP 0
#endif

#ifndef APP_DIAG_AUDIO_CLOCK
#define APP_DIAG_AUDIO_CLOCK 0
#endif

#ifndef APP_DIAG_AUDIO_SOURCE
#define APP_DIAG_AUDIO_SOURCE 0
#endif

#ifndef APP_DIAG_AUDIO_INDEX
#define APP_DIAG_AUDIO_INDEX 0
#endif

#ifndef APP_DIAG_AUDIO_WORKSPACE
#define APP_DIAG_AUDIO_WORKSPACE 0
#endif

#ifndef APP_DIAG_TRANSPORT_INTENT
#define APP_DIAG_TRANSPORT_INTENT 0
#endif

#ifndef APP_DIAG_AUDIO_SEEK
#define APP_DIAG_AUDIO_SEEK 0
#endif

#ifndef APP_DIAG_AUDIO_CODEC
#define APP_DIAG_AUDIO_CODEC 0
#endif

#ifndef APP_DIAG_LIBRARY_METADATA
#define APP_DIAG_LIBRARY_METADATA 0
#endif

#ifndef APP_DIAG_LIBRARY_ITEMS
#define APP_DIAG_LIBRARY_ITEMS 0
#endif

#ifndef APP_DIAG_SYSTEM_HEARTBEAT
#define APP_DIAG_SYSTEM_HEARTBEAT 0
#endif

#ifndef APP_DIAG_PLAYER_PLAYLIST
#define APP_DIAG_PLAYER_PLAYLIST 0
#endif

#ifndef APP_DIAG_PLAYER_TRANSPORT
#define APP_DIAG_PLAYER_TRANSPORT 0
#endif

// 既有性能专项开关继续归入同一个总配置头，保持旧 include 兼容。
#ifndef APP_DIAG_FLAC_PERFORMANCE
#define APP_DIAG_FLAC_PERFORMANCE 0
#endif

#ifndef APP_DIAG_MP3_PERFORMANCE
#define APP_DIAG_MP3_PERFORMANCE 0
#endif
