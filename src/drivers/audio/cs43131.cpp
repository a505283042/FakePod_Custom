#include "cs43131.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "board_pins.h"
#include "i2c_bus.h"
#include "../../audio/audio_rate_profile.h"

static const char *TAG = "DAC";
static i2c_master_dev_handle_t g_device = nullptr;
static bool g_ready = false;
static uint8_t g_revision = 0;
static uint8_t g_subrevision = 0;
static uint8_t g_i2c_address = 0;

static constexpr uint32_t REG_DEVICE_ID_AB = 0x010000;
static constexpr uint32_t REG_DEVICE_ID_CD = 0x010001;
static constexpr uint32_t REG_DEVICE_ID_E = 0x010002;
static constexpr uint32_t REG_REVISION_ID = 0x010004;
static constexpr uint32_t REG_SUBREVISION_ID = 0x010005;
static constexpr uint32_t REG_SYSTEM_CLOCKING = 0x010006;
static constexpr uint32_t REG_POWER_DOWN = 0x020000;
static constexpr uint32_t REG_CRYSTAL_SETTING = 0x020052;
static constexpr uint32_t REG_ASP_SAMPLE_RATE = 0x01000B;
static constexpr uint32_t REG_ASP_SAMPLE_BITS = 0x01000C;
static constexpr uint32_t REG_ASP_N_LSB = 0x040010;
static constexpr uint32_t REG_ASP_N_MSB = 0x040011;
static constexpr uint32_t REG_ASP_M_LSB = 0x040012;
static constexpr uint32_t REG_ASP_M_MSB = 0x040013;
static constexpr uint32_t REG_ASP_LCHI_LSB = 0x040014;
static constexpr uint32_t REG_ASP_LCHI_MSB = 0x040015;
static constexpr uint32_t REG_ASP_LCPR_LSB = 0x040016;
static constexpr uint32_t REG_ASP_LCPR_MSB = 0x040017;
static constexpr uint32_t REG_ASP_CLOCK_CONFIG = 0x040018;
static constexpr uint32_t REG_ASP_FRAME_CONFIG = 0x040019;
static constexpr uint32_t REG_ASP_CH1_LOCATION = 0x050000;
static constexpr uint32_t REG_ASP_CH2_LOCATION = 0x050001;
static constexpr uint32_t REG_ASP_CH1_SIZE_ENABLE = 0x05000A;
static constexpr uint32_t REG_ASP_CH2_SIZE_ENABLE = 0x05000B;
static constexpr uint32_t REG_INTERRUPT_STATUS_1 = 0x0F0000;
static constexpr uint32_t REG_INTERRUPT_STATUS_2 = 0x0F0001;
static constexpr uint32_t REG_INTERRUPT_MASK_1 = 0x0F0010;
static constexpr uint32_t REG_HP_OUTPUT_CONTROL = 0x080000;
static constexpr uint32_t REG_POP_FREE_POWER_UP_2 = 0x080032;
static constexpr uint32_t REG_PCM_FILTER_OPTION = 0x090000;
static constexpr uint32_t REG_PCM_VOLUME_B = 0x090001;
static constexpr uint32_t REG_PCM_VOLUME_A = 0x090002;
static constexpr uint32_t REG_PCM_PATH_CONTROL_1 = 0x090003;
static constexpr uint32_t REG_PCM_PATH_CONTROL_2 = 0x090004;
static constexpr uint32_t REG_CLASS_H_CONTROL = 0x0B0000;
static constexpr uint32_t REG_POP_FREE_POWER_UP_1 = 0x010010;

static constexpr uint8_t PCM_TEST_VOLUME_MINUS_20_DB = 0x28;
static constexpr uint8_t PCM_PATH_SOFT_RAMP_MUTED = 0xEF;
static constexpr uint8_t PCM_PATH_SOFT_RAMP_UNMUTED = 0xEC;
static constexpr int HP_PDN_DONE_WAIT_MS = 100;

static esp_err_t cs43131_hardware_reset()
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << FAKEPOD_DAC_RST;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK) {
        return ret;
    }

    // RESET 为低有效。先保持复位，再释放并等待控制口完成上电。
    gpio_set_level(static_cast<gpio_num_t>(FAKEPOD_DAC_RST), 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(static_cast<gpio_num_t>(FAKEPOD_DAC_RST), 1);
    vTaskDelay(pdMS_TO_TICKS(3));
    return ESP_OK;
}

esp_err_t cs43131_read_reg(uint32_t address, uint8_t *value)
{
    if (g_device == nullptr || value == nullptr || address > 0xFFFFFF) {
        return ESP_ERR_INVALID_ARG;
    }

    // CS43131 使用 24 位 MAP 地址，控制字节 0x00 表示 8 位访问且不自增。
    uint8_t preamble[4] = {
        static_cast<uint8_t>((address >> 16) & 0xFF),
        static_cast<uint8_t>((address >> 8) & 0xFF),
        static_cast<uint8_t>(address & 0xFF),
        0x00
    };

    esp_err_t ret = i2c_master_transmit(g_device, preamble, sizeof(preamble), 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MAP前导写失败：地址=0x%06lX，I2C=0x%02X，错误=%s",
            static_cast<unsigned long>(address), g_i2c_address, esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_master_receive(g_device, value, 1, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "寄存器数据读取失败：地址=0x%06lX，I2C=0x%02X，错误=%s",
            static_cast<unsigned long>(address), g_i2c_address, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t cs43131_write_reg(uint32_t address, uint8_t value)
{
    if (g_device == nullptr || address > 0xFFFFFF) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[5] = {
        static_cast<uint8_t>((address >> 16) & 0xFF),
        static_cast<uint8_t>((address >> 8) & 0xFF),
        static_cast<uint8_t>(address & 0xFF),
        0x00,
        value
    };
    return i2c_master_transmit(g_device, data, sizeof(data), 100);
}

esp_err_t cs43131_init()
{
    if (g_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "正在初始化 CS43131 控制口");
    ESP_LOGI(TAG, "RESET=GPIO%d，I2C地址=0x%02X", FAKEPOD_DAC_RST, FAKEPOD_ADDR_CS43131);
    ESP_LOGI(TAG, "音频引脚：BCLK=GPIO%d LRCK=GPIO%d SDIN=GPIO%d，MCLK=板载24.576MHz晶振",
        FAKEPOD_I2S_BCLK, FAKEPOD_I2S_LRCK, FAKEPOD_I2S_DOUT);

    esp_err_t ret = cs43131_hardware_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CS43131 硬件复位失败：%s", esp_err_to_name(ret));
        return ret;
    }

    // ADR 电阻决定地址低两位，CS43131 的合法地址范围为 0x30~0x33。
    // Bring-up 阶段不再假定地址，复位后直接探测真实响应地址。
    ESP_LOGI(TAG, "正在探测 CS43131 I2C 地址：0x30~0x33");
    for (uint8_t address = 0x30; address <= 0x33; ++address) {
        esp_err_t probe_ret = i2c_bus_probe_address(address, 20);
        if (probe_ret == ESP_OK) {
            g_i2c_address = address;
            ESP_LOGI(TAG, "发现 CS43131 候选地址：0x%02X", address);
            break;
        }
    }

    if (g_i2c_address == 0) {
        ESP_LOGE(TAG, "0x30~0x33 均无设备响应，请重点检查 RESET 引脚、DAC 供电和 ADR 硬件连接");
        return ESP_ERR_NOT_FOUND;
    }

    // 控制口 Bring-up 先使用 100kHz，提高初次通信的时序裕量。
    ret = i2c_bus_add_device_at_speed(g_i2c_address, 100000, &g_device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 CS43131 I2C 设备失败：%s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "CS43131 控制口使用地址=0x%02X，速率=100kHz", g_i2c_address);

    uint8_t id_ab = 0;
    uint8_t id_cd = 0;
    uint8_t id_e = 0;
    ret = cs43131_read_reg(REG_DEVICE_ID_AB, &id_ab);
    if (ret == ESP_OK) ret = cs43131_read_reg(REG_DEVICE_ID_CD, &id_cd);
    if (ret == ESP_OK) ret = cs43131_read_reg(REG_DEVICE_ID_E, &id_e);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取 CS43131 设备 ID 失败：%s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "设备 ID：0x%02X 0x%02X 0x%02X", id_ab, id_cd, id_e);
    if (id_ab != 0x43 || id_cd != 0x13 || id_e != 0x10) {
        ESP_LOGE(TAG, "设备 ID 与 CS43131 预期值 43 13 10 不一致");
        return ESP_ERR_NOT_FOUND;
    }

    ret = cs43131_read_reg(REG_REVISION_ID, &g_revision);
    if (ret == ESP_OK) ret = cs43131_read_reg(REG_SUBREVISION_ID, &g_subrevision);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取 CS43131 修订号失败：%s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t system_clocking = 0;
    uint8_t power_down = 0;
    esp_err_t clock_ret = cs43131_read_reg(REG_SYSTEM_CLOCKING, &system_clocking);
    esp_err_t power_ret = cs43131_read_reg(REG_POWER_DOWN, &power_down);

    ESP_LOGI(TAG, "修订号：0x%02X，子修订号：0x%02X", g_revision, g_subrevision);
    if (clock_ret == ESP_OK) {
        ESP_LOGI(TAG, "系统时钟控制复位值：0x%02X", system_clocking);
    }
    if (power_ret == ESP_OK) {
        ESP_LOGI(TAG, "电源控制复位值：0x%02X（保持全部音频模块关闭）", power_down);
    }

    g_ready = true;
    ESP_LOGI(TAG, "CS43131 控制口验证成功，本阶段未开启耳放和音频输出");
    return ESP_OK;
}

esp_err_t cs43131_prepare_pcm_playback_32bit(uint32_t sample_rate_hz)
{
    if (!g_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    AudioRateProfile rate_profile = {};
    if (!audio_rate_profile_get(sample_rate_hz, &rate_profile)) {
        ESP_LOGE(TAG, "CS43131 PCM 采样率不在 FakePod Rate Profile：收到=%luHz",
            static_cast<unsigned long>(sample_rate_hz));
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "正在准备 CS43131 ASP：%luHz / I2S / 32bit slot / Slave",
        static_cast<unsigned long>(sample_rate_hz));

    // 先读取一次中断状态，清除上电阶段可能残留的 sticky 状态。
    uint8_t status1 = 0;
    cs43131_read_reg(REG_INTERRUPT_STATUS_1, &status1);

    // 板载无源晶振为 24.576MHz。CS43131 数据手册中 River 24.576MHz
    // 参考晶振对应 7.5uA 偏置，即 Crystal Setting = 0x06。
    esp_err_t ret = cs43131_write_reg(REG_CRYSTAL_SETTING, 0x06);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "配置 24.576MHz 晶振偏置失败：%s", esp_err_to_name(ret));
        return ret;
    }

    // XTAL_READY/XTAL_ERROR 是 sticky interrupt status。数据手册要求在
    // 启动晶振之前先清状态，再解除这两个中断的 mask，否则可能读不到 READY。
    status1 = 0;
    ret = cs43131_read_reg(REG_INTERRUPT_STATUS_1, &status1);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t mask1 = 0;
    ret = cs43131_read_reg(REG_INTERRUPT_MASK_1, &mask1);
    if (ret != ESP_OK) {
        return ret;
    }

    mask1 &= 0xE7;
    ret = cs43131_write_reg(REG_INTERRUPT_MASK_1, mask1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "解除 XTAL_READY/XTAL_ERROR 中断屏蔽失败：%s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t power = 0;
    ret = cs43131_read_reg(REG_POWER_DOWN, &power);
    if (ret != ESP_OK) {
        return ret;
    }

    // 只启动 XTAL，耳放和 ASP 仍保持关闭。复位值 0xFE 清除 PDN_XTAL 后应为 0xF6。
    power &= 0xF6;
    ret = cs43131_write_reg(REG_POWER_DOWN, power);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动 CS43131 晶振失败：%s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "XTAL配置：BIAS=0x06，MASK1=0x%02X，POWER_DOWN=0x%02X", mask1, power);

    // XTAL_READY/XTAL_ERROR 为 sticky 状态。轮询最多 20ms，
    // 比数据手册给出的启动时间留出更充分裕量。
    bool xtal_ready = false;
    uint8_t last_status1 = 0;

    for (int attempt = 0; attempt < 20; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(1));

        status1 = 0;
        ret = cs43131_read_reg(REG_INTERRUPT_STATUS_1, &status1);
        if (ret != ESP_OK) {
            return ret;
        }

        last_status1 = status1;

        if ((status1 & 0x08) != 0) {
            ESP_LOGE(TAG, "CS43131 报告晶振启动错误：INT_STATUS1=0x%02X", status1);
            return ESP_FAIL;
        }

        if ((status1 & 0x10) != 0) {
            xtal_ready = true;
            ESP_LOGI(TAG, "24.576MHz 晶振已就绪：等待=%dms，INT_STATUS1=0x%02X", attempt + 1, status1);
            break;
        }
    }

    if (!xtal_ready) {
        ESP_LOGE(TAG, "CS43131 未报告 XTAL_READY：等待20ms，最后INT_STATUS1=0x%02X", last_status1);
        return ESP_ERR_TIMEOUT;
    }

    // 选择 Direct MCLK/XTAL，内部 MCLK 设为 24.576MHz。
    ret = cs43131_write_reg(REG_SYSTEM_CLOCKING, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "切换到 24.576MHz 直接时钟失败：%s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    struct RegValue {
        uint32_t reg;
        uint8_t value;
    };

    // 44.1~192kHz Rate Profile 都使用板载 24.576MHz XTAL 作为内部 MCLK。
    // 数据手册明确说明除 384kHz Master Mode 外，其余采样率可使用 22.5792 或 24.576MHz MCLK_INT。
    // CS43131 保持 Slave，BCLK/LRCK 由 ESP32-S3 产生。
    const uint8_t sample_rate_reg = rate_profile.cs43131_asp_sprate;
    const RegValue config[] = {
        {REG_ASP_SAMPLE_RATE, sample_rate_reg},
        {REG_ASP_SAMPLE_BITS, 0x04},
        {REG_ASP_N_LSB, 0x01},
        {REG_ASP_N_MSB, 0x00},
        {REG_ASP_M_LSB, 0x08},
        {REG_ASP_M_MSB, 0x00},
        {REG_ASP_LCHI_LSB, 0x1F},
        {REG_ASP_LCHI_MSB, 0x00},
        {REG_ASP_LCPR_LSB, 0x3F},
        {REG_ASP_LCPR_MSB, 0x00},
        {REG_ASP_CLOCK_CONFIG, 0x0C},
        {REG_ASP_FRAME_CONFIG, 0x0A},
        {REG_ASP_CH1_LOCATION, 0x00},
        {REG_ASP_CH2_LOCATION, 0x00},
        {REG_ASP_CH1_SIZE_ENABLE, 0x07},
        {REG_ASP_CH2_SIZE_ENABLE, 0x0F},
    };

    for (const auto &item : config) {
        ret = cs43131_write_reg(item.reg, item.value);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "配置 ASP 寄存器 0x%06lX 失败：%s",
                static_cast<unsigned long>(item.reg), esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "CS43131 ASP 参数配置完成：采样率=%luHz，ASP_SPRATE=0x%02X，当前仍保持 PDN_ASP=1、PDN_HP=1",
        static_cast<unsigned long>(sample_rate_hz),
        static_cast<unsigned>(sample_rate_reg));
    return ESP_OK;
}

esp_err_t cs43131_prepare_pcm_test_48k_32bit()
{
    return cs43131_prepare_pcm_playback_32bit(48000);
}

esp_err_t cs43131_enable_asp_input()
{
    if (!g_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t power = 0;
    esp_err_t ret = cs43131_read_reg(REG_POWER_DOWN, &power);
    if (ret != ESP_OK) {
        return ret;
    }

    // 只解除 ASP 输入路径掉电；PDN_HP 必须继续保持为 1。
    power &= static_cast<uint8_t>(~0x40U);
    power |= 0x10;
    ret = cs43131_write_reg(REG_POWER_DOWN, power);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "ASP 输入已开启：POWER_DOWN=0x%02X，耳放仍保持关闭", power);
    return ESP_OK;
}

esp_err_t cs43131_read_asp_status(uint8_t *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return cs43131_read_reg(REG_INTERRUPT_STATUS_2, status);
}


esp_err_t cs43131_prepare_headphone_playback_low_volume()
{
    if (!g_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "正在配置低音量 PCM 耳放输出：0.5Vrms满量程，PCM数字音量=-20dB");

    struct RegValue {
        uint32_t reg;
        uint8_t value;
    };

    // 在耳放仍处于掉电状态时先设置模拟满量程、PCM滤波和低数字音量。
    // 0x80000 的 OUT_FS=00 对应 0.5Vrms，避免第一次模拟输出使用默认 1.73Vrms。
    static constexpr RegValue config[] = {
        {REG_PCM_FILTER_OPTION, 0x02},
        {REG_PCM_VOLUME_B, PCM_TEST_VOLUME_MINUS_20_DB},
        {REG_PCM_VOLUME_A, PCM_TEST_VOLUME_MINUS_20_DB},
        {REG_PCM_PATH_CONTROL_1, PCM_PATH_SOFT_RAMP_MUTED},
        {REG_PCM_PATH_CONTROL_2, 0x00},
        {REG_CLASS_H_CONTROL, 0x1E},
        {REG_HP_OUTPUT_CONTROL, 0x00},
        {REG_POP_FREE_POWER_UP_1, 0x99},
        {REG_POP_FREE_POWER_UP_2, 0x20},
    };

    for (const auto &item : config) {
        esp_err_t ret = cs43131_write_reg(item.reg, item.value);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "配置耳放播放寄存器 0x%06lX 失败：%s",
                static_cast<unsigned long>(item.reg), esp_err_to_name(ret));
            return ret;
        }
    }

    uint8_t power = 0;
    esp_err_t ret = cs43131_read_reg(REG_POWER_DOWN, &power);
    if (ret != ESP_OK) {
        return ret;
    }

    // ASP 已经在数字链路阶段开启；这里按照数据手册 PCM pop-free 上电序列
    // 再确认 ASP 保持开启，然后解除 PDN_HP。
    power &= static_cast<uint8_t>(~0x40U);
    ret = cs43131_write_reg(REG_POWER_DOWN, power);
    if (ret != ESP_OK) {
        return ret;
    }

    power &= static_cast<uint8_t>(~0x10U);
    ret = cs43131_write_reg(REG_POWER_DOWN, power);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(12));

    ret = cs43131_write_reg(REG_POP_FREE_POWER_UP_2, 0x00);
    if (ret == ESP_OK) {
        ret = cs43131_write_reg(REG_POP_FREE_POWER_UP_1, 0x00);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "耳放 pop-free 上电完成：POWER_DOWN=0x%02X，PCM仍保持手动静音", power);
    return ESP_OK;
}

esp_err_t cs43131_set_pcm_volume_attenuation(uint8_t half_db_steps)
{
    if (!g_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = cs43131_write_reg(REG_PCM_VOLUME_B, half_db_steps);
    if (ret == ESP_OK) {
        ret = cs43131_write_reg(REG_PCM_VOLUME_A, half_db_steps);
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PCM数字音量：衰减=%u.%udB，寄存器=0x%02X",
            static_cast<unsigned>(half_db_steps / 2U),
            static_cast<unsigned>((half_db_steps & 1U) ? 5U : 0U),
            half_db_steps);
    }
    return ret;
}

esp_err_t cs43131_set_pcm_mute(bool mute)
{
    if (!g_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t value = mute ? PCM_PATH_SOFT_RAMP_MUTED : PCM_PATH_SOFT_RAMP_UNMUTED;
    esp_err_t ret = cs43131_write_reg(REG_PCM_PATH_CONTROL_1, value);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PCM输出：%s，路径控制=0x%02X", mute ? "静音" : "解除静音", value);
    }
    return ret;
}

esp_err_t cs43131_power_down_headphone_playback()
{
    if (!g_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // 数据手册要求在关闭耳放前启用 PDN_DONE 中断。读取状态寄存器会清除旧 sticky 状态。
    uint8_t mask1 = 0;
    esp_err_t ret = cs43131_read_reg(REG_INTERRUPT_MASK_1, &mask1);
    if (ret != ESP_OK) {
        return ret;
    }

    mask1 &= 0xFE;
    ret = cs43131_write_reg(REG_INTERRUPT_MASK_1, mask1);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t status1 = 0;
    ret = cs43131_read_reg(REG_INTERRUPT_STATUS_1, &status1);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t power = 0;
    ret = cs43131_read_reg(REG_POWER_DOWN, &power);
    if (ret != ESP_OK) {
        return ret;
    }

    // 先关闭耳放，等待 PDN_DONE，再关闭 ASP。XTAL 此时继续运行，
    // 随后由 cs43131_finish_pcm_playback() 切回 RCO 并关闭晶振。
    power |= 0x10;
    ret = cs43131_write_reg(REG_POWER_DOWN, power);
    if (ret != ESP_OK) {
        return ret;
    }

    bool pdn_done = false;
    uint8_t last_status = 0;
    for (int attempt = 0; attempt < HP_PDN_DONE_WAIT_MS; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(1));
        ret = cs43131_read_reg(REG_INTERRUPT_STATUS_1, &status1);
        if (ret != ESP_OK) {
            return ret;
        }
        last_status = status1;
        if ((status1 & 0x01) != 0) {
            pdn_done = true;
            ESP_LOGI(TAG, "耳放掉电完成：等待=%dms，INT_STATUS1=0x%02X", attempt + 1, status1);
            break;
        }
    }

    if (!pdn_done) {
        uint8_t verify_power = 0;
        esp_err_t verify_ret = cs43131_read_reg(REG_POWER_DOWN, &verify_power);
        if (verify_ret == ESP_OK) {
            ESP_LOGE(TAG, "等待耳放 PDN_DONE 超时：等待=%dms，最后INT_STATUS1=0x%02X，POWER_DOWN=0x%02X",
                HP_PDN_DONE_WAIT_MS, last_status, verify_power);
        } else {
            ESP_LOGE(TAG, "等待耳放 PDN_DONE 超时：等待=%dms，最后INT_STATUS1=0x%02X",
                HP_PDN_DONE_WAIT_MS, last_status);
        }
        return ESP_ERR_TIMEOUT;
    }

    power |= 0x40;
    ret = cs43131_write_reg(REG_POWER_DOWN, power);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "耳放与ASP已安全关闭：POWER_DOWN=0x%02X", power);
    return ESP_OK;
}

esp_err_t cs43131_finish_pcm_playback()
{
    if (!g_ready || g_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t power = 0;
    esp_err_t ret = cs43131_read_reg(REG_POWER_DOWN, &power);
    if (ret != ESP_OK) {
        return ret;
    }

    // 先关闭 ASP，数字时钟仍保持有效，避免在接口工作中直接切时钟源。
    power |= 0x40;
    ret = cs43131_write_reg(REG_POWER_DOWN, power);
    if (ret != ESP_OK) {
        return ret;
    }

    // 切回复位默认 RCO。此模式仅用于控制口待机，不用于 DAC 播放。
    ret = cs43131_write_reg(REG_SYSTEM_CLOCKING, 0x06);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    // 最后关闭 XTAL，恢复控制口验证阶段的低功耗安全状态。
    power |= 0x08;
    ret = cs43131_write_reg(REG_POWER_DOWN, power);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "PCM播放链路已关闭：ASP和XTAL已关闭，当前POWER_DOWN=0x%02X", power);
    return ESP_OK;
}

// Stage 8.x 旧接口保留为兼容别名，后续可供系统诊断页面继续复用。
esp_err_t cs43131_prepare_headphone_test_low_volume()
{
    return cs43131_prepare_headphone_playback_low_volume();
}

esp_err_t cs43131_set_pcm_test_mute(bool mute)
{
    return cs43131_set_pcm_mute(mute);
}

esp_err_t cs43131_power_down_headphone_test()
{
    return cs43131_power_down_headphone_playback();
}

esp_err_t cs43131_finish_pcm_test()
{
    return cs43131_finish_pcm_playback();
}

bool cs43131_is_ready()
{
    return g_ready;
}

uint8_t cs43131_get_revision()
{
    return g_revision;
}

uint8_t cs43131_get_subrevision()
{
    return g_subrevision;
}
