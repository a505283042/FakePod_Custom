#include "cst820.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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


// CST820 上电后必须经过一次硬复位才能进入正常上报模式；
// 未复位时芯片可能停留在上电随机状态并偶发上报虚假触点。
static void cst820_hardware_reset()
{
#if defined(FAKEPOD_TOUCH_RST) && FAKEPOD_TOUCH_RST >= 0
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << FAKEPOD_TOUCH_RST;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGW(TAG, "配置触摸 RST(IO%d) 失败", FAKEPOD_TOUCH_RST);
        return;
    }
    gpio_set_level(static_cast<gpio_num_t>(FAKEPOD_TOUCH_RST), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(FAKEPOD_TOUCH_RST), 1);
    // 复位释放后给 CST820 固件留出启动时间再访问 I2C。
    vTaskDelay(pdMS_TO_TICKS(80));
#endif
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


    cst820_hardware_reset();


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
