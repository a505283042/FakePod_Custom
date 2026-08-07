#pragma once

#include <stdint.h>

#include "esp_err.h"


// 初始化 CST820 触摸芯片
esp_err_t cst820_init();


// 获取芯片 ID
uint8_t cst820_get_chip_id();


// 判断 CST820 是否初始化成功
bool cst820_is_ready();