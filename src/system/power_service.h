#pragma once

#include "esp_err.h"

// GPIO48 与 EC190707 硬件电源键共用物理 K2。
// V1.1 根据实机 Brownout 时序，在硬件约 2 秒断电前提前识别长按并触发显式 NVS flush。
// 不在 ISR 中访问 NVS，也不提前关闭 Audio/Display；先用实机核定真实硬断电时序。
esp_err_t power_service_init();

// READY 后每轮调用。10ms system_loop 轮询足以覆盖人机电源键时序，避免额外 ISR 链。
void power_service_update();
