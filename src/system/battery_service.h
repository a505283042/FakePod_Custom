#pragma once

#include <stdint.h>

#include "esp_err.h"

struct BatterySnapshot
{
    bool valid;
    bool calibrated;
    uint16_t raw_average;
    uint16_t raw_min;
    uint16_t raw_max;
    uint16_t adc_mv;
    uint16_t battery_mv;
    uint16_t filtered_mv;
    uint8_t percent;
    uint32_t sequence;
};

// BatteryService V1：GPIO1 / ADC1_CH0，BAT+ 经 10k/10k 1:2 分压。
// 负责校准采样、滤波与SOC估算；UI只读取快照，RELEASE默认不输出周期遥测。
esp_err_t battery_service_init();
bool battery_service_is_ready();

// system_loop 每轮可调用；内部自行限频为每 2 秒一次 32-sample burst。
void battery_service_update();

// POD 快照，供后续电池图标/设置页复用。
bool battery_service_get_snapshot(BatterySnapshot *out_snapshot);
