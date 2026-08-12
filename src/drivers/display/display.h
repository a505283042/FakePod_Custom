#pragma once

#include <stdbool.h>
#include <stddef.h>
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


// P1.5.3.2R.36.4：统一 Bounded SPI 传输。
// R.36.3 先由 Launcher 实机验证；R.36.4 将主页 Cover Present 也迁入同一有界底层。
// 不经过 esp_lcd_panel_io_tx_param()/tx_color()，因此不触发其内部 portMAX_DELAY
// acquire/recycle；直接复用 Panel IO 已创建的 SPI device，在同一 LVGL Task 中先有界回收
// Panel IO 已提交事务，再以 transaction address phase 发送 CO5300 32-bit QSPI header。
// 每个 queue/get 都有 deadline；可安全回收的失败会熔断本次启动的 BoundedSPI 快路径并回退 LVGL；
// 已入队 descriptor 若在恢复 deadline 内仍无法回收，则受控重启，避免 DMA UAF/脏结果队列再次卡死 UI。
struct DisplayBoundedSpiStats
{
    uint32_t generation = 0U;
    uint32_t total_us = 0U;
    uint32_t panel_drain_us = 0U;
    uint32_t window_setup_us = 0U;
    uint32_t te_wait_us = 0U;
    uint32_t byte_swap_us = 0U;
    uint32_t copy_us = 0U;
    uint32_t producer_us = 0U;
    uint32_t stream_us = 0U;
    uint32_t queue_wait_us = 0U;
    uint32_t first_sequence = 0U;
    uint32_t last_sequence = 0U;
    uint16_t chunks = 0U;
    uint16_t staging_rows = 0U;
    uint8_t staging_buffers = 0U;
    uint32_t staging_total_bytes = 0U;
    bool te_aligned = false;
    bool wire_order = false;
};

bool display_launcher_bounded_spi_available();
esp_err_t display_launcher_bounded_spi_session_begin();
void display_launcher_bounded_spi_session_end();
bool display_launcher_bounded_spi_session_active();

// producer 输出的字节序由 present_stream() 的 wire_order 明确声明；true 时 display 层
// 不再扫描 staging 做 byte swap，可让上层 compositor 直接生成 CO5300 SPI wire-order。
using DisplayBoundedSpiStripProducer = esp_err_t (*)(
    void *context,
    uint16_t source_y,
    uint16_t rows,
    uint16_t width,
    uint8_t *dst_rgb565,
    size_t dst_bytes);

esp_err_t display_launcher_bounded_spi_present_stream(
    DisplayBoundedSpiStripProducer producer,
    void *producer_context,
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    bool wait_for_te,
    DisplayBoundedSpiStats *out_stats = nullptr);

esp_err_t display_launcher_bounded_spi_present(
    const uint8_t *rgb565,
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    bool wait_for_te,
    DisplayBoundedSpiStats *out_stats = nullptr);

// R.36.4：主页封面一次性 Bounded Present。
// 内部严格执行 session begin -> full-screen present -> session end；若 Launcher 已持有 session、
// staging 内存不足或 BoundedSPI 已熔断，立即返回错误，由上层保持旧图/回退 LVGL。
esp_err_t display_cover_bounded_spi_present(
    const uint8_t *rgb565,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    DisplayBoundedSpiStats *out_stats = nullptr);

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

// P1.5.3.2R.36.2：DirectPresent 全路径已移除 esp_lcd_panel_io_tx_param(-1) 无超时 barrier。
// owned 入口继续表示 Launcher/HomeResume 这类同一 LVGL Task 内的连续/恢复提交；底层与普通入口
// 都只依赖有界 color-done 等待，任何异常都会熔断本次启动的 Direct 快路径并回退 LVGL。
esp_err_t display_present_rgb565_region_direct_owned(
    const uint8_t *rgb565,
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    bool wait_for_te,
    DisplayDirectPresentStats *out_stats = nullptr);
