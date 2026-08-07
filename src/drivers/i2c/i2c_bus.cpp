#include "i2c_bus.h"

#include "esp_log.h"

#include "board_pins.h"


static const char *TAG = "I2C";


// I2C 主总线句柄
static i2c_master_bus_handle_t g_i2c_bus = nullptr;


// ============================================================
// 初始化 I2C 总线
// ============================================================

esp_err_t i2c_bus_init()
{
    if (g_i2c_bus != nullptr) {
        return ESP_OK;
    }


    ESP_LOGI(
        TAG,
        "正在初始化 I2C 总线"
    );

    ESP_LOGI(
        TAG,
        "SCL = GPIO%d，SDA = GPIO%d",
        FAKEPOD_I2C_SCL,
        FAKEPOD_I2C_SDA
    );


    i2c_master_bus_config_t config = {};

    config.i2c_port = I2C_NUM_0;

    config.sda_io_num =
        static_cast<gpio_num_t>(
            FAKEPOD_I2C_SDA
        );

    config.scl_io_num =
        static_cast<gpio_num_t>(
            FAKEPOD_I2C_SCL
        );

    config.clk_source =
        I2C_CLK_SRC_DEFAULT;

    config.glitch_ignore_cnt = 7;

    // PCB 上已有硬件上拉。
    // 开启内部弱上拉可提高调试阶段容错性。
    config.flags.enable_internal_pullup = true;


    esp_err_t ret =
        i2c_new_master_bus(
            &config,
            &g_i2c_bus
        );


    if (ret == ESP_OK) {

        ESP_LOGI(
            TAG,
            "I2C 总线初始化成功"
        );

    } else {

        ESP_LOGE(
            TAG,
            "I2C 总线初始化失败：%s",
            esp_err_to_name(ret)
        );
    }


    return ret;
}


// ============================================================
// 扫描 I2C 总线
// ============================================================

void i2c_bus_scan()
{
    if (g_i2c_bus == nullptr) {

        ESP_LOGE(
            TAG,
            "I2C 尚未初始化，无法扫描"
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "开始扫描 I2C 设备"
    );


    int found = 0;


    for (
        uint8_t address = 0x08;
        address <= 0x77;
        ++address
    ) {

        esp_err_t ret =
            i2c_master_probe(
                g_i2c_bus,
                address,
                20
            );


        if (ret == ESP_OK) {

            const char *name =
                "未知设备";


            if (
                address ==
                FAKEPOD_ADDR_CST820
            ) {

                name = "CST820 触摸";

            }
            else if (
                address ==
                FAKEPOD_ADDR_QMI8658
            ) {

                name = "QMI8658 六轴";
            }


            ESP_LOGI(
                TAG,
                "发现设备：0x%02X [%s]",
                address,
                name
            );


            found++;
        }
    }


    ESP_LOGI(
        TAG,
        "I2C 扫描完成，共发现 %d 个设备",
        found
    );
}


// ============================================================
// 注册 I2C 设备
// ============================================================

esp_err_t i2c_bus_add_device(
    uint8_t address,
    i2c_master_dev_handle_t *handle
)
{
    if (
        g_i2c_bus == nullptr ||
        handle == nullptr
    ) {

        return ESP_ERR_INVALID_ARG;
    }


    i2c_device_config_t config = {};

    config.dev_addr_length =
        I2C_ADDR_BIT_LEN_7;

    config.device_address =
        address;

    config.scl_speed_hz =
        400000;


    return i2c_master_bus_add_device(
        g_i2c_bus,
        &config,
        handle
    );
}


// ============================================================
// 读取单字节寄存器
// ============================================================

esp_err_t i2c_bus_read_reg8(
    i2c_master_dev_handle_t device,
    uint8_t reg,
    uint8_t *value
)
{
    if (
        device == nullptr ||
        value == nullptr
    ) {

        return ESP_ERR_INVALID_ARG;
    }


    return i2c_master_transmit_receive(
        device,
        &reg,
        1,
        value,
        1,
        100
    );
}


// ============================================================
// 写入单字节寄存器
// ============================================================

esp_err_t i2c_bus_write_reg8(
    i2c_master_dev_handle_t device,
    uint8_t reg,
    uint8_t value
)
{
    if (device == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }


    uint8_t data[2] = {
        reg,
        value
    };


    return i2c_master_transmit(
        device,
        data,
        sizeof(data),
        100
    );
}