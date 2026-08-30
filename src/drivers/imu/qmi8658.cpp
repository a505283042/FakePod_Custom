#include "qmi8658.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_pins.h"
#include "i2c_bus.h"
#include "app_diag_config.h"


static const char *TAG = "IMU";


static i2c_master_dev_handle_t g_device =
    nullptr;


static uint8_t g_device_id = 0;
static uint8_t g_revision = 0;


static bool g_ready = false;


// QMI8658 寄存器
static constexpr uint8_t REG_WHO_AM_I =
    0x00;

static constexpr uint8_t REG_REVISION_ID =
    0x01;


static void qmi8658_rollback_device()
{
    g_ready = false;
    g_device_id = 0;
    g_revision = 0;

    const esp_err_t cleanup_ret = i2c_bus_remove_device(&g_device);
    if (cleanup_ret != ESP_OK) {
        ESP_LOGW(TAG, "回滚 QMI8658 I2C 设备失败：%s", esp_err_to_name(cleanup_ret));
    }
}


// ============================================================
// 初始化 QMI8658
// ============================================================

esp_err_t qmi8658_init()
{
    if (g_ready) {
        return ESP_OK;
    }

#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(
        TAG,
        "正在初始化 QMI8658"
    );
#endif


    esp_err_t ret =
        i2c_bus_add_device(
            FAKEPOD_ADDR_QMI8658,
            &g_device
        );


    if (ret != ESP_OK) {

        ESP_LOGW(
            TAG,
            "注册 QMI8658 失败：%s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    ret =
        i2c_bus_read_reg8(
            g_device,
            REG_WHO_AM_I,
            &g_device_id
        );


    if (ret != ESP_OK) {

        ESP_LOGW(
            TAG,
            "读取 WHO_AM_I 失败：%s",
            esp_err_to_name(ret)
        );

        qmi8658_rollback_device();
        return ret;
    }


    ret =
        i2c_bus_read_reg8(
            g_device,
            REG_REVISION_ID,
            &g_revision
        );


    if (ret != ESP_OK) {

        ESP_LOGW(
            TAG,
            "读取 REVISION_ID 失败：%s",
            esp_err_to_name(ret)
        );

        qmi8658_rollback_device();
        return ret;
    }


#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(
        TAG,
        "设备 ID：0x%02X",
        g_device_id
    );
#endif


#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(
        TAG,
        "芯片版本：0x%02X",
        g_revision
    );
#endif


    if (g_device_id != 0x05) {

        ESP_LOGW(
            TAG,
            "QMI8658 设备 ID 与预期值 0x05 不一致"
        );
    }


    g_ready = true;


#if APP_DIAG_BOOT_VERBOSE
    ESP_LOGI(
        TAG,
        "QMI8658 初始化成功"
    );
#endif


    return ESP_OK;
}


// ============================================================
// 获取设备 ID
// ============================================================

uint8_t qmi8658_get_device_id()
{
    return g_device_id;
}


// ============================================================
// 获取版本 ID
// ============================================================

uint8_t qmi8658_get_revision()
{
    return g_revision;
}


// ============================================================
// 获取初始化状态
// ============================================================

bool qmi8658_is_ready()
{
    return g_ready;
}