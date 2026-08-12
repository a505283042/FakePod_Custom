#include "display.h"
#include "display_backend.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_lcd_co5300.h"

#include "board_pins.h"


static const char *TAG = "显示";


// ============================================================
// 显示硬件参数
// ============================================================

// 使用 ESP32-S3 的 SPI2 外设驱动屏幕
static constexpr spi_host_device_t LCD_HOST =
    SPI2_HOST;


// LVGL 官方刷新保持 50MHz QSPI 与 40 行 DMA 条带。
// 大面积 Cover/Launcher 快路径已经迁移到 display_bounded_spi.cpp。
static constexpr int LCD_TRANSFER_HEIGHT =
    40;

static constexpr int LCD_PIXEL_CLOCK_HZ =
    50 * 1000 * 1000;


static constexpr size_t LCD_TRANSFER_BUFFER_SIZE =
    FAKEPOD_LCD_WIDTH *
    LCD_TRANSFER_HEIGHT *
    2;

// ============================================================
// 全局状态
// ============================================================

static esp_lcd_panel_io_handle_t g_panel_io =
    nullptr;


static esp_lcd_panel_handle_t g_panel =
    nullptr;


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
// 鱼鹰 AM200Q460460LK 初始化序列（R.38.3 启动阶段仅将亮度改为 0）
// ============================================================
//
// 厂家原始初始化代码：
//
// FE 00
// C4 80
// 3A 55
// 35 00
// 53 20
// 51 00   （R.38.3：启动阶段保持暗屏，首帧完成后再恢复 60%）
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
    {0x00};

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


        // Panel IO 的 color-done callback 由 esp_lvgl_port 在注册显示设备时唯一拥有。
        // BoundedSPI 直接复用底层 SPI device，但 raw descriptor 会显式禁止触发该回调。
        io_config.on_color_trans_done =
            nullptr;


        io_config.user_ctx =
            nullptr;


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
    // R.38.3：硬件初始化完成后保持不可见
    // ========================================================
    // 厂家序列仍负责退出 Sleep，但 0x51 已固定为 0。这里再次确认亮度为 0，
    // 并关闭显示输出。直到 LVGL 第一帧完成，用户都不应看到未写入有效内容的 GRAM。

    ret = esp_lcd_panel_co5300_set_brightness(g_panel, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动暗屏亮度设置失败：%s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_disp_on_off(g_panel, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "关闭启动显示输出失败：%s", esp_err_to_name(ret));
        return ret;
    }

    g_present_output_enabled = false;


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
        "CO5300 AMOLED 初始化成功：启动阶段保持暗屏，等待 LVGL 首帧揭屏"
    );


    return ESP_OK;
}


// ============================================================
// 填充整个屏幕
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


esp_err_t display_reveal_after_first_frame()
{
    if (!g_ready || g_panel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_present_output_enabled) {
        return ESP_OK;
    }

    // 首帧已经完成，此时再恢复目标亮度并开启输出，避免任何默认 GRAM 白屏暴露。
    esp_err_t ret = esp_lcd_panel_co5300_set_brightness(g_panel, 60);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "首帧揭屏亮度设置失败：%s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_disp_on_off(g_panel, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "首帧揭屏开启输出失败：%s", esp_err_to_name(ret));
        (void)esp_lcd_panel_co5300_set_brightness(g_panel, 0);
        return ret;
    }

    g_present_output_enabled = true;
    ESP_LOGI(TAG, "R.38.3 首帧揭屏完成：输出=ON 亮度=60%%");
    return ESP_OK;
}


// ============================================================
// 全屏封面 Present Hold
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
