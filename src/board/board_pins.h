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