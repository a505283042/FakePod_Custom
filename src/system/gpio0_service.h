#pragma once

#include "esp_err.h"

// ============================================================
// GPIO0 辅助按键服务（GPIO0 = FAKEPOD_AUX_KEY）
//
// 采用轮询 + 软件去抖方式（与 power_service 同架构，不使用 ISR）。
// 在 system_loop_update() 末尾调用 gpio0_service_update()。
//
// 档位使用「释放时分级」：一旦达到更高档位，低档位自动作废。
//  ・<300ms 释放         → 音量 -1
//  ・300~1500ms 释放     → 锁屏 / 解锁（如屏在 AOD/熄屏 先唤醒到 Normal）
//  ・1500~3000ms 释放    → 进入 AOD（AMOLED 息屏显示，自动锁定）
//  ・≥3000ms 后，每过 1s → 在 AOD ↔ 熄屏 之间翻转（持续按住时）
//
// 说明：「释放时分级」避免在按住期间瞬时执行造成困扰。
//       只有「3000ms 后按住持续翻转」是按住实时执行，方便用户快速在 AOD/熄屏间切换。
// ============================================================

esp_err_t gpio0_service_init(void);
void      gpio0_service_update(void);
