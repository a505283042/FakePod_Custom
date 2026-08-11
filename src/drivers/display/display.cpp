#include "display.h"

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "esp_lcd_co5300.h"

#include "board_pins.h"


static const char *TAG = "显示";


// ============================================================
// 显示硬件参数
// ============================================================

// 使用 ESP32-S3 的 SPI2 外设驱动屏幕
static constexpr spi_host_device_t LCD_HOST =
    SPI2_HOST;


// P1.5.3.2R.29：继续保持 50MHz QSPI、40 行 LVGL DMA 与双 staging。
// DirectPresent 不再对每个 strip 调 panel_draw_bitmap()；改为一次设置完整 GRAM 窗口，
// 首块 RAMWR、后续 RAMWRC 连续写，并保持两笔 color transaction 常驻队列，减少总线空隙。
static constexpr int LCD_TRANSFER_HEIGHT =
    40;

static constexpr int LCD_PIXEL_CLOCK_HZ =
    50 * 1000 * 1000;


static constexpr size_t LCD_TRANSFER_BUFFER_SIZE =
    FAKEPOD_LCD_WIDTH *
    LCD_TRANSFER_HEIGHT *
    2;

// R.27 双 staging 必须一次拿到两块同尺寸 Internal DMA 内存。优先 16 行双缓冲（29.44KB 总计），
// 若运行期碎片化导致第二块申请失败，就自动降到 12/8/4 行。
static constexpr uint16_t DIRECT_PRESENT_PIPELINE_ROWS[] = {16U, 12U, 8U, 4U};

// CO5300 官方 QSPI 驱动的 32-bit command phase 编码：
// bits31:24=opcode，bits15:8=DCS command。官方驱动写命令使用 0x02，写颜色使用 0x32。
static constexpr uint32_t CO5300_QSPI_OPCODE_WRITE_CMD = 0x02U;
static constexpr uint32_t CO5300_QSPI_OPCODE_WRITE_COLOR = 0x32U;
static constexpr UBaseType_t DIRECT_PRESENT_DONE_QUEUE_DEPTH = 12U;

static constexpr int display_co5300_qspi_command(uint32_t opcode, uint8_t command)
{
    return static_cast<int>((opcode << 24U) | (static_cast<uint32_t>(command) << 8U));
}


// ============================================================
// 全局状态
// ============================================================

static esp_lcd_panel_io_handle_t g_panel_io =
    nullptr;


static esp_lcd_panel_handle_t g_panel =
    nullptr;


static SemaphoreHandle_t g_tx_done =
    nullptr;

// P1.5.3.2R.26：Panel IO color-done 统一桥接。esp_lvgl_port_add_disp() 会覆盖
// display_init() 阶段注册的 callback，因此 UI 注册完 LVGL display 后再安装此桥。
// DirectPresent 期间 callback 只释放 g_tx_done；其余时间保持 LVGL 原语义，
// 对当前 display 调用 lv_display_flush_ready()。
static lv_display_t *g_lvgl_display = nullptr;
static volatile bool g_direct_present_active = false;
static bool g_color_done_bridge_installed = false;


// P1.5.3.2R.21：CO5300 TE 上升沿同步。
// TE 只负责给刷新起点提供垂直时序基准；像素数据仍由现有 QSPI + SPI DMA 发送。
static SemaphoreHandle_t g_te_edge =
    nullptr;

static bool g_te_ready =
    false;

static uint32_t g_te_period_us =
    0U;


// R.22：封面整帧 present hold。请求由 LVGL 线程发起并在同一 LVGL 刷新事件线程消费，
// 因此这里只需要轻量状态，不引入额外任务/队列。
static bool g_present_hold_requested =
    false;

static bool g_present_output_enabled =
    false;

static uint32_t g_present_output_toggle_count =
    0U;


// 初始化是否成功
static bool g_ready =
    false;


// ============================================================
// 鱼鹰 AM200Q460460LK 官方初始化序列
// ============================================================
//
// 厂家原始初始化代码：
//
// FE 00
// C4 80
// 3A 55
// 35 00
// 53 20
// 51 FF
// 63 FF
// 2A 00 0A 01 D5
// 2B 00 00 01 CB
// 11
// delay 60ms
// 29
//
// 其中：
//
// 3A 55 = RGB565
//
// X：0x000A ~ 0x01D5
//    10 ~ 469
//    共 460 像素
//
// Y：0x0000 ~ 0x01CB
//    0 ~ 459
//    共 460 像素
//
// ============================================================

static const uint8_t INIT_FE[] =
    {0x00};

static const uint8_t INIT_C4[] =
    {0x80};

static const uint8_t INIT_3A[] =
    {0x55};

static const uint8_t INIT_35[] =
    {0x00};

static const uint8_t INIT_53[] =
    {0x20};

static const uint8_t INIT_51[] =
    {0xFF};

static const uint8_t INIT_63[] =
    {0xFF};

static const uint8_t INIT_2A[] =
    {
        0x00,
        0x0A,
        0x01,
        0xD5
    };

static const uint8_t INIT_2B[] =
    {
        0x00,
        0x00,
        0x01,
        0xCB
    };


static const co5300_lcd_init_cmd_t LCD_INIT_COMMANDS[] =
{
    {
        0xFE,
        INIT_FE,
        sizeof(INIT_FE),
        0
    },

    {
        0xC4,
        INIT_C4,
        sizeof(INIT_C4),
        0
    },

    {
        0x3A,
        INIT_3A,
        sizeof(INIT_3A),
        0
    },

    {
        0x35,
        INIT_35,
        sizeof(INIT_35),
        0
    },

    {
        0x53,
        INIT_53,
        sizeof(INIT_53),
        0
    },

    {
        0x51,
        INIT_51,
        sizeof(INIT_51),
        0
    },

    {
        0x63,
        INIT_63,
        sizeof(INIT_63),
        0
    },

    {
        0x2A,
        INIT_2A,
        sizeof(INIT_2A),
        0
    },

    {
        0x2B,
        INIT_2B,
        sizeof(INIT_2B),
        0
    },

    {
        0x11,
        nullptr,
        0,
        60
    },

    {
        0x29,
        nullptr,
        0,
        20
    }
};


// ============================================================
// CO5300 TE（Tearing Effect）同步
// ============================================================

static void display_te_isr(void *arg)
{
    (void)arg;

    if (g_te_edge == nullptr) {
        return;
    }

    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(g_te_edge, &task_woken);
    if (task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}


static bool display_te_take_edge(TickType_t timeout_ticks)
{
    if (g_te_edge == nullptr) {
        return false;
    }

    return xSemaphoreTake(g_te_edge, timeout_ticks) == pdTRUE;
}


static esp_err_t display_te_init()
{
    g_te_ready = false;
    g_te_period_us = 0U;

    g_te_edge = xSemaphoreCreateBinary();
    if (g_te_edge == nullptr) {
        ESP_LOGW(TAG, "R.21 TE：创建同步信号量失败，继续无TE模式");
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t te_config = {};
    te_config.pin_bit_mask = 1ULL << FAKEPOD_LCD_TE;
    te_config.mode = GPIO_MODE_INPUT;
    te_config.pull_up_en = GPIO_PULLUP_DISABLE;
    te_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    te_config.intr_type = GPIO_INTR_POSEDGE;

    esp_err_t ret = gpio_config(&te_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "R.21 TE：GPIO%d 输入配置失败：%s",
            FAKEPOD_LCD_TE, esp_err_to_name(ret));
        return ret;
    }

    // GPIO ISR service 可能已由其他驱动安装；ESP_ERR_INVALID_STATE 表示可直接复用。
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "R.21 TE：安装GPIO ISR service失败：%s", esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_isr_handler_add(
        static_cast<gpio_num_t>(FAKEPOD_LCD_TE),
        display_te_isr,
        nullptr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "R.21 TE：GPIO%d ISR注册失败：%s",
            FAKEPOD_LCD_TE, esp_err_to_name(ret));
        return ret;
    }

    // 丢弃注册瞬间可能残留的边沿，从两个连续 TE 上升沿测一次真实周期。
    while (display_te_take_edge(0)) {
    }

    if (!display_te_take_edge(pdMS_TO_TICKS(100))) {
        ESP_LOGW(TAG, "R.21 TE：GPIO%d 100ms内未检测到首个TE上升沿，自动降级",
            FAKEPOD_LCD_TE);
        return ESP_ERR_TIMEOUT;
    }

    const int64_t first_us = esp_timer_get_time();
    if (!display_te_take_edge(pdMS_TO_TICKS(100))) {
        ESP_LOGW(TAG, "R.21 TE：GPIO%d 未检测到第二个TE上升沿，自动降级",
            FAKEPOD_LCD_TE);
        return ESP_ERR_TIMEOUT;
    }

    const int64_t second_us = esp_timer_get_time();
    const int64_t measured_us = second_us - first_us;
    if (measured_us <= 0 || measured_us > 100000) {
        ESP_LOGW(TAG, "R.21 TE：测得周期异常=%lldus，自动降级",
            static_cast<long long>(measured_us));
        return ESP_FAIL;
    }

    g_te_period_us = static_cast<uint32_t>(measured_us);
    g_te_ready = true;

    const uint32_t refresh_x100 =
        g_te_period_us > 0U ? 100000000U / g_te_period_us : 0U;
    ESP_LOGI(TAG,
        "R.21 TE同步就绪：GPIO=%d 上升沿，period=%uus (~%u.%02uHz)",
        FAKEPOD_LCD_TE,
        static_cast<unsigned>(g_te_period_us),
        static_cast<unsigned>(refresh_x100 / 100U),
        static_cast<unsigned>(refresh_x100 % 100U));

    return ESP_OK;
}


// ============================================================
// QSPI DMA 完成回调
// ============================================================

static bool IRAM_ATTR display_on_color_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_ctx
)
{
    (void) panel_io;
    (void) event_data;


    SemaphoreHandle_t semaphore =
        static_cast<SemaphoreHandle_t>(
            user_ctx
        );


    BaseType_t task_woken =
        pdFALSE;


    xSemaphoreGiveFromISR(
        semaphore,
        &task_woken
    );


    return
        task_woken == pdTRUE;
}


// ============================================================
// R.26 Panel IO color-done 统一桥接
// ============================================================

static bool IRAM_ATTR display_color_done_bridge(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_ctx)
{
    (void) panel_io;
    (void) event_data;
    (void) user_ctx;

    BaseType_t task_woken = pdFALSE;

    if (g_direct_present_active) {
        if (g_tx_done != nullptr) {
            xSemaphoreGiveFromISR(g_tx_done, &task_woken);
        }
    } else if (g_lvgl_display != nullptr) {
        // 与 esp_lvgl_port 自带 SPI color-done callback 保持同样语义。
        // 只有真正的 LVGL flush 才进入这里；DirectPresent 完成通知不会误喂给 LVGL。
        lv_display_flush_ready(g_lvgl_display);
    }

    return task_woken == pdTRUE;
}


// ============================================================
// 初始化 CO5300
// ============================================================

esp_err_t display_init()
{
    if (g_ready) {
        return ESP_OK;
    }


    ESP_LOGI(
        TAG,
        "正在初始化 CO5300 AMOLED"
    );


    ESP_LOGI(
        TAG,
        "分辨率：%d × %d",
        FAKEPOD_LCD_WIDTH,
        FAKEPOD_LCD_HEIGHT
    );


    ESP_LOGI(
        TAG,
        "QSPI：CLK=%d CS=%d D0=%d D1=%d D2=%d D3=%d RST=%d",
        FAKEPOD_LCD_CLK,
        FAKEPOD_LCD_CS,
        FAKEPOD_LCD_D0,
        FAKEPOD_LCD_D1,
        FAKEPOD_LCD_D2,
        FAKEPOD_LCD_D3,
        FAKEPOD_LCD_RST
    );


    // ========================================================
    // 创建 DMA 完成信号量
    // ========================================================

    // R.29：DirectPresent 同时允许两笔 color transaction 在 Panel IO 队列中飞行，
    // 完成通知不能再用 binary semaphore，否则两个 ISR 回调可能合并丢计数。
    g_tx_done =
        xSemaphoreCreateCounting(DIRECT_PRESENT_DONE_QUEUE_DEPTH, 0U);


    if (g_tx_done == nullptr) {

        ESP_LOGE(
            TAG,
            "创建屏幕 DMA 信号量失败"
        );

        return ESP_ERR_NO_MEM;
    }


    // P1.5.3.2R.18：不再为 display_test_colors() 永久占用内部 DMA RAM。
    // 颜色测试若真的被调用，会临时申请 40 行 buffer，测试结束立即释放。


    // ========================================================
    // 初始化 QSPI 总线
    // ========================================================

    ESP_LOGI(
        TAG,
        "正在初始化 QSPI 总线"
    );


    // ========================================================
    // 配置 QSPI 总线
    //
    // 不直接使用 CO5300_PANEL_BUS_QSPI_CONFIG 宏。
    //
    // 原因：该宏使用 C 风格指定初始化器，
    // 在 ESP-IDF 5.5 + C++ 下会因为结构体字段顺序
    // 不一致而触发编译错误。
    //
    // 这里采用先清零、再逐字段赋值的方式，
    // 功能与官方宏完全一致，同时兼容 C++。
    // ========================================================

    spi_bus_config_t bus_config = {};


        // QSPI 时钟
        bus_config.sclk_io_num =
            FAKEPOD_LCD_CLK;


        // QSPI Data0 ~ Data3
        bus_config.data0_io_num =
            FAKEPOD_LCD_D0;

        bus_config.data1_io_num =
            FAKEPOD_LCD_D1;

        bus_config.data2_io_num =
            FAKEPOD_LCD_D2;

        bus_config.data3_io_num =
            FAKEPOD_LCD_D3;


        // 当前只使用 Quad SPI，
        // Data4 ~ Data7 不使用。
        bus_config.data4_io_num = -1;
        bus_config.data5_io_num = -1;
        bus_config.data6_io_num = -1;
        bus_config.data7_io_num = -1;


        // 单次 DMA 最大传输尺寸
        bus_config.max_transfer_sz =
            LCD_TRANSFER_BUFFER_SIZE;


    esp_err_t ret =
        spi_bus_initialize(
            LCD_HOST,
            &bus_config,
            SPI_DMA_CH_AUTO
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "QSPI 总线初始化失败：%s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    ESP_LOGI(
        TAG,
        "R.22 QSPI 总线初始化成功：50MHz / Quad / SPI_DMA_CH_AUTO / max_transfer=%uB",
        static_cast<unsigned>(LCD_TRANSFER_BUFFER_SIZE)
    );


    // ========================================================
    // 创建 LCD Panel IO
    // ========================================================

    ESP_LOGI(
        TAG,
        "正在创建 CO5300 Panel IO"
    );


    // ========================================================
    // 配置 CO5300 QSPI Panel IO
    //
    // 同样不使用官方宏，避免 C++ 指定初始化器
    // 带来的字段顺序/缺省字段警告。
    // ========================================================

    esp_lcd_panel_io_spi_config_t io_config = {};


        // CS 引脚
        io_config.cs_gpio_num =
            FAKEPOD_LCD_CS;


        // QSPI 模式没有单独 DC 引脚
        io_config.dc_gpio_num =
            -1;


        // SPI Mode 0
        io_config.spi_mode =
            0;


        // R.22：50MHz QSPI 实验；其余 SPI mode / Quad / DMA 参数保持不变。
        io_config.pclk_hz =
            LCD_PIXEL_CLOCK_HZ;


        // DMA 事务队列深度
        io_config.trans_queue_depth =
            10;


        // DMA 像素传输完成回调
        io_config.on_color_trans_done =
            display_on_color_done;


        // 回调参数
        io_config.user_ctx =
            g_tx_done;


        // CO5300 QSPI 命令使用 32bit command phase
        io_config.lcd_cmd_bits =
            32;


        // 参数长度 8bit
        io_config.lcd_param_bits =
            8;


        // 开启 Quad SPI
        io_config.flags.quad_mode =
            true;


    ret =
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t) LCD_HOST,
            &io_config,
            &g_panel_io
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "创建 Panel IO 失败：%s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    // ========================================================
    // CO5300 厂家参数
    // ========================================================

    co5300_vendor_config_t vendor_config =
        {};


    vendor_config.init_cmds =
        LCD_INIT_COMMANDS;


    vendor_config.init_cmds_size =
        sizeof(LCD_INIT_COMMANDS) /
        sizeof(LCD_INIT_COMMANDS[0]);


    vendor_config.flags.use_qspi_interface =
        1;


    // ========================================================
    // CO5300 面板参数
    // ========================================================

    esp_lcd_panel_dev_config_t panel_config =
        {};


    panel_config.reset_gpio_num =
        FAKEPOD_LCD_RST;


    panel_config.rgb_ele_order =
        LCD_RGB_ELEMENT_ORDER_RGB;


    panel_config.bits_per_pixel =
        16;


    panel_config.vendor_config =
        &vendor_config;


    // ========================================================
    // 创建 CO5300 面板
    // ========================================================

    ESP_LOGI(
        TAG,
        "正在创建 CO5300 面板驱动"
    );


    ret =
        esp_lcd_new_panel_co5300(
            g_panel_io,
            &panel_config,
            &g_panel
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "创建 CO5300 驱动失败：%s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    // ========================================================
    // 硬件复位
    // ========================================================

    ESP_LOGI(
        TAG,
        "正在复位显示屏"
    );


    ret =
        esp_lcd_panel_reset(
            g_panel
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "显示屏复位失败：%s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    // ========================================================
    // 执行厂家初始化序列
    // ========================================================

    ESP_LOGI(
        TAG,
        "正在发送厂家初始化命令"
    );


    ret =
        esp_lcd_panel_init(
            g_panel
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "CO5300 初始化失败：%s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    // ========================================================
    // 设置屏幕显存偏移
    // ========================================================
    //
    // 此屏有效显示区域：
    //
    // X = 10 ~ 469
    // Y = 0  ~ 459
    //
    // 因此逻辑坐标：
    //
    // 0 ~ 459
    //
    // 会自动映射到：
    //
    // 10 ~ 469
    //
    // ========================================================

    ret =
        esp_lcd_panel_set_gap(
            g_panel,
            FAKEPOD_LCD_X_OFFSET,
            FAKEPOD_LCD_Y_OFFSET
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "设置显示区域偏移失败：%s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    // ========================================================
    // 开启显示
    // ========================================================

    ret =
        esp_lcd_panel_disp_on_off(
            g_panel,
            true
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "开启显示失败：%s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    g_present_output_enabled = true;


    // ========================================================
    // Bring-up 阶段降低 AMOLED 亮度
    // ========================================================

    ret =
        esp_lcd_panel_co5300_set_brightness(
            g_panel,
            60
        );


    if (ret != ESP_OK) {

        ESP_LOGW(
            TAG,
            "设置亮度失败：%s",
            esp_err_to_name(ret)
        );
    }


    // P1.5.3.2R.21：厂家初始化序列已经发送 0x35 00 (TEON)，
    // 此处把屏幕 TE 输出 GPIO6 真正接入 ESP32-S3。失败只降级为原刷新路径。
    const esp_err_t te_ret = display_te_init();
    if (te_ret != ESP_OK) {
        ESP_LOGW(TAG, "R.21 TE同步未启用：%s；显示继续使用原QSPI DMA路径",
            esp_err_to_name(te_ret));
    }


    g_ready =
        true;


    ESP_LOGI(
        TAG,
        "CO5300 AMOLED 初始化成功"
    );


    return ESP_OK;
}


// ============================================================
// 填充整个屏幕
// ============================================================

static esp_err_t display_fill_rgb565(
    uint16_t color,
    uint8_t *strip_buffer
)
{
    if (
        !g_ready ||
        g_panel == nullptr ||
        strip_buffer == nullptr
    ) {

        return ESP_ERR_INVALID_STATE;
    }


    // ========================================================
    // RGB565 使用高字节在前发送
    // ========================================================

    const uint8_t high =
        static_cast<uint8_t>(
            color >> 8
        );


    const uint8_t low =
        static_cast<uint8_t>(
            color & 0xFF
        );


    const int pixel_count =
        FAKEPOD_LCD_WIDTH *
        LCD_TRANSFER_HEIGHT;


    for (
        int i = 0;
        i < pixel_count;
        ++i
    ) {

        strip_buffer[i * 2] =
            high;


        strip_buffer[i * 2 + 1] =
            low;
    }


    // ========================================================
    // 每次传输 40 行
    // ========================================================

    for (
        int y = 0;
        y < FAKEPOD_LCD_HEIGHT;
        y += LCD_TRANSFER_HEIGHT
    ) {

        // 清掉可能残留的完成信号
        xSemaphoreTake(
            g_tx_done,
            0
        );


        const int end_y =
            (y + LCD_TRANSFER_HEIGHT < FAKEPOD_LCD_HEIGHT)
                ? y + LCD_TRANSFER_HEIGHT
                : FAKEPOD_LCD_HEIGHT;


        esp_err_t ret =
            esp_lcd_panel_draw_bitmap(
                g_panel,

                0,
                y,

                FAKEPOD_LCD_WIDTH,
                end_y,

                strip_buffer
            );


        if (ret != ESP_OK) {

            ESP_LOGE(
                TAG,
                "发送像素失败，Y=%d：%s",
                y,
                esp_err_to_name(ret)
            );

            return ret;
        }


        // 等待这一条 DMA 传输真正完成，
        // 防止下一次传输提前覆盖缓冲。
        if (
            xSemaphoreTake(
                g_tx_done,
                pdMS_TO_TICKS(1000)
            ) != pdTRUE
        ) {

            ESP_LOGE(
                TAG,
                "等待屏幕 DMA 完成超时，Y=%d",
                y
            );

            return ESP_ERR_TIMEOUT;
        }
    }


    return ESP_OK;
}


// ============================================================
// 屏幕颜色测试
// ============================================================

esp_err_t display_test_colors()
{
    if (!g_ready) {
        return ESP_ERR_INVALID_STATE;
    }


    ESP_LOGI(
        TAG,
        "开始屏幕颜色测试"
    );


    uint8_t *strip_buffer = static_cast<uint8_t *>(
        heap_caps_malloc(
            LCD_TRANSFER_BUFFER_SIZE,
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
        )
    );
    if (strip_buffer == nullptr) {
        ESP_LOGE(TAG, "颜色测试临时 DMA buffer 申请失败：%uB",
            static_cast<unsigned>(LCD_TRANSFER_BUFFER_SIZE));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "颜色测试临时 DMA buffer：%uB（测试结束释放）",
        static_cast<unsigned>(LCD_TRANSFER_BUFFER_SIZE));


    // RGB565
    static constexpr uint16_t COLOR_RED =
        0xF800;

    static constexpr uint16_t COLOR_GREEN =
        0x07E0;

    static constexpr uint16_t COLOR_BLUE =
        0x001F;

    static constexpr uint16_t COLOR_WHITE =
        0xFFFF;

    static constexpr uint16_t COLOR_DARK_BLUE =
        0x0010;


    ESP_LOGI(
        TAG,
        "测试：红色"
    );

    ESP_ERROR_CHECK(
        display_fill_rgb565(
            COLOR_RED,
            strip_buffer
        )
    );

    vTaskDelay(
        pdMS_TO_TICKS(600)
    );


    ESP_LOGI(
        TAG,
        "测试：绿色"
    );

    ESP_ERROR_CHECK(
        display_fill_rgb565(
            COLOR_GREEN,
            strip_buffer
        )
    );

    vTaskDelay(
        pdMS_TO_TICKS(600)
    );


    ESP_LOGI(
        TAG,
        "测试：蓝色"
    );

    ESP_ERROR_CHECK(
        display_fill_rgb565(
            COLOR_BLUE,
            strip_buffer
        )
    );

    vTaskDelay(
        pdMS_TO_TICKS(600)
    );


    ESP_LOGI(
        TAG,
        "测试：白色"
    );

    ESP_ERROR_CHECK(
        display_fill_rgb565(
            COLOR_WHITE,
            strip_buffer
        )
    );

    // AMOLED 白屏只短暂显示
    vTaskDelay(
        pdMS_TO_TICKS(300)
    );


    // 最后保持低亮度深蓝色，
    // 方便确认程序仍然正常运行。
    ESP_ERROR_CHECK(
        display_fill_rgb565(
            COLOR_DARK_BLUE,
            strip_buffer
        )
    );


    heap_caps_free(strip_buffer);
    strip_buffer = nullptr;


    ESP_LOGI(
        TAG,
        "颜色测试完成，屏幕保持深蓝色；临时 DMA buffer 已释放"
    );


    return ESP_OK;
}


// ============================================================
// 获取初始化状态
// ============================================================

bool display_is_ready()
{
    return g_ready;
}


// ============================================================
// 获取底层 LCD 句柄
// ============================================================

bool display_te_is_ready()
{
    return g_te_ready && g_te_edge != nullptr;
}


bool display_te_wait_next(uint32_t timeout_ms)
{
    if (!display_te_is_ready()) {
        return false;
    }

    // 只接受“调用之后”的下一次 TE，避免消费旧的已缓存脉冲后立即开始刷新。
    while (display_te_take_edge(0)) {
    }

    const TickType_t timeout_ticks =
        timeout_ms == 0U ? 0U : pdMS_TO_TICKS(timeout_ms);
    return display_te_take_edge(timeout_ticks);
}


uint32_t display_te_get_period_us()
{
    return g_te_period_us;
}


esp_lcd_panel_io_handle_t display_get_panel_io()
{
    return g_panel_io;
}

esp_lcd_panel_handle_t display_get_panel()
{
    return g_panel;
}


esp_err_t display_install_lvgl_color_done_bridge(void *lvgl_display)
{
    if (!g_ready || g_panel_io == nullptr || lvgl_display == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    g_lvgl_display = static_cast<lv_display_t *>(lvgl_display);

    esp_lcd_panel_io_callbacks_t callbacks = {};
    callbacks.on_color_trans_done = display_color_done_bridge;
    const esp_err_t ret = esp_lcd_panel_io_register_event_callbacks(
        g_panel_io,
        &callbacks,
        nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "R.26 安装LVGL/DirectPresent color-done桥接失败：%s", esp_err_to_name(ret));
        g_lvgl_display = nullptr;
        g_color_done_bridge_installed = false;
        return ret;
    }

    g_color_done_bridge_installed = true;
    ESP_LOGI(TAG,
        "R.26 color-done桥接已安装：LVGL flush_ready + DirectPresent DMA semaphore 共用Panel IO callback");
    return ESP_OK;
}


// ============================================================
// R.29 封面 Direct Surface Present / Continuous GRAM Stream
// ============================================================

esp_err_t display_present_rgb565_direct(
    const uint8_t *rgb565,
    uint16_t width,
    uint16_t height,
    bool wire_order,
    DisplayDirectPresentStats *out_stats)
{
    DisplayDirectPresentStats stats = {};
    stats.wire_order = wire_order;
    stats.continuous_stream = true;
    const int64_t total_started_us = esp_timer_get_time();

    if (!g_ready || g_panel == nullptr || g_panel_io == nullptr || g_tx_done == nullptr ||
        !g_color_done_bridge_installed || g_lvgl_display == nullptr || rgb565 == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (width != FAKEPOD_LCD_WIDTH || height != FAKEPOD_LCD_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    // 先让 LVGL 已排队的 color transaction 完成。tx_param(-1) 是既有 queue barrier；
    // barrier 返回后才切到 DirectPresent callback 路由，避免迟到的 LVGL callback 串台。
    const int64_t barrier_started_us = esp_timer_get_time();
    esp_err_t result = esp_lcd_panel_io_tx_param(g_panel_io, -1, nullptr, 0);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "R.29 ContinuousGRAM：Panel IO queue barrier失败：%s", esp_err_to_name(result));
        return result;
    }
    stats.io_barrier_us = static_cast<uint32_t>(esp_timer_get_time() - barrier_started_us);

    while (xSemaphoreTake(g_tx_done, 0) == pdTRUE) {
    }
    g_direct_present_active = true;

    // 继续沿用 R.27/R.28 的自适应双 Internal DMA staging。R.29 的优化点不是增大 buffer，
    // 而是让两个 buffer 对应的 color transaction 真正排队连续发送。
    uint8_t *dma_strip[2] = {nullptr, nullptr};
    uint16_t staging_rows = 0U;
    size_t staging_bytes = 0U;
    const size_t dma_free_before =
        heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t dma_largest_before =
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    for (uint16_t candidate_rows : DIRECT_PRESENT_PIPELINE_ROWS) {
        const size_t candidate_bytes =
            static_cast<size_t>(width) * static_cast<size_t>(candidate_rows) * 2U;
        dma_strip[0] = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16U,
            candidate_bytes,
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (dma_strip[0] == nullptr) {
            continue;
        }
        dma_strip[1] = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16U,
            candidate_bytes,
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (dma_strip[1] != nullptr) {
            staging_rows = candidate_rows;
            staging_bytes = candidate_bytes;
            break;
        }
        heap_caps_free(dma_strip[0]);
        dma_strip[0] = nullptr;
    }

    if (dma_strip[0] == nullptr || dma_strip[1] == nullptr || staging_rows == 0U) {
        if (dma_strip[0] != nullptr) heap_caps_free(dma_strip[0]);
        if (dma_strip[1] != nullptr) heap_caps_free(dma_strip[1]);
        g_direct_present_active = false;
        ESP_LOGW(TAG,
            "R.29 ContinuousGRAM：双DMA staging申请失败 free=%u largest=%u，已尝试16/12/8/4行×2",
            static_cast<unsigned>(dma_free_before),
            static_cast<unsigned>(dma_largest_before));
        return ESP_ERR_NO_MEM;
    }

    stats.staging_rows = staging_rows;
    stats.staging_buffers = 2U;
    stats.staging_bytes = static_cast<uint32_t>(staging_bytes);
    stats.staging_total_bytes = static_cast<uint32_t>(staging_bytes * 2U);
    stats.queue_peak = 2U;

    auto prepare_strip = [&](uint8_t *dst, int y, int rows) {
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(rows) * 2U;
        const uint8_t *src = rgb565 + static_cast<size_t>(y) * width * 2U;
        const int64_t started_us = esp_timer_get_time();
        if (wire_order) {
            memcpy(dst, src, bytes);
            stats.copy_us += static_cast<uint32_t>(esp_timer_get_time() - started_us);
            return;
        }

        const size_t pixels = bytes / 2U;
        for (size_t i = 0; i < pixels; ++i) {
            dst[i * 2U] = src[i * 2U + 1U];
            dst[i * 2U + 1U] = src[i * 2U];
        }
        stats.byte_swap_us += static_cast<uint32_t>(esp_timer_get_time() - started_us);
    };

    auto tx_color_chunk = [&](uint8_t command, const uint8_t *buffer, size_t bytes) -> esp_err_t {
        const int qspi_command = display_co5300_qspi_command(
            CO5300_QSPI_OPCODE_WRITE_COLOR,
            command);
        return esp_lcd_panel_io_tx_color(g_panel_io, qspi_command, buffer, bytes);
    };

    // 在 TE 到来前先设置一次完整 GRAM 窗口。官方 CO5300 draw_bitmap() 每个 strip 都会
    // CASET + RASET + RAMWR；R.29 将 CASET/RASET 从 N 次缩为 1 次。
    const uint16_t x_start = static_cast<uint16_t>(FAKEPOD_LCD_X_OFFSET);
    const uint16_t x_end = static_cast<uint16_t>(FAKEPOD_LCD_X_OFFSET + width - 1U);
    const uint16_t y_start = static_cast<uint16_t>(FAKEPOD_LCD_Y_OFFSET);
    const uint16_t y_end = static_cast<uint16_t>(FAKEPOD_LCD_Y_OFFSET + height - 1U);
    const uint8_t caset[4] = {
        static_cast<uint8_t>(x_start >> 8U),
        static_cast<uint8_t>(x_start & 0xFFU),
        static_cast<uint8_t>(x_end >> 8U),
        static_cast<uint8_t>(x_end & 0xFFU),
    };
    const uint8_t raset[4] = {
        static_cast<uint8_t>(y_start >> 8U),
        static_cast<uint8_t>(y_start & 0xFFU),
        static_cast<uint8_t>(y_end >> 8U),
        static_cast<uint8_t>(y_end & 0xFFU),
    };

    const int64_t window_started_us = esp_timer_get_time();
    result = esp_lcd_panel_io_tx_param(
        g_panel_io,
        display_co5300_qspi_command(CO5300_QSPI_OPCODE_WRITE_CMD, LCD_CMD_CASET),
        caset,
        sizeof(caset));
    if (result == ESP_OK) {
        result = esp_lcd_panel_io_tx_param(
            g_panel_io,
            display_co5300_qspi_command(CO5300_QSPI_OPCODE_WRITE_CMD, LCD_CMD_RASET),
            raset,
            sizeof(raset));
    }
    stats.window_setup_us = static_cast<uint32_t>(esp_timer_get_time() - window_started_us);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "R.29 ContinuousGRAM：完整窗口设置失败：%s", esp_err_to_name(result));
    }

    int next_source_y = 0;
    int rows_in_buffer[2] = {0, 0};
    uint8_t queue_buffer_order[2] = {0U, 1U};
    uint8_t queued_count = 0U;
    uint8_t completed_count = 0U;

    const int64_t pipeline_started_us = esp_timer_get_time();

    // 首次预填最多两块，在 TE 到来后立即把 RAMWR + RAMWRC 两笔 color transaction 排入队列。
    if (result == ESP_OK) {
        for (uint8_t buffer_index = 0U; buffer_index < 2U && next_source_y < height; ++buffer_index) {
            const int rows = (next_source_y + staging_rows <= height)
                ? static_cast<int>(staging_rows)
                : static_cast<int>(height) - next_source_y;
            prepare_strip(dma_strip[buffer_index], next_source_y, rows);
            rows_in_buffer[buffer_index] = rows;
            next_source_y += rows;
        }
    }

    if (result == ESP_OK && display_te_is_ready()) {
        const uint32_t period_us = display_te_get_period_us();
        uint32_t timeout_ms = 25U;
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

    while (xSemaphoreTake(g_tx_done, 0) == pdTRUE) {
    }

    int64_t stream_started_us = 0;
    if (result == ESP_OK) {
        stream_started_us = esp_timer_get_time();
        for (uint8_t buffer_index = 0U; buffer_index < 2U; ++buffer_index) {
            if (rows_in_buffer[buffer_index] <= 0) {
                continue;
            }
            const size_t bytes = static_cast<size_t>(width) *
                static_cast<size_t>(rows_in_buffer[buffer_index]) * 2U;
            const uint8_t command = (queued_count == 0U) ? LCD_CMD_RAMWR : LCD_CMD_RAMWRC;
            result = tx_color_chunk(command, dma_strip[buffer_index], bytes);
            if (result != ESP_OK) {
                ESP_LOGW(TAG,
                    "R.29 ContinuousGRAM：首批tx_color失败 buffer=%u rows=%d：%s",
                    static_cast<unsigned>(buffer_index),
                    rows_in_buffer[buffer_index],
                    esp_err_to_name(result));
                break;
            }
            queue_buffer_order[queued_count] = buffer_index;
            ++queued_count;
            ++stats.chunks;
        }
    }

    // 两块 buffer 始终保持 FIFO 复用：等最老一块完成 -> 立刻用它准备后续数据 -> RAMWRC 入队。
    // 当 CPU memcpy/swap 比另一块 DMA 更快时，Panel IO 队列持续有下一笔 transaction，总线无需等 CPU。
    while (result == ESP_OK && completed_count < stats.chunks) {
        const int64_t wait_started_us = esp_timer_get_time();
        if (xSemaphoreTake(g_tx_done, pdMS_TO_TICKS(250)) != pdTRUE) {
            result = ESP_ERR_TIMEOUT;
            ESP_LOGW(TAG,
                "R.29 ContinuousGRAM：等待color-done超时 completed=%u queued=%u next_y=%d",
                static_cast<unsigned>(completed_count),
                static_cast<unsigned>(stats.chunks),
                next_source_y);
            break;
        }
        stats.dma_wait_us += static_cast<uint32_t>(esp_timer_get_time() - wait_started_us);

        const uint8_t completed_buffer = queue_buffer_order[completed_count & 1U];
        ++completed_count;

        if (next_source_y >= height) {
            continue;
        }

        const int rows = (next_source_y + staging_rows <= height)
            ? static_cast<int>(staging_rows)
            : static_cast<int>(height) - next_source_y;
        prepare_strip(dma_strip[completed_buffer], next_source_y, rows);
        rows_in_buffer[completed_buffer] = rows;
        next_source_y += rows;

        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(rows) * 2U;
        result = tx_color_chunk(LCD_CMD_RAMWRC, dma_strip[completed_buffer], bytes);
        if (result != ESP_OK) {
            ESP_LOGW(TAG,
                "R.29 ContinuousGRAM：RAMWRC续写失败 buffer=%u rows=%d：%s",
                static_cast<unsigned>(completed_buffer),
                rows,
                esp_err_to_name(result));
            break;
        }
        queue_buffer_order[stats.chunks & 1U] = completed_buffer;
        ++stats.chunks;
    }

    // 上面的循环条件会随着 stats.chunks 增长。最后确保所有已提交 transaction 均拿到 callback。
    while (result == ESP_OK && completed_count < stats.chunks) {
        const int64_t wait_started_us = esp_timer_get_time();
        if (xSemaphoreTake(g_tx_done, pdMS_TO_TICKS(250)) != pdTRUE) {
            result = ESP_ERR_TIMEOUT;
            ESP_LOGW(TAG,
                "R.29 ContinuousGRAM：尾部color-done超时 completed=%u queued=%u",
                static_cast<unsigned>(completed_count),
                static_cast<unsigned>(stats.chunks));
            break;
        }
        stats.dma_wait_us += static_cast<uint32_t>(esp_timer_get_time() - wait_started_us);
        ++completed_count;
    }

    if (stream_started_us != 0) {
        stats.stream_us = static_cast<uint32_t>(esp_timer_get_time() - stream_started_us);
        stats.dma_us = stats.stream_us;
    }

    const uint32_t pipeline_elapsed_us =
        static_cast<uint32_t>(esp_timer_get_time() - pipeline_started_us);
    stats.pipeline_us = pipeline_elapsed_us > stats.te_wait_us
        ? pipeline_elapsed_us - stats.te_wait_us
        : pipeline_elapsed_us;

    // 对 R.29 更有意义的 overlap 是 copy/swap 和连续 stream 的重叠量。
    const uint64_t serial_estimate_us =
        static_cast<uint64_t>(stats.byte_swap_us) + static_cast<uint64_t>(stats.copy_us) +
        static_cast<uint64_t>(stats.stream_us);
    stats.overlap_saved_us = serial_estimate_us > stats.pipeline_us
        ? static_cast<uint32_t>(serial_estimate_us - stats.pipeline_us)
        : 0U;

    // 异常路径强制 queue barrier，正常路径理论上所有 color-done 已消费；无论哪种情况都清理
    // 多余计数后才恢复 LVGL callback 路由。
    if (result != ESP_OK) {
        (void) esp_lcd_panel_io_tx_param(g_panel_io, -1, nullptr, 0);
    }
    while (xSemaphoreTake(g_tx_done, 0) == pdTRUE) {
    }
    g_direct_present_active = false;

    heap_caps_free(dma_strip[0]);
    heap_caps_free(dma_strip[1]);
    stats.total_us = static_cast<uint32_t>(esp_timer_get_time() - total_started_us);
    if (out_stats != nullptr) {
        *out_stats = stats;
    }

    static uint32_t s_direct_present_count = 0U;
    ++s_direct_present_count;
    if (s_direct_present_count <= 12U || (s_direct_present_count % 60U) == 0U || result != ESP_OK) {
        ESP_LOGI(TAG,
            "R.29 ContinuousGRAM：count=%u result=%s source=%s total=%uus barrier=%uus window=%uus te=%uus pipeline=%uus stream=%uus copy=%uus swap=%uus wait=%uus overlap≈%uus chunks=%u queue_peak=%u staging=%u行×%u total=%uB free=%u largest=%u aligned=%u",
            static_cast<unsigned>(s_direct_present_count),
            esp_err_to_name(result),
            stats.wire_order ? "wire" : "native",
            static_cast<unsigned>(stats.total_us),
            static_cast<unsigned>(stats.io_barrier_us),
            static_cast<unsigned>(stats.window_setup_us),
            static_cast<unsigned>(stats.te_wait_us),
            static_cast<unsigned>(stats.pipeline_us),
            static_cast<unsigned>(stats.stream_us),
            static_cast<unsigned>(stats.copy_us),
            static_cast<unsigned>(stats.byte_swap_us),
            static_cast<unsigned>(stats.dma_wait_us),
            static_cast<unsigned>(stats.overlap_saved_us),
            static_cast<unsigned>(stats.chunks),
            static_cast<unsigned>(stats.queue_peak),
            static_cast<unsigned>(stats.staging_rows),
            static_cast<unsigned>(stats.staging_buffers),
            static_cast<unsigned>(stats.staging_total_bytes),
            static_cast<unsigned>(dma_free_before),
            static_cast<unsigned>(dma_largest_before),
            static_cast<unsigned>(stats.te_aligned));
    }
    return result;
}


// ============================================================
// R.22 全屏封面 Present Hold
// ============================================================

void display_present_request_hold()
{
    g_present_hold_requested = true;
}


bool display_present_take_hold_request()
{
    const bool requested = g_present_hold_requested;
    g_present_hold_requested = false;
    return requested;
}


bool display_present_set_output(bool enabled)
{
    if (!g_ready || g_panel == nullptr) {
        return false;
    }
    if (g_present_output_enabled == enabled) {
        return true;
    }

    const esp_err_t ret = esp_lcd_panel_disp_on_off(g_panel, enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "R.22 PresentHold：%s显示输出失败：%s",
            enabled ? "恢复" : "暂停", esp_err_to_name(ret));
        return false;
    }

    g_present_output_enabled = enabled;
    ++g_present_output_toggle_count;
    if (g_present_output_toggle_count <= 8U || (g_present_output_toggle_count % 60U) == 0U) {
        ESP_LOGI(TAG, "R.22 PresentHold：输出=%s toggle=%u",
            enabled ? "ON" : "OFF",
            static_cast<unsigned>(g_present_output_toggle_count));
    }
    return true;
}


bool display_present_output_is_enabled()
{
    return g_present_output_enabled;
}
