#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"


// 初始化 FakePod 的 I2C0 总线
esp_err_t i2c_bus_init();


// 扫描总线上的所有 I2C 设备
void i2c_bus_scan();


// 向总线注册一个设备
esp_err_t i2c_bus_add_device(
    uint8_t address,
    i2c_master_dev_handle_t *handle
);


// 读取单字节寄存器
esp_err_t i2c_bus_read_reg8(
    i2c_master_dev_handle_t device,
    uint8_t reg,
    uint8_t *value
);


// 写入单字节寄存器
esp_err_t i2c_bus_write_reg8(
    i2c_master_dev_handle_t device,
    uint8_t reg,
    uint8_t value
);