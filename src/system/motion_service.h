#pragma once

#include "esp_err.h"

// QMI8658C Motion Controls V2：
// - 向前翻转 -> 下一首
// - 向后翻转 -> 上一首
// - Double Tap (INT1/GPIO47) -> 播放/暂停
// 只在 Music + Normal亮屏 + Unlocked 时执行播放器动作；IMU 采样本身不访问 TF。
esp_err_t motion_service_init();
bool motion_service_is_ready();

// system_loop 每轮调用；Accel+Gyro burst 读取限频到100Hz，INT1 只在普通任务上下文读取/清状态。
void motion_service_update();
