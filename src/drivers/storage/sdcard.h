#pragma once

#include <stdint.h>

#include "esp_err.h"


// 初始化并挂载 TF 卡
esp_err_t sdcard_init();


// 判断 TF 卡是否已经成功挂载
bool sdcard_is_mounted();


// 获取 TF 卡容量，单位 MB
uint32_t sdcard_get_capacity_mb();


// 调试用：打印根目录内容
void sdcard_debug_list_root();