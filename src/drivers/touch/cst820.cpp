#include "cst820.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_pins.h"
#include "i2c_bus.h"
#include "app_diag_config.h"


static const char *TAG = "触摸";


// CST820 I2C 设备句柄
static i2c_master_dev_handle_t g_device = nullptr;


// 芯片 ID
static uint8_t g_chip_id = 0;


// 是否已经初始化成功
static bool g_ready = false;


// CST820 芯片 ID 寄存器
static constexpr uint8_t REG_CHIP_ID = 0xA7;
static constexpr uint8_t REG_GESTURE = 0x01;
static constexpr uint8_t REG_FINGER_NUM = 0x02;


static void cst820_rollback_device()
{
    g_ready = false;
    g_chip_id = 0;

    const esp_err_t cleanup_ret = i2c_bus_remove_device(&g_device);
    if (cleanup_ret != ESP_OK) {
        ESP_LOGW(TAG, "回滚 CST820 I2C 设备失败：%s", esp_err_to_name(cleanup_ret));
    }
}


// ============================================================
// 初始化 CST820
// ============================================================

esp_err_t cst820_init()
{
    if (g_ready) {
        return ESP_OK;
    }

#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(
        TAG,
        "正在初始化 CST820"
    );
#endif


    esp_err_t ret =
        i2c_bus_add_device(
            FAKEPOD_ADDR_CST820,
            &g_device
        );


    if (ret != ESP_OK) {

        ESP_LOGW(
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

        ESP_LOGW(
            TAG,
            "读取 CST820 芯片 ID 失败：%s",
            esp_err_to_name(ret)
        );

        cst820_rollback_device();
        return ret;
    }


#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(
        TAG,
        "CST820 芯片 ID：0x%02X",
        g_chip_id
    );
#endif


    if (g_chip_id != 0xB7) {

        ESP_LOGW(
            TAG,
            "CST820 芯片 ID 与预期值 0xB7 不一致"
        );
    }


    g_ready = true;


#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(
        TAG,
        "CST820 初始化成功"
    );
#endif


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

// ============================================================
// 读取触摸状态和原始坐标
// ============================================================

esp_err_t cst820_read_point(CST820Point *point)
{
    if (point == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    point->pressed = false;
    point->x = 0;
    point->y = 0;
    point->gesture = 0;

    if (!g_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2c_bus_read_reg8(
        g_device,
        REG_GESTURE,
        &point->gesture
    );

    if (ret != ESP_OK) {
        return ret;
    }

    // 从 0x02 连续读取：触点数、X高、X低、Y高、Y低。
    uint8_t data[5] = {};
    ret = i2c_bus_read_bytes(
        g_device,
        REG_FINGER_NUM,
        data,
        sizeof(data)
    );

    if (ret != ESP_OK) {
        return ret;
    }

    point->pressed = (data[0] & 0x0F) > 0;
    if (!point->pressed) {
        return ESP_OK;
    }

    point->x = static_cast<uint16_t>(
        ((data[1] & 0x0F) << 8) | data[2]
    );
    point->y = static_cast<uint16_t>(
        ((data[3] & 0x0F) << 8) | data[4]
    );

    return ESP_OK;
}
