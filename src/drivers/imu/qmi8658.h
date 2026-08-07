#pragma once

#include <stdint.h>

#include "esp_err.h"


// 初始化 QMI8658
esp_err_t qmi8658_init();


// 获取 WHO_AM_I
uint8_t qmi8658_get_device_id();


// 获取芯片版本号
uint8_t qmi8658_get_revision();


// 判断 IMU 是否已经初始化
bool qmi8658_is_ready();