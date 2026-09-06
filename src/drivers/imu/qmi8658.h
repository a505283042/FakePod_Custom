#pragma once

#include <stdint.h>

#include "esp_err.h"


struct Qmi8658AccelSample
{
    int16_t raw_x;
    int16_t raw_y;
    int16_t raw_z;
    int16_t mg_x;
    int16_t mg_y;
    int16_t mg_z;
};


struct Qmi8658GyroSample
{
    int16_t raw_x;
    int16_t raw_y;
    int16_t raw_z;
    int32_t mdps_x;
    int32_t mdps_y;
    int32_t mdps_z;
};


struct Qmi8658TapEvent
{
    uint8_t count;      // 0=无，1=单击，2=双击
    uint8_t axis;       // 1=X，2=Y，3=Z
    bool negative;
};


// 初始化 QMI8658 基础设备句柄，只验证 WHO_AM_I / REVISION。
esp_err_t qmi8658_init();

// Motion Controls V2：开启 Accelerometer（±4g）+ Gyroscope（±512dps），
// 同时配置硬件 Tap Engine；Tap Activity 继续路由到 INT1/GPIO47。
esp_err_t qmi8658_configure_motion_profile();

// 读取最新三轴加速度；Motion profile 下按 ±4g 换算为 mg。
esp_err_t qmi8658_read_accel(Qmi8658AccelSample *out_sample);

// 一次 burst 同步读取 Accel + Gyro；Gyro 按 ±512dps 换算为 mdps。
esp_err_t qmi8658_read_motion(
    Qmi8658AccelSample *out_accel,
    Qmi8658GyroSample *out_gyro);

// 读取并清除 STATUS1 活动中断；如为 Tap，同步返回 TAP_STATUS。
esp_err_t qmi8658_read_tap_event(Qmi8658TapEvent *out_event);

// 获取 WHO_AM_I
uint8_t qmi8658_get_device_id();

// 获取芯片版本号
uint8_t qmi8658_get_revision();

// 判断 IMU 是否已经初始化
bool qmi8658_is_ready();
