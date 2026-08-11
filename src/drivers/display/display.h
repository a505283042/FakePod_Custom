#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"


// 初始化 CO5300 AMOLED
esp_err_t display_init();


// 判断显示屏是否初始化完成
bool display_is_ready();


// 执行启动阶段颜色测试
esp_err_t display_test_colors();


// 获取底层 LCD 句柄，供 LVGL 显示适配层使用
esp_lcd_panel_io_handle_t display_get_panel_io();
esp_lcd_panel_handle_t display_get_panel();

// P1.5.3.2R.26：esp_lvgl_port 在 lvgl_port_add_disp() 内会重新注册 Panel IO 的
// on_color_trans_done 回调，从而覆盖 display_init() 阶段的 DMA 完成 semaphore 回调。
// LVGL display 创建完成后调用此接口安装统一桥接：普通 LVGL flush -> flush_ready；
// DirectPresent -> g_tx_done。参数使用 void* 避免底层 display.h 暴露 LVGL 类型依赖。
esp_err_t display_install_lvgl_color_done_bridge(void *lvgl_display);


// P1.5.3.2R.21：CO5300 TE（Tearing Effect）同步。
// GPIO6 由屏幕输出 TE 脉冲；大面积 LVGL 刷新可在开始前等待下一次 TE 上升沿，
// 让 QSPI framebuffer 写入尽量从垂直消隐边界启动。TE 不可用时自动降级，不阻断显示。
bool display_te_is_ready();
bool display_te_wait_next(uint32_t timeout_ms);
uint32_t display_te_get_period_us();

// P1.5.3.2R.22：全屏封面 Present Hold 兼容回退。
// R.23 直接 Surface 提交失败时仍可退回这条路径。
void display_present_request_hold();
bool display_present_take_hold_request();
bool display_present_set_output(bool enabled);
bool display_present_output_is_enabled();

// P1.5.3.2R.29：封面 Direct Surface Present 使用 Continuous GRAM Stream。
// 一次设置完整 CASET/RASET，首块 RAMWR、后续 RAMWRC，并以两块 staging 保持两笔 color
// transaction 流水；wire_order=true 时继续使用 R.28 预交换 Surface，native 仍可在线 swap 回退。
struct DisplayDirectPresentStats
{
    uint32_t total_us = 0U;
    uint32_t io_barrier_us = 0U;
    uint32_t window_setup_us = 0U;
    uint32_t te_wait_us = 0U;
    uint32_t byte_swap_us = 0U;
    uint32_t copy_us = 0U;
    uint32_t dma_us = 0U;
    uint32_t dma_wait_us = 0U;
    uint32_t pipeline_us = 0U;
    uint32_t stream_us = 0U;
    uint32_t overlap_saved_us = 0U;
    uint16_t chunks = 0U;
    uint16_t staging_rows = 0U;
    uint8_t staging_buffers = 0U;
    uint32_t staging_bytes = 0U;
    uint32_t staging_total_bytes = 0U;
    uint8_t queue_peak = 0U;
    bool te_aligned = false;
    bool wire_order = false;
    bool continuous_stream = false;
};

esp_err_t display_present_rgb565_direct(
    const uint8_t *rgb565,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    DisplayDirectPresentStats *out_stats = nullptr);

// P1.5.3.2R.33.2：Launcher 等局部静态 RGB565 帧复用 R.29 Continuous GRAM，
// 但窗口可位于屏幕任意区域。连续动画可关闭 TE 等待，避免每个离散帧额外等待一轮 VBlank。
esp_err_t display_present_rgb565_region_direct(
    const uint8_t *rgb565,
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    bool wait_for_te,
    DisplayDirectPresentStats *out_stats = nullptr);
