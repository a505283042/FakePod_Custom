#pragma once

#include <stddef.h>
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

// 按指定总线速度注册 I2C 设备
esp_err_t i2c_bus_add_device_at_speed(
    uint8_t address,
    uint32_t scl_speed_hz,
    i2c_master_dev_handle_t *handle
);

// 注销已经注册的 I2C 设备；成功后把调用方句柄清空。
esp_err_t i2c_bus_remove_device(
    i2c_master_dev_handle_t *handle
);

// 探测指定 7 位 I2C 地址是否有设备响应
esp_err_t i2c_bus_probe_address(
    uint8_t address,
    int timeout_ms
);


// 读取单字节寄存器
esp_err_t i2c_bus_read_reg8(
    i2c_master_dev_handle_t device,
    uint8_t reg,
    uint8_t *value
);


// 从指定寄存器开始连续读取多个字节
esp_err_t i2c_bus_read_bytes(
    i2c_master_dev_handle_t device,
    uint8_t reg,
    uint8_t *data,
    size_t length
);


// 写入单字节寄存器
esp_err_t i2c_bus_write_reg8(
    i2c_master_dev_handle_t device,
    uint8_t reg,
    uint8_t value
);