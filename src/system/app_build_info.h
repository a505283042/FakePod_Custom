#pragma once

// 单一固件版本入口。运行期模块不再各自打印历史阶段号；
// 正式启动只由 Boot 层输出一次版本与诊断档位。
#define FAKEPOD_FIRMWARE_VERSION "P1.5.3.2R.40.5.1"
#define FAKEPOD_FIRMWARE_PHASE   "video-runtime-log-cleanup"
