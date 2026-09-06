#include "qmi8658.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
static bool g_motion_profile_ready = false;


// QMI8658 寄存器
static constexpr uint8_t REG_WHO_AM_I =
    0x00;

static constexpr uint8_t REG_REVISION_ID =
    0x01;

static constexpr uint8_t REG_CTRL1 = 0x02;
static constexpr uint8_t REG_CTRL2 = 0x03;
static constexpr uint8_t REG_CTRL3 = 0x04;
static constexpr uint8_t REG_CTRL7 = 0x08;
static constexpr uint8_t REG_CTRL8 = 0x09;
static constexpr uint8_t REG_CTRL9 = 0x0A;
static constexpr uint8_t REG_CAL1_L = 0x0B;
static constexpr uint8_t REG_STATUSINT = 0x2D;
static constexpr uint8_t REG_STATUS1 = 0x2F;
static constexpr uint8_t REG_AX_L = 0x35;
static constexpr uint8_t REG_TAP_STATUS = 0x59;

static constexpr uint8_t CTRL1_ADDR_AUTO_INCREMENT = 0x40;
static constexpr uint8_t CTRL7_DRDY_DISABLE = 0x20;
static constexpr uint8_t CTRL7_ACCEL_ENABLE = 0x01;
static constexpr uint8_t CTRL7_GYRO_ENABLE = 0x02;
static constexpr uint8_t CTRL8_CTRL9_STATUS_HANDSHAKE = 0x80;
static constexpr uint8_t CTRL8_ACTIVITY_TO_INT1 = 0x40;
static constexpr uint8_t CTRL8_TAP_ENABLE = 0x01;
static constexpr uint8_t STATUSINT_CTRL9_DONE = 0x80;
static constexpr uint8_t STATUS1_TAP = 0x02;
static constexpr uint8_t CTRL9_ACK = 0x00;
static constexpr uint8_t CTRL9_CONFIGURE_TAP = 0x0C;

// Motion Controls V2: Accel ±4g + Gyro ±512dps.
// With both sensors enabled, CTRL2 setting 0x4 yields ~448.4Hz accel ODR;
// CTRL3 setting 0x4 yields ~448.4Hz gyro ODR. Host reads the contiguous
// AX..GZ window at 100Hz, which is sufficient for the wrist-flip gesture.
static constexpr uint8_t MOTION_CTRL2 = 0x14;
static constexpr uint8_t MOTION_CTRL3 = 0x54;
static constexpr int32_t MOTION_ACCEL_RANGE_MG = 4000;
static constexpr int32_t MOTION_GYRO_RANGE_MDPS = 512000;
static constexpr uint32_t MOTION_ACCEL_ODR_HZ = 448U;

// Reference tap profile: physical time is converted to samples for the selected accelerometer ODR.
static constexpr uint16_t TAP_PEAK_WINDOW_MS = 20U;
static constexpr uint16_t TAP_WINDOW_MS = 50U;
static constexpr uint16_t TAP_DOUBLE_WINDOW_MS = 250U;
static constexpr uint8_t TAP_PRIORITY_Z_X_Y = 4U;
static constexpr uint16_t TAP_ALPHA_X10000 = 625U;
static constexpr uint16_t TAP_GAMMA_X10000 = 2500U;
static constexpr uint16_t TAP_PEAK_THRESHOLD_MG = 800U;
static constexpr uint16_t TAP_QUIET_THRESHOLD_MG = 400U;

static constexpr TickType_t CTRL9_TIMEOUT_TICKS = pdMS_TO_TICKS(1000);


static void qmi8658_rollback_device()
{
    g_ready = false;
    g_motion_profile_ready = false;
    g_device_id = 0;
    g_revision = 0;

    const esp_err_t cleanup_ret = i2c_bus_remove_device(&g_device);
    if (cleanup_ret != ESP_OK) {
        ESP_LOGW(TAG, "回滚 QMI8658 I2C 设备失败：%s", esp_err_to_name(cleanup_ret));
    }
}


static uint16_t qmi8658_ms_to_samples(uint16_t milliseconds)
{
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(milliseconds) * MOTION_ACCEL_ODR_HZ) / 1000U);
}


static uint16_t qmi8658_scale_ratio(uint16_t ratio_x10000)
{
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(ratio_x10000) * 128U) / 10000U);
}


static uint16_t qmi8658_scale_threshold(uint16_t threshold_mg)
{
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(threshold_mg) * 1024U) / 1000U);
}


static esp_err_t qmi8658_write_cal_set(const uint8_t values[8])
{
    if (g_device == nullptr || values == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // QMI8658C does not support burst writes to configuration registers.
    for (uint8_t i = 0; i < 8U; ++i) {
        const esp_err_t ret = i2c_bus_write_reg8(
            g_device,
            static_cast<uint8_t>(REG_CAL1_L + i),
            values[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}


static esp_err_t qmi8658_ctrl9_command(uint8_t command)
{
    esp_err_t ret = i2c_bus_write_reg8(g_device, REG_CTRL9, command);
    if (ret != ESP_OK) {
        return ret;
    }

    const TickType_t started = xTaskGetTickCount();
    while (true) {
        uint8_t status = 0;
        ret = i2c_bus_read_reg8(g_device, REG_STATUSINT, &status);
        if (ret != ESP_OK) {
            return ret;
        }
        if ((status & STATUSINT_CTRL9_DONE) != 0U) {
            break;
        }
        if (xTaskGetTickCount() - started >= CTRL9_TIMEOUT_TICKS) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    ret = i2c_bus_write_reg8(g_device, REG_CTRL9, CTRL9_ACK);
    if (ret != ESP_OK) {
        return ret;
    }

    for (uint8_t attempt = 0; attempt < 10U; ++attempt) {
        uint8_t status = 0;
        ret = i2c_bus_read_reg8(g_device, REG_STATUSINT, &status);
        if (ret != ESP_OK) {
            return ret;
        }
        if ((status & STATUSINT_CTRL9_DONE) == 0U) {
            return ESP_OK;
        }
        vTaskDelay(1);
    }

    return ESP_ERR_TIMEOUT;
}


static esp_err_t qmi8658_configure_tap_engine()
{
    uint8_t first[8] = {};
    first[0] = static_cast<uint8_t>(qmi8658_ms_to_samples(TAP_PEAK_WINDOW_MS));
    first[1] = TAP_PRIORITY_Z_X_Y;

    const uint16_t tap_window = qmi8658_ms_to_samples(TAP_WINDOW_MS);
    first[2] = static_cast<uint8_t>(tap_window & 0xFFU);
    first[3] = static_cast<uint8_t>((tap_window >> 8) & 0xFFU);

    const uint16_t double_window = qmi8658_ms_to_samples(TAP_DOUBLE_WINDOW_MS);
    first[4] = static_cast<uint8_t>(double_window & 0xFFU);
    first[5] = static_cast<uint8_t>((double_window >> 8) & 0xFFU);
    first[6] = 0U;
    first[7] = 0x01U;

    esp_err_t ret = qmi8658_write_cal_set(first);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = qmi8658_ctrl9_command(CTRL9_CONFIGURE_TAP);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t second[8] = {};
    second[0] = static_cast<uint8_t>(qmi8658_scale_ratio(TAP_ALPHA_X10000));
    second[1] = static_cast<uint8_t>(qmi8658_scale_ratio(TAP_GAMMA_X10000));

    const uint16_t peak_threshold = qmi8658_scale_threshold(TAP_PEAK_THRESHOLD_MG);
    second[2] = static_cast<uint8_t>(peak_threshold & 0xFFU);
    second[3] = static_cast<uint8_t>((peak_threshold >> 8) & 0xFFU);

    const uint16_t quiet_threshold = qmi8658_scale_threshold(TAP_QUIET_THRESHOLD_MG);
    second[4] = static_cast<uint8_t>(quiet_threshold & 0xFFU);
    second[5] = static_cast<uint8_t>((quiet_threshold >> 8) & 0xFFU);
    second[6] = 0U;
    second[7] = 0x02U;

    ret = qmi8658_write_cal_set(second);
    if (ret != ESP_OK) {
        return ret;
    }
    return qmi8658_ctrl9_command(CTRL9_CONFIGURE_TAP);
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
// Motion Controls V1
// ============================================================

esp_err_t qmi8658_configure_motion_profile()
{
    if (!g_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_motion_profile_ready) {
        return ESP_OK;
    }

    // Tap configuration is only valid while accelerometer and gyroscope are disabled.
    esp_err_t ret = i2c_bus_write_reg8(g_device, REG_CTRL7, 0x00U);
    if (ret != ESP_OK) {
        return ret;
    }

    // Address auto-increment + little-endian data for the 6-byte AX_L burst read.
    ret = i2c_bus_write_reg8(g_device, REG_CTRL1, CTRL1_ADDR_AUTO_INCREMENT);
    if (ret != ESP_OK) {
        return ret;
    }

    // Activity/Tap -> INT1. CTRL9 handshake stays on STATUSINT so setup cannot pulse GPIO47.
    ret = i2c_bus_write_reg8(
        g_device,
        REG_CTRL8,
        static_cast<uint8_t>(CTRL8_CTRL9_STATUS_HANDSHAKE | CTRL8_ACTIVITY_TO_INT1));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_bus_write_reg8(g_device, REG_CTRL2, MOTION_CTRL2);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2c_bus_write_reg8(g_device, REG_CTRL3, MOTION_CTRL3);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = qmi8658_configure_tap_engine();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Tap Engine 参数配置失败：%s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_bus_write_reg8(
        g_device,
        REG_CTRL8,
        static_cast<uint8_t>(
            CTRL8_CTRL9_STATUS_HANDSHAKE |
            CTRL8_ACTIVITY_TO_INT1 |
            CTRL8_TAP_ENABLE));
    if (ret != ESP_OK) {
        return ret;
    }

    // Accelerometer + Gyroscope; DRDY remains blocked from unused INT2.
    ret = i2c_bus_write_reg8(
        g_device,
        REG_CTRL7,
        static_cast<uint8_t>(
            CTRL7_DRDY_DISABLE | CTRL7_GYRO_ENABLE | CTRL7_ACCEL_ENABLE));
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(1);

    // Clear stale Activity state before MotionService registers the GPIO47 ISR.
    uint8_t stale_status = 0;
    (void)i2c_bus_read_reg8(g_device, REG_STATUS1, &stale_status);

    g_motion_profile_ready = true;
    ESP_LOGI(
        TAG,
        "Motion profile就绪：Accel≈448Hz ±4g Gyro≈448Hz ±512dps Tap=ON ActivityINT=INT1");
    return ESP_OK;
}


esp_err_t qmi8658_read_accel(Qmi8658AccelSample *out_sample)
{
    if (out_sample == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_sample = {};

    if (!g_ready || !g_motion_profile_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[6] = {};
    const esp_err_t ret = i2c_bus_read_bytes(g_device, REG_AX_L, data, sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    out_sample->raw_x = static_cast<int16_t>(
        static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
    out_sample->raw_y = static_cast<int16_t>(
        static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8));
    out_sample->raw_z = static_cast<int16_t>(
        static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8));

    out_sample->mg_x = static_cast<int16_t>(
        (static_cast<int32_t>(out_sample->raw_x) * MOTION_ACCEL_RANGE_MG) / 32768);
    out_sample->mg_y = static_cast<int16_t>(
        (static_cast<int32_t>(out_sample->raw_y) * MOTION_ACCEL_RANGE_MG) / 32768);
    out_sample->mg_z = static_cast<int16_t>(
        (static_cast<int32_t>(out_sample->raw_z) * MOTION_ACCEL_RANGE_MG) / 32768);

    return ESP_OK;
}


esp_err_t qmi8658_read_motion(
    Qmi8658AccelSample *out_accel,
    Qmi8658GyroSample *out_gyro)
{
    if (out_accel == nullptr || out_gyro == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_accel = {};
    *out_gyro = {};

    if (!g_ready || !g_motion_profile_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // AX_L..GZ_H are contiguous when CTRL1.ADDR_AI=1. One 12-byte transaction
    // keeps the shared TP/IMU I2C bus lighter than separate accel + gyro reads.
    uint8_t data[12] = {};
    const esp_err_t ret = i2c_bus_read_bytes(g_device, REG_AX_L, data, sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    out_accel->raw_x = static_cast<int16_t>(
        static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
    out_accel->raw_y = static_cast<int16_t>(
        static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8));
    out_accel->raw_z = static_cast<int16_t>(
        static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8));

    out_accel->mg_x = static_cast<int16_t>(
        (static_cast<int32_t>(out_accel->raw_x) * MOTION_ACCEL_RANGE_MG) / 32768);
    out_accel->mg_y = static_cast<int16_t>(
        (static_cast<int32_t>(out_accel->raw_y) * MOTION_ACCEL_RANGE_MG) / 32768);
    out_accel->mg_z = static_cast<int16_t>(
        (static_cast<int32_t>(out_accel->raw_z) * MOTION_ACCEL_RANGE_MG) / 32768);

    out_gyro->raw_x = static_cast<int16_t>(
        static_cast<uint16_t>(data[6]) | (static_cast<uint16_t>(data[7]) << 8));
    out_gyro->raw_y = static_cast<int16_t>(
        static_cast<uint16_t>(data[8]) | (static_cast<uint16_t>(data[9]) << 8));
    out_gyro->raw_z = static_cast<int16_t>(
        static_cast<uint16_t>(data[10]) | (static_cast<uint16_t>(data[11]) << 8));

    out_gyro->mdps_x = static_cast<int32_t>(
        (static_cast<int64_t>(out_gyro->raw_x) * MOTION_GYRO_RANGE_MDPS) / 32768);
    out_gyro->mdps_y = static_cast<int32_t>(
        (static_cast<int64_t>(out_gyro->raw_y) * MOTION_GYRO_RANGE_MDPS) / 32768);
    out_gyro->mdps_z = static_cast<int32_t>(
        (static_cast<int64_t>(out_gyro->raw_z) * MOTION_GYRO_RANGE_MDPS) / 32768);

    return ESP_OK;
}


esp_err_t qmi8658_read_tap_event(Qmi8658TapEvent *out_event)
{
    if (out_event == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_event = {};

    if (!g_ready || !g_motion_profile_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t status1 = 0;
    esp_err_t ret = i2c_bus_read_reg8(g_device, REG_STATUS1, &status1);
    if (ret != ESP_OK) {
        return ret;
    }
    if ((status1 & STATUS1_TAP) == 0U) {
        return ESP_OK;
    }

    uint8_t tap_status = 0;
    ret = i2c_bus_read_reg8(g_device, REG_TAP_STATUS, &tap_status);
    if (ret != ESP_OK) {
        return ret;
    }

    out_event->count = static_cast<uint8_t>(tap_status & 0x03U);
    out_event->axis = static_cast<uint8_t>((tap_status >> 4) & 0x03U);
    out_event->negative = (tap_status & 0x80U) != 0U;
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
