#include "display.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/spi_master.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#include "esp_lcd_co5300.h"

#include "board_pins.h"


static const char *TAG = "显示";


// ============================================================
// 显示硬件参数
// ============================================================

// 使用 ESP32-S3 的 SPI2 外设驱动屏幕
static constexpr spi_host_device_t LCD_HOST =
    SPI2_HOST;


// 每次向屏幕发送 20 行像素
//
// 460 × 20 × RGB565(2字节)
// = 18400 字节
//
// 这样不需要一次申请完整 460×460 帧缓冲。
static constexpr int LCD_STRIP_HEIGHT =
    20;


static constexpr size_t LCD_STRIP_BUFFER_SIZE =
    FAKEPOD_LCD_WIDTH *
    LCD_STRIP_HEIGHT *
    2;


// ============================================================
// 全局状态
// ============================================================

static esp_lcd_panel_io_handle_t g_panel_io =
    nullptr;


static esp_lcd_panel_handle_t g_panel =
    nullptr;


static SemaphoreHandle_t g_tx_done =
    nullptr;


// DMA 条带缓冲
static uint8_t *g_strip_buffer =
    nullptr;


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

    g_tx_done =
        xSemaphoreCreateBinary();


    if (g_tx_done == nullptr) {

        ESP_LOGE(
            TAG,
            "创建屏幕 DMA 信号量失败"
        );

        return ESP_ERR_NO_MEM;
    }


    // ========================================================
    // 分配 DMA 条带缓冲
    // ========================================================

    g_strip_buffer =
        static_cast<uint8_t *>(
            heap_caps_malloc(
                LCD_STRIP_BUFFER_SIZE,
                MALLOC_CAP_DMA |
                MALLOC_CAP_INTERNAL
            )
        );


    if (g_strip_buffer == nullptr) {

        ESP_LOGE(
            TAG,
            "申请屏幕 DMA 缓冲失败，需要 %u 字节",
            static_cast<unsigned>(
                LCD_STRIP_BUFFER_SIZE
            )
        );

        return ESP_ERR_NO_MEM;
    }


    ESP_LOGI(
        TAG,
        "DMA 条带缓冲：%u 字节",
        static_cast<unsigned>(
            LCD_STRIP_BUFFER_SIZE
        )
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
            LCD_STRIP_BUFFER_SIZE;


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
        "QSPI 总线初始化成功"
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


        // CO5300 官方 QSPI 默认 40MHz
        io_config.pclk_hz =
            40 * 1000 * 1000;


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
    uint16_t color
)
{
    if (
        !g_ready ||
        g_panel == nullptr ||
        g_strip_buffer == nullptr
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
        LCD_STRIP_HEIGHT;


    for (
        int i = 0;
        i < pixel_count;
        ++i
    ) {

        g_strip_buffer[i * 2] =
            high;


        g_strip_buffer[i * 2 + 1] =
            low;
    }


    // ========================================================
    // 每次传输 20 行
    // ========================================================

    for (
        int y = 0;
        y < FAKEPOD_LCD_HEIGHT;
        y += LCD_STRIP_HEIGHT
    ) {

        // 清掉可能残留的完成信号
        xSemaphoreTake(
            g_tx_done,
            0
        );


        esp_err_t ret =
            esp_lcd_panel_draw_bitmap(
                g_panel,

                0,
                y,

                FAKEPOD_LCD_WIDTH,
                y + LCD_STRIP_HEIGHT,

                g_strip_buffer
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
            COLOR_RED
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
            COLOR_GREEN
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
            COLOR_BLUE
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
            COLOR_WHITE
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
            COLOR_DARK_BLUE
        )
    );


    ESP_LOGI(
        TAG,
        "颜色测试完成，屏幕保持深蓝色"
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

esp_lcd_panel_io_handle_t display_get_panel_io()
{
    return g_panel_io;
}

esp_lcd_panel_handle_t display_get_panel()
{
    return g_panel;
}
