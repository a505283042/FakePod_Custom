#include "cst820.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_pins.h"
#include "i2c_bus.h"


static const char *TAG = "触摸";


// CST820 I2C 设备句柄
static i2c_master_dev_handle_t g_device = nullptr;


// 芯片 ID
static uint8_t g_chip_id = 0;


// 是否已经初始化成功
static bool g_ready = false;


// CST820 芯片 ID 寄存器
static constexpr uint8_t REG_CHIP_ID = 0xA7;


// ============================================================
// 初始化 CST820
// ============================================================

esp_err_t cst820_init()
{
    ESP_LOGI(
        TAG,
        "正在初始化 CST820"
    );


    esp_err_t ret =
        i2c_bus_add_device(
            FAKEPOD_ADDR_CST820,
            &g_device
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "注册 CST820 失败：%s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    ret =
        i2c_bus_read_reg8(
            g_device,
            REG_CHIP_ID,
            &g_chip_id
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "读取 CST820 芯片 ID 失败：%s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    ESP_LOGI(
        TAG,
        "CST820 芯片 ID：0x%02X",
        g_chip_id
    );


    if (g_chip_id != 0xB7) {

        ESP_LOGW(
            TAG,
            "CST820 芯片 ID 与预期值 0xB7 不一致"
        );
    }


    g_ready = true;


    ESP_LOGI(
        TAG,
        "CST820 初始化成功"
    );


    return ESP_OK;
}


// ============================================================
// 获取芯片 ID
// ============================================================

uint8_t cst820_get_chip_id()
{
    return g_chip_id;
}


// ============================================================
// 获取初始化状态
// ============================================================

bool cst820_is_ready()
{
    return g_ready;
}