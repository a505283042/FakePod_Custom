#include "display.h"
#include "display_backend.h"
#include "display_bounded_spi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "driver/spi_master.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_diag_config.h"
#include "board_pins.h"

static const char *TAG = "显示";

// ============================================================
// P1.5.3.2R.36.4 Bounded SPI Transport
// ============================================================
//
// 旧 Launcher PanelIO DirectScene 与 R.29 Cover ContinuousGRAM 都通过 esp_lcd_panel_io_tx_param()/
// tx_color() 与 LVGL 共享 Panel IO。ESP-IDF 5.5 的 SPI Panel IO 在这些入口内部存在
// portMAX_DELAY 的 bus acquire/result recycle：Launcher 已由 R.36.3 迁移后通过压力测试；
// R.36.4 继续迁移 Cover，彻底移除主页换封面对旧 ContinuousGRAM 的运行时依赖。
//
// 本模块不再调用上述 Panel IO 发送入口。项目固定 ESP-IDF 5.5，因此使用官方源中可验证的
// esp_lcd_panel_io_spi_t 前缀布局取得其唯一 spi_device_handle_t 和 num_trans_inflight：
// 1) 在同一个 LVGL Task 内先用有限 get_trans_result() 回收 Panel IO 已提交事务；
// 2) 再直接向同一个 spi_device queue 本模块自己的 raw descriptor；
// 3) 每个 queue/get 都有 deadline，正常/异常路径都必须回收 descriptor；
// 4) 如果已入队 descriptor 在固定恢复窗口仍拿不回来，执行受控重启，绝不把脏 result queue
//    重新交给 LVGL，也绝不释放仍可能被 DMA 引用的 staging。
//
// CO5300 QSPI header 通过 32-bit address phase 发送：
//   0x02 00 <DCS> 00 + 单线参数
//   0x32 00 <DCS> 00 + QIO RGB565
// address phase 保持单线，只有 RGB data phase 设置 SPI_TRANS_MODE_QIO；无需
// SPI_TRANS_CS_KEEP_ACTIVE，因此完全不调用当前 IDF 只支持 portMAX_DELAY 的 acquire_bus()。

struct DisplayPanelIoSpiV55Prefix
{
    esp_lcd_panel_io_t base;
    spi_device_handle_t spi_dev;
    size_t spi_trans_max_bytes;
    int dc_gpio_num;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
    size_t queue_size;
    size_t num_trans_inflight;
    int lcd_cmd_bits;
    int lcd_param_bits;
};

// SPI driver 在 SPI_TRANS_VARIABLE_ADDR 下会把 spi_transaction_t 后的前三字节解释为
// command_bits/address_bits/dummy_bits；esp_lcd SPI device 的 post-callback 又会把同一尾部
// 解释为自己的 32-bit flags。这里显式保留4字节 tail：byte0=0 让 callback 的低2位恒为0，
// byte1=32 给 SPI driver 提供32-bit address/header，byte2/3=0。项目固定 IDF 5.5 + ESP32-S3。
struct DisplayBoundedRawSpiTransaction
{
    spi_transaction_t base = {};
    uint8_t command_bits = 0U;
    uint8_t address_bits = 0U;
    uint8_t dummy_bits = 0U;
    uint8_t callback_flags_guard = 0U;
};

static_assert(offsetof(DisplayBoundedRawSpiTransaction, command_bits) == sizeof(spi_transaction_t),
    "R.36.3 raw SPI descriptor tail must immediately follow spi_transaction_t");

struct DisplayBoundedSpiState
{
    bool active = false;
    bool faulted = false;
    const char *owner = "none";
    uint32_t generation = 0U;
    uint32_t next_sequence = 1U;
    uint8_t *dma_strip[2] = {nullptr, nullptr};
    uint16_t staging_rows = 0U;
    size_t staging_bytes = 0U;
    // control_tx 同样常驻，确保 CASET/RASET 若发生 result timeout，descriptor 生命周期
    // 不会随着函数栈退出而失效。color_tx 与 DMA staging 共同覆盖整个 session。
    DisplayBoundedRawSpiTransaction control_tx[2] = {};
    DisplayBoundedRawSpiTransaction color_tx[2] = {};
    uint32_t color_sequence[2] = {0U, 0U};
};

static DisplayBoundedSpiState g_bounded_spi = {};

static constexpr uint16_t BOUNDED_SPI_STAGING_ROWS[] = {16U, 12U, 8U, 4U};
static constexpr uint32_t BOUNDED_SPI_PANEL_DRAIN_TIMEOUT_MS = 80U;
static constexpr uint32_t BOUNDED_SPI_QUEUE_TIMEOUT_MS = 20U;
static constexpr uint32_t BOUNDED_SPI_RESULT_TIMEOUT_MS = 80U;
static constexpr uint32_t BOUNDED_SPI_RECOVERY_TIMEOUT_MS = 120U;
static constexpr uint32_t CO5300_QSPI_OPCODE_WRITE_CMD = 0x02U;
static constexpr uint32_t CO5300_QSPI_OPCODE_WRITE_COLOR = 0x32U;

static constexpr uint32_t display_bounded_qspi_header(uint32_t opcode, uint8_t command)
{
    return (opcode << 24U) | (static_cast<uint32_t>(command) << 8U);
}

static DisplayPanelIoSpiV55Prefix *display_panel_io_spi_v55_prefix()
{
#if ESP_IDF_VERSION_MAJOR == 5 && ESP_IDF_VERSION_MINOR == 5
    return reinterpret_cast<DisplayPanelIoSpiV55Prefix *>(display_get_panel_io());
#else
    return nullptr;
#endif
}

static bool display_bounded_spi_abi_valid(DisplayPanelIoSpiV55Prefix *io)
{
    // 当前板卡是 CO5300 QSPI：无独立 D/C、32-bit QSPI header、8-bit 参数。
    // 只有这组已验证配置才允许触碰 IDF 5.5 的 Panel IO 私有前缀。
    return io != nullptr && io->spi_dev != nullptr && io->dc_gpio_num < 0 &&
        io->lcd_cmd_bits == 32 && io->lcd_param_bits == 8 && io->queue_size >= 2U &&
        io->spi_trans_max_bytes > 0U;
}

static bool display_bounded_spi_available_internal()
{
    if (!display_is_ready() || display_get_panel_io() == nullptr || g_bounded_spi.faulted) {
        return false;
    }
    return display_bounded_spi_abi_valid(display_panel_io_spi_v55_prefix());
}

bool display_launcher_bounded_spi_available()
{
    return display_bounded_spi_available_internal();
}

static esp_err_t display_bounded_spi_drain_panel(
    DisplayPanelIoSpiV55Prefix *io,
    uint32_t *elapsed_us)
{
    if (elapsed_us != nullptr) {
        *elapsed_us = 0U;
    }
    if (!display_bounded_spi_abi_valid(io)) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t started_us = esp_timer_get_time();
    const size_t pending = io->num_trans_inflight;
    for (size_t i = 0U; i < pending; ++i) {
        spi_transaction_t *completed = nullptr;
        esp_err_t ret = spi_device_get_trans_result(
            io->spi_dev,
            &completed,
            pdMS_TO_TICKS(BOUNDED_SPI_PANEL_DRAIN_TIMEOUT_MS));
        if (ret != ESP_OK) {
            // 已存在的 Panel IO descriptor 若连固定恢复窗口都拿不回来，继续走 LVGL 只会让
            // 下一次 panel_io tx_* 再次落入其内部 portMAX_DELAY。受控重启优于永久 UI 死锁。
            const int64_t deadline_us = esp_timer_get_time() +
                static_cast<int64_t>(BOUNDED_SPI_RECOVERY_TIMEOUT_MS) * 1000LL;
            while (ret != ESP_OK && esp_timer_get_time() < deadline_us) {
                completed = nullptr;
                ret = spi_device_get_trans_result(io->spi_dev, &completed, pdMS_TO_TICKS(10));
            }
            if (ret != ESP_OK) {
                ESP_LOGE(TAG,
                    "R.36.4 BoundedSPI：Panel IO事务%ums内未回收 pending=%u done=%u；受控重启避免回到无界Panel IO",
                    static_cast<unsigned>(BOUNDED_SPI_PANEL_DRAIN_TIMEOUT_MS + BOUNDED_SPI_RECOVERY_TIMEOUT_MS),
                    static_cast<unsigned>(pending),
                    static_cast<unsigned>(i));
                esp_restart();
            }
            ESP_LOGW(TAG,
                "R.36.4 BoundedSPI：Panel IO事务在恢复窗口完成 pending=%u done=%u；本帧继续",
                static_cast<unsigned>(pending),
                static_cast<unsigned>(i + 1U));
        }
        if (io->num_trans_inflight > 0U) {
            --io->num_trans_inflight;
        }
    }
    if (elapsed_us != nullptr) {
        *elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
    }
    return ESP_OK;
}

static void display_bounded_spi_prepare_transaction(
    DisplayBoundedRawSpiTransaction *tx,
    DisplayPanelIoSpiV55Prefix *io,
    uint32_t qspi_header,
    bool qio_data)
{
    memset(tx, 0, sizeof(*tx));
    tx->base.flags = SPI_TRANS_VARIABLE_ADDR;
    if (qio_data) {
        // 只让 data phase 走4线；32-bit address/header phase 仍保持单线，匹配 CO5300 官方驱动。
        tx->base.flags |= SPI_TRANS_MODE_QIO;
    }
    tx->base.addr = static_cast<uint64_t>(qspi_header);
    // Panel IO pre/post callback 的 user 仍需指向其内部对象；callback_flags_guard 让其私有
    // en_trans_done_cb 位恒为0，因此 raw transaction 不会误触 LVGL color-done bridge。
    tx->base.user = io;
    tx->command_bits = 0U;
    tx->address_bits = 32U;
    tx->dummy_bits = 0U;
    tx->callback_flags_guard = 0U;
}

static esp_err_t display_bounded_spi_queue_wait_single(
    DisplayPanelIoSpiV55Prefix *io,
    DisplayBoundedRawSpiTransaction *tx,
    uint32_t *queue_wait_us)
{
    const int64_t started_us = esp_timer_get_time();
    esp_err_t ret = spi_device_queue_trans(
        io->spi_dev,
        &tx->base,
        pdMS_TO_TICKS(BOUNDED_SPI_QUEUE_TIMEOUT_MS));
    if (ret != ESP_OK) {
        return ret;
    }

    spi_transaction_t *completed = nullptr;
    ret = spi_device_get_trans_result(
        io->spi_dev,
        &completed,
        pdMS_TO_TICKS(BOUNDED_SPI_RESULT_TIMEOUT_MS));
    if (queue_wait_us != nullptr) {
        *queue_wait_us += static_cast<uint32_t>(esp_timer_get_time() - started_us);
    }
    if (ret == ESP_OK) {
        if (completed != &tx->base) {
            ESP_LOGE(TAG,
                "R.36.4 BoundedSPI：事务身份错位 expected=%p actual=%p；结果队列已失配，受控重启",
                static_cast<void *>(&tx->base),
                static_cast<void *>(completed));
            esp_restart();
        }
        return ESP_OK;
    }

    // queue 成功后 descriptor 已归 SPI driver 管理，不能直接退出并让上层继续 LVGL。
    // 再给固定 recovery window；如果仍拿不回 descriptor，则受控重启避免脏 result queue。
    const int64_t deadline_us = esp_timer_get_time() +
        static_cast<int64_t>(BOUNDED_SPI_RECOVERY_TIMEOUT_MS) * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        completed = nullptr;
        const esp_err_t recovery_ret = spi_device_get_trans_result(
            io->spi_dev, &completed, pdMS_TO_TICKS(10));
        if (recovery_ret == ESP_OK) {
            if (completed != &tx->base) {
                ESP_LOGE(TAG,
                    "R.36.4 BoundedSPI：恢复阶段事务身份错位 expected=%p actual=%p，受控重启",
                    static_cast<void *>(&tx->base),
                    static_cast<void *>(completed));
                esp_restart();
            }
            return ret;
        }
    }

    ESP_LOGE(TAG,
        "R.36.4 BoundedSPI：control transaction %ums内未回收，受控重启避免Panel IO结果队列污染",
        static_cast<unsigned>(BOUNDED_SPI_RESULT_TIMEOUT_MS + BOUNDED_SPI_RECOVERY_TIMEOUT_MS));
    esp_restart();
    return ESP_ERR_TIMEOUT;
}

static esp_err_t display_bounded_spi_send_window_param(
    DisplayPanelIoSpiV55Prefix *io,
    uint8_t control_index,
    uint8_t dcs_command,
    const uint8_t param[4],
    uint32_t *queue_wait_us)
{
    DisplayBoundedRawSpiTransaction &tx = g_bounded_spi.control_tx[control_index & 1U];
    display_bounded_spi_prepare_transaction(
        &tx,
        io,
        display_bounded_qspi_header(CO5300_QSPI_OPCODE_WRITE_CMD, dcs_command),
        false);
    tx.base.flags |= SPI_TRANS_USE_TXDATA;
    tx.base.length = 32U;
    memcpy(tx.base.tx_data, param, 4U);
    return display_bounded_spi_queue_wait_single(io, &tx, queue_wait_us);
}

static void display_bounded_spi_poison(const char *reason)
{
    if (!g_bounded_spi.faulted) {
        ESP_LOGE(TAG,
            "R.36.4 BoundedSPI熔断：owner=%s reason=%s；本次启动后续BoundedSPI固定回退LVGL",
            g_bounded_spi.owner != nullptr ? g_bounded_spi.owner : "none",
            reason != nullptr ? reason : "unknown");
    }
    g_bounded_spi.faulted = true;
}

static esp_err_t display_bounded_spi_session_begin(const char *owner)
{
    if (!display_bounded_spi_available_internal() || g_bounded_spi.active) {
        return ESP_ERR_INVALID_STATE;
    }

    DisplayPanelIoSpiV55Prefix *io = display_panel_io_spi_v55_prefix();
    if (!display_bounded_spi_abi_valid(io)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    g_bounded_spi.staging_rows = 0U;
    g_bounded_spi.staging_bytes = 0U;
    for (uint16_t candidate_rows : BOUNDED_SPI_STAGING_ROWS) {
        const size_t bytes =
            static_cast<size_t>(FAKEPOD_LCD_WIDTH) * static_cast<size_t>(candidate_rows) * 2U;
        if (bytes > io->spi_trans_max_bytes) {
            continue;
        }
        g_bounded_spi.dma_strip[0] = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16U, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (g_bounded_spi.dma_strip[0] == nullptr) {
            continue;
        }
        g_bounded_spi.dma_strip[1] = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16U, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (g_bounded_spi.dma_strip[1] != nullptr) {
            g_bounded_spi.staging_rows = candidate_rows;
            g_bounded_spi.staging_bytes = bytes;
            break;
        }
        heap_caps_free(g_bounded_spi.dma_strip[0]);
        g_bounded_spi.dma_strip[0] = nullptr;
    }

    if (g_bounded_spi.dma_strip[0] == nullptr ||
        g_bounded_spi.dma_strip[1] == nullptr ||
        g_bounded_spi.staging_rows == 0U) {
        if (g_bounded_spi.dma_strip[0] != nullptr) {
            heap_caps_free(g_bounded_spi.dma_strip[0]);
        }
        if (g_bounded_spi.dma_strip[1] != nullptr) {
            heap_caps_free(g_bounded_spi.dma_strip[1]);
        }
        g_bounded_spi.dma_strip[0] = nullptr;
        g_bounded_spi.dma_strip[1] = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ++g_bounded_spi.generation;
    if (g_bounded_spi.generation == 0U) {
        ++g_bounded_spi.generation;
    }
    g_bounded_spi.next_sequence = 1U;
    g_bounded_spi.owner = owner != nullptr ? owner : "unknown";
    g_bounded_spi.active = true;
#if APP_DIAG_DISPLAY_TRANSPORT
    ESP_LOGI(TAG,
        "BoundedSPI BEGIN：owner=%s gen=%u staging=%u行×2 total=%uB DMAfree=%u",
        g_bounded_spi.owner,
        static_cast<unsigned>(g_bounded_spi.generation),
        static_cast<unsigned>(g_bounded_spi.staging_rows),
        static_cast<unsigned>(g_bounded_spi.staging_bytes * 2U),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
#endif
    return ESP_OK;
}

esp_err_t display_launcher_bounded_spi_session_begin()
{
    return display_bounded_spi_session_begin("launcher");
}

bool display_launcher_bounded_spi_session_active()
{
    return g_bounded_spi.active;
}

static void display_bounded_spi_session_end_internal()
{
    if (!g_bounded_spi.active && g_bounded_spi.dma_strip[0] == nullptr &&
        g_bounded_spi.dma_strip[1] == nullptr) {
        return;
    }

#if APP_DIAG_DISPLAY_TRANSPORT
    const uint32_t generation = g_bounded_spi.generation;
    const char *owner = g_bounded_spi.owner != nullptr ? g_bounded_spi.owner : "none";
#endif
    g_bounded_spi.active = false;
    // 能走到 Session END 就意味着本层所有 raw descriptor 已被有界回收；任何未回收路径
    // 都会在 bounded present 内直接受控重启，因此这里可以安全释放。
    if (g_bounded_spi.dma_strip[0] != nullptr) {
        heap_caps_free(g_bounded_spi.dma_strip[0]);
    }
    if (g_bounded_spi.dma_strip[1] != nullptr) {
        heap_caps_free(g_bounded_spi.dma_strip[1]);
    }
    g_bounded_spi.dma_strip[0] = nullptr;
    g_bounded_spi.dma_strip[1] = nullptr;
    g_bounded_spi.staging_rows = 0U;
    g_bounded_spi.staging_bytes = 0U;
    g_bounded_spi.owner = "none";
#if APP_DIAG_DISPLAY_TRANSPORT
    ESP_LOGI(TAG,
        "BoundedSPI END：owner=%s gen=%u faulted=%d DMAfree=%u",
        owner,
        static_cast<unsigned>(generation),
        g_bounded_spi.faulted ? 1 : 0,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
#endif
}

void display_launcher_bounded_spi_session_end()
{
    display_bounded_spi_session_end_internal();
}

static esp_err_t display_bounded_spi_present_internal(
    const uint8_t *rgb565,
    DisplayBoundedSpiStripProducer producer,
    void *producer_context,
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    bool wait_for_te,
    DisplayBoundedSpiStats *out_stats)
{
    DisplayBoundedSpiStats stats = {};
    stats.generation = g_bounded_spi.generation;
    stats.wire_order = wire_order;
    const int64_t total_started_us = esp_timer_get_time();

    if (!g_bounded_spi.active || g_bounded_spi.faulted ||
        ((rgb565 == nullptr) == (producer == nullptr))) {
        return ESP_ERR_INVALID_STATE;
    }
    if (width == 0U || height == 0U || x >= FAKEPOD_LCD_WIDTH || y >= FAKEPOD_LCD_HEIGHT ||
        static_cast<uint32_t>(x) + width > FAKEPOD_LCD_WIDTH ||
        static_cast<uint32_t>(y) + height > FAKEPOD_LCD_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    DisplayPanelIoSpiV55Prefix *io = display_panel_io_spi_v55_prefix();
    if (!display_bounded_spi_abi_valid(io)) {
        display_bounded_spi_poison("Panel IO ABI/配置不匹配");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t result = display_bounded_spi_drain_panel(io, &stats.panel_drain_us);
    if (result != ESP_OK) {
        return result;
    }

    const uint16_t x_start = static_cast<uint16_t>(FAKEPOD_LCD_X_OFFSET + x);
    const uint16_t x_end = static_cast<uint16_t>(FAKEPOD_LCD_X_OFFSET + x + width - 1U);
    const uint16_t y_start = static_cast<uint16_t>(FAKEPOD_LCD_Y_OFFSET + y);
    const uint16_t y_end = static_cast<uint16_t>(FAKEPOD_LCD_Y_OFFSET + y + height - 1U);
    const uint8_t caset[4] = {
        static_cast<uint8_t>(x_start >> 8U), static_cast<uint8_t>(x_start & 0xFFU),
        static_cast<uint8_t>(x_end >> 8U), static_cast<uint8_t>(x_end & 0xFFU)};
    const uint8_t raset[4] = {
        static_cast<uint8_t>(y_start >> 8U), static_cast<uint8_t>(y_start & 0xFFU),
        static_cast<uint8_t>(y_end >> 8U), static_cast<uint8_t>(y_end & 0xFFU)};

    const int64_t window_started_us = esp_timer_get_time();
    result = display_bounded_spi_send_window_param(
        io, 0U, LCD_CMD_CASET, caset, &stats.queue_wait_us);
    if (result == ESP_OK) {
        result = display_bounded_spi_send_window_param(
            io, 1U, LCD_CMD_RASET, raset, &stats.queue_wait_us);
    }
    stats.window_setup_us = static_cast<uint32_t>(esp_timer_get_time() - window_started_us);
    if (result != ESP_OK) {
        display_bounded_spi_poison("CASET/RASET有界事务失败");
        return result;
    }

    if (wait_for_te && display_te_is_ready()) {
        uint32_t timeout_ms = 25U;
        const uint32_t period_us = display_te_get_period_us();
        if (period_us > 0U) {
            const uint32_t period_ms_ceil = (period_us + 999U) / 1000U;
            timeout_ms = period_ms_ceil + 8U;
            if (timeout_ms < 25U) timeout_ms = 25U;
            if (timeout_ms > 50U) timeout_ms = 50U;
        }
        const int64_t te_started_us = esp_timer_get_time();
        stats.te_aligned = display_te_wait_next(timeout_ms);
        stats.te_wait_us = static_cast<uint32_t>(esp_timer_get_time() - te_started_us);
    }

    const uint16_t staging_rows = g_bounded_spi.staging_rows;
    stats.staging_rows = staging_rows;
    stats.staging_buffers = 2U;
    stats.staging_total_bytes = static_cast<uint32_t>(g_bounded_spi.staging_bytes * 2U);

    auto prepare_strip = [&](uint8_t buffer_index, uint16_t source_y, uint16_t rows) -> esp_err_t {
        uint8_t *dst = g_bounded_spi.dma_strip[buffer_index];
        const size_t bytes = static_cast<size_t>(width) * rows * 2U;
        if (producer != nullptr) {
            const int64_t produced_us = esp_timer_get_time();
            const esp_err_t produced = producer(
                producer_context, source_y, rows, width, dst, bytes);
            stats.producer_us += static_cast<uint32_t>(esp_timer_get_time() - produced_us);
            if (produced != ESP_OK) {
                return produced;
            }
            if (!wire_order) {
                const int64_t swapped_us = esp_timer_get_time();
                for (size_t i = 0U; i < bytes; i += 2U) {
                    const uint8_t lo = dst[i];
                    dst[i] = dst[i + 1U];
                    dst[i + 1U] = lo;
                }
                stats.byte_swap_us += static_cast<uint32_t>(esp_timer_get_time() - swapped_us);
            }
            return ESP_OK;
        }

        const uint8_t *src = rgb565 + static_cast<size_t>(source_y) * width * 2U;
        const int64_t started_us = esp_timer_get_time();
        if (wire_order) {
            memcpy(dst, src, bytes);
            stats.copy_us += static_cast<uint32_t>(esp_timer_get_time() - started_us);
            return ESP_OK;
        }
        const size_t pixels = bytes / 2U;
        for (size_t i = 0U; i < pixels; ++i) {
            dst[i * 2U] = src[i * 2U + 1U];
            dst[i * 2U + 1U] = src[i * 2U];
        }
        stats.byte_swap_us += static_cast<uint32_t>(esp_timer_get_time() - started_us);
        return ESP_OK;
    };

    auto queue_color = [&](uint8_t buffer_index, uint8_t dcs_command, uint16_t rows) -> esp_err_t {
        DisplayBoundedRawSpiTransaction &tx = g_bounded_spi.color_tx[buffer_index];
        display_bounded_spi_prepare_transaction(
            &tx,
            io,
            display_bounded_qspi_header(CO5300_QSPI_OPCODE_WRITE_COLOR, dcs_command),
            true);
        const size_t color_bytes = static_cast<size_t>(width) * rows * 2U;
        if (color_bytes > io->spi_trans_max_bytes) {
            return ESP_ERR_INVALID_ARG;
        }
        tx.base.length = color_bytes * 8U;
        tx.base.tx_buffer = g_bounded_spi.dma_strip[buffer_index];
        g_bounded_spi.color_sequence[buffer_index] = g_bounded_spi.next_sequence++;
        if (stats.first_sequence == 0U) {
            stats.first_sequence = g_bounded_spi.color_sequence[buffer_index];
        }
        stats.last_sequence = g_bounded_spi.color_sequence[buffer_index];
        const int64_t queued_us = esp_timer_get_time();
        const esp_err_t ret = spi_device_queue_trans(
            io->spi_dev,
            &tx.base,
            pdMS_TO_TICKS(BOUNDED_SPI_QUEUE_TIMEOUT_MS));
        stats.queue_wait_us += static_cast<uint32_t>(esp_timer_get_time() - queued_us);
        if (ret == ESP_OK) {
            ++stats.chunks;
        }
        return ret;
    };

    uint16_t next_y = 0U;
    uint8_t queued_order[2] = {0U, 1U};
    uint16_t submitted = 0U;
    uint16_t completed = 0U;
    const int64_t stream_started_us = esp_timer_get_time();

    for (uint8_t buffer_index = 0U; buffer_index < 2U && next_y < height; ++buffer_index) {
        const uint16_t rows = static_cast<uint16_t>(
            next_y + staging_rows <= height ? staging_rows : height - next_y);
        result = prepare_strip(buffer_index, next_y, rows);
        if (result != ESP_OK) {
            break;
        }
        next_y = static_cast<uint16_t>(next_y + rows);
        result = queue_color(buffer_index, submitted == 0U ? LCD_CMD_RAMWR : LCD_CMD_RAMWRC, rows);
        if (result != ESP_OK) {
            break;
        }
        queued_order[submitted & 1U] = buffer_index;
        ++submitted;
    }

    while (result == ESP_OK && completed < submitted) {
        spi_transaction_t *returned = nullptr;
        const int64_t wait_started_us = esp_timer_get_time();
        result = spi_device_get_trans_result(
            io->spi_dev,
            &returned,
            pdMS_TO_TICKS(BOUNDED_SPI_RESULT_TIMEOUT_MS));
        stats.queue_wait_us += static_cast<uint32_t>(esp_timer_get_time() - wait_started_us);
        if (result != ESP_OK) {
            break;
        }

        const uint8_t buffer_index = queued_order[completed & 1U];
        if (returned != &g_bounded_spi.color_tx[buffer_index].base) {
            ESP_LOGE(TAG,
                "R.36.4 BoundedSPI：color事务身份错位 gen=%u seq=%u expected=%p actual=%p；受控重启",
                static_cast<unsigned>(g_bounded_spi.generation),
                static_cast<unsigned>(g_bounded_spi.color_sequence[buffer_index]),
                static_cast<void *>(&g_bounded_spi.color_tx[buffer_index].base),
                static_cast<void *>(returned));
            esp_restart();
        }
        ++completed;

        if (next_y >= height) {
            continue;
        }
        const uint16_t rows = static_cast<uint16_t>(
            next_y + staging_rows <= height ? staging_rows : height - next_y);
        result = prepare_strip(buffer_index, next_y, rows);
        if (result != ESP_OK) {
            break;
        }
        next_y = static_cast<uint16_t>(next_y + rows);
        result = queue_color(buffer_index, LCD_CMD_RAMWRC, rows);
        if (result != ESP_OK) {
            break;
        }
        queued_order[submitted & 1U] = buffer_index;
        ++submitted;
    }

    // 正常路径确保全部 raw transaction 都被本层回收，不把任何 descriptor 留给 Panel IO。
    while (result == ESP_OK && completed < submitted) {
        spi_transaction_t *returned = nullptr;
        const int64_t wait_started_us = esp_timer_get_time();
        result = spi_device_get_trans_result(
            io->spi_dev,
            &returned,
            pdMS_TO_TICKS(BOUNDED_SPI_RESULT_TIMEOUT_MS));
        stats.queue_wait_us += static_cast<uint32_t>(esp_timer_get_time() - wait_started_us);
        if (result != ESP_OK) {
            break;
        }
        const uint8_t buffer_index = queued_order[completed & 1U];
        if (returned != &g_bounded_spi.color_tx[buffer_index].base) {
            ESP_LOGE(TAG,
                "R.36.4 BoundedSPI：尾部color事务身份错位 gen=%u expected=%p actual=%p；受控重启",
                static_cast<unsigned>(g_bounded_spi.generation),
                static_cast<void *>(&g_bounded_spi.color_tx[buffer_index].base),
                static_cast<void *>(returned));
            esp_restart();
        }
        ++completed;
    }
    stats.stream_us = static_cast<uint32_t>(esp_timer_get_time() - stream_started_us);

    if (result != ESP_OK) {
        // 有界恢复：最多再等固定窗口回收本层已经提交的 descriptor。若仍有未完成事务，
        // 立即受控重启；绝不释放仍可能被 DMA 引用的 staging，也不把脏 result queue 交回 LVGL。
        const int64_t deadline_us = esp_timer_get_time() +
            static_cast<int64_t>(BOUNDED_SPI_RECOVERY_TIMEOUT_MS) * 1000LL;
        while (completed < submitted && esp_timer_get_time() < deadline_us) {
            spi_transaction_t *returned = nullptr;
            if (spi_device_get_trans_result(io->spi_dev, &returned, pdMS_TO_TICKS(10)) == ESP_OK) {
                const uint8_t buffer_index = queued_order[completed & 1U];
                if (returned != &g_bounded_spi.color_tx[buffer_index].base) {
                    ESP_LOGE(TAG,
                        "R.36.4 BoundedSPI恢复阶段color身份错位 gen=%u expected=%p actual=%p；受控重启",
                        static_cast<unsigned>(g_bounded_spi.generation),
                        static_cast<void *>(&g_bounded_spi.color_tx[buffer_index].base),
                        static_cast<void *>(returned));
                    esp_restart();
                }
                ++completed;
            }
        }
        if (completed < submitted) {
            ESP_LOGE(TAG,
                "R.36.4 BoundedSPI恢复未排空：gen=%u completed=%u submitted=%u；受控重启避免DMA descriptor/UAF与Panel IO结果队列污染",
                static_cast<unsigned>(g_bounded_spi.generation),
                static_cast<unsigned>(completed),
                static_cast<unsigned>(submitted));
            esp_restart();
        }
        display_bounded_spi_poison("raw transaction queue/result失败");
    }

    stats.total_us = static_cast<uint32_t>(esp_timer_get_time() - total_started_us);
    if (out_stats != nullptr) {
        *out_stats = stats;
    }
    return result;
}

esp_err_t display_launcher_bounded_spi_present_stream(
    DisplayBoundedSpiStripProducer producer,
    void *producer_context,
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    bool wait_for_te,
    DisplayBoundedSpiStats *out_stats)
{
    if (producer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return display_bounded_spi_present_internal(
        nullptr, producer, producer_context, x, y, width, height, wire_order, wait_for_te, out_stats);
}

esp_err_t display_launcher_bounded_spi_present(
    const uint8_t *rgb565,
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    bool wait_for_te,
    DisplayBoundedSpiStats *out_stats)
{
    return display_bounded_spi_present_internal(
        rgb565, nullptr, nullptr, x, y, width, height, wire_order, wait_for_te, out_stats);
}

esp_err_t display_cover_bounded_spi_present(
    const uint8_t *rgb565,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    DisplayBoundedSpiStats *out_stats)
{
    if (rgb565 == nullptr || width != FAKEPOD_LCD_WIDTH || height != FAKEPOD_LCD_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display_bounded_spi_available_internal() || g_bounded_spi.active) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = display_bounded_spi_session_begin("cover");
    if (result != ESP_OK) {
        return result;
    }

    result = display_bounded_spi_present_internal(
        rgb565, nullptr, nullptr, 0U, 0U, width, height, wire_order, true, out_stats);
    display_bounded_spi_session_end_internal();
    return result;
}
