#pragma once

#include <stdint.h>

#include "esp_err.h"


// CST820 单点触摸数据
struct CST820Point
{
    bool pressed;
    uint16_t x;
    uint16_t y;
    uint8_t gesture;
};


// 初始化 CST820 触摸芯片
esp_err_t cst820_init();


// 获取芯片 ID
uint8_t cst820_get_chip_id();


// 判断 CST820 是否初始化成功
bool cst820_is_ready();


// 读取当前触摸状态和原始坐标
esp_err_t cst820_read_point(CST820Point *point);