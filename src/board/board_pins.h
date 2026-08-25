#pragma once

// ============================================================
// FakePod Nano 硬件引脚定义
// ============================================================


// ============================================================
// I2C 总线
// ============================================================

#define FAKEPOD_I2C_SCL        2
#define FAKEPOD_I2C_SDA        3


// ============================================================
// I2C 设备地址
// ============================================================

#define FAKEPOD_ADDR_CST820    0x15
#define FAKEPOD_ADDR_QMI8658   0x6B
#define FAKEPOD_ADDR_CS43131    0x30


// ============================================================
// CST820 触摸控制
// ============================================================

#define FAKEPOD_TOUCH_INT      4
#define FAKEPOD_TOUCH_RST      5


// ============================================================
// CO5300 AMOLED
// 2.0 英寸 460 × 460
// QSPI 接口
// ============================================================

#define FAKEPOD_LCD_TE         6
#define FAKEPOD_LCD_RST        7
#define FAKEPOD_LCD_CS         8
#define FAKEPOD_LCD_CLK        9

#define FAKEPOD_LCD_D0         10
#define FAKEPOD_LCD_D1         11
#define FAKEPOD_LCD_D2         12
#define FAKEPOD_LCD_D3         13

#define FAKEPOD_LCD_WIDTH      460
#define FAKEPOD_LCD_HEIGHT     460

// 这块屏实际显存窗口从 X=10 开始
#define FAKEPOD_LCD_X_OFFSET   10
#define FAKEPOD_LCD_Y_OFFSET   0


// ============================================================
// CS43131 音频 DAC
// ESP32-S3 经过 1.8V 电平转换后连接到 CS43131 ASP1。
// MCLK 由板载 24.576MHz 晶振直接提供，不占用 ESP32 GPIO。
// ============================================================

#define FAKEPOD_I2S_LRCK       38
#define FAKEPOD_I2S_DOUT       39
#define FAKEPOD_I2S_BCLK       40
#define FAKEPOD_DAC_RST        41

// ============================================================
// 硬件电源键 / EC190707
// K2 经 D1 隔离后接入 GPIO48(BTN)，用于在约3秒硬关电源前捕获关机意图。
// 短按 (<500ms释放) 作为音量加。
// ============================================================
#define FAKEPOD_POWER_KEY      48

// ============================================================
// 辅助功能键 K1 / GPIO0
// 原理图：GPIO0 经 R15(10kΩ) 上拉至 3V3；K1 按下时 GPIO0 短接至 GND。
// Strapping 脚安全：外部强上拉保证启动时 GPIO0 默认高电平（正常 Boot）。
// 档位（释放时分级，仅在稳定按下才开始计时）：
//     <300ms 释放 → 音量 -1
//   300~1500ms 释放 → 锁屏 / 解锁（如屏在 AOD/熄屏则先唤醒回正常）
//  1500~3000ms 释放 → 进入 AOD（AMOLED 息屏显示，自动锁定）
//      ≥3000ms 释放 → 真·熄屏 brightness=0；
//                       按住超过 3s 继续保持时每 1s 在 AOD↔熄屏 间翻转
// ============================================================
#define FAKEPOD_AUX_KEY         0

// ============================================================
// TF 卡 - SDMMC 4-bit
// ============================================================
//
// DAT2 -> GPIO21
// DAT3 -> GPIO18
// CMD  -> GPIO17
// CLK  -> GPIO16
// DAT0 -> GPIO15
// DAT1 -> GPIO14
//
// ============================================================

#define FAKEPOD_SD_CLK         16
#define FAKEPOD_SD_CMD         17

#define FAKEPOD_SD_D0          15
#define FAKEPOD_SD_D1          14
#define FAKEPOD_SD_D2          21
#define FAKEPOD_SD_D3          18