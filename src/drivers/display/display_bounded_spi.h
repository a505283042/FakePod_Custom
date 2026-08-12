#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 仅供 UI 大面积物理提交快路径使用；普通显示/LVGL 模块只应依赖 display.h。
// 统一 Bounded SPI 传输。
// Launcher 与主页 Cover Present 共用同一有界传输底层。
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

// 主页封面一次性 Bounded Present。
// 内部严格执行 session begin -> full-screen present -> session end；若 Launcher 已持有 session、
// staging 内存不足或 BoundedSPI 已熔断，立即返回错误，由上层保持旧图/回退 LVGL。
esp_err_t display_cover_bounded_spi_present(
    const uint8_t *rgb565,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    DisplayBoundedSpiStats *out_stats = nullptr);
