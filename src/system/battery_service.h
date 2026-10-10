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

// 最近一次显示电量从 >50% 下降至 <=50% 时的实测记录，存于 NVS，重启后仍可读取。
struct BatteryHalfRecord
{
    uint16_t measured_mv;  // ADC 校准、板级分压换算后的电池端电压；非 LUT 反推
    uint16_t filtered_mv;  // 当时用于屏幕百分比计算的滤波电压
    uint8_t percent;       // 记录当时实际显示的百分比
};
bool battery_service_get_half_record(BatteryHalfRecord *out_record);
