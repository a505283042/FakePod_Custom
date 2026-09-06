#include "battery_service.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

#include "board_pins.h"

#ifndef FAKEPOD_BATTERY_DIAGNOSTIC_LOG
#define FAKEPOD_BATTERY_DIAGNOSTIC_LOG 0
#endif

namespace {

static const char *TAG = "电池";

static constexpr adc_atten_t BATTERY_ADC_ATTEN = ADC_ATTEN_DB_12;
static constexpr adc_bitwidth_t BATTERY_ADC_BITWIDTH = ADC_BITWIDTH_DEFAULT;

// 硬件已固定：R80=10k, R81=10k，BAT_ADC = VBAT / 2。
static constexpr uint32_t BATTERY_DIVIDER_NUM = 2U;
static constexpr uint32_t BATTERY_DIVIDER_DEN = 1U;

// V1 先保持 1.0000；与万用表比对后只改这里即可补偿实际分压/板级误差。
static constexpr uint32_t BATTERY_BOARD_CAL_NUM = 10000U;
static constexpr uint32_t BATTERY_BOARD_CAL_DEN = 10000U;

static constexpr size_t BATTERY_SAMPLE_COUNT = 32U;
static constexpr size_t BATTERY_TRIM_COUNT = 4U;
static constexpr size_t BATTERY_RETAINED_COUNT = BATTERY_SAMPLE_COUNT - BATTERY_TRIM_COUNT * 2U;
static_assert(BATTERY_RETAINED_COUNT > 0U, "battery trim must retain samples");

static constexpr TickType_t BATTERY_SAMPLE_INTERVAL = pdMS_TO_TICKS(2000);

// 每 2 秒一个样本，alpha=1/8，等效慢滤波时间尺度约十几秒；首样本直接建立基线。
static constexpr uint32_t BATTERY_EMA_OLD_WEIGHT = 7U;
static constexpr uint32_t BATTERY_EMA_DIVISOR = 8U;

// RELEASE 日志只记录低电量状态边界；退出阈值留 2% 回差，避免临界点反复刷日志。
static constexpr uint8_t BATTERY_LOW_ENTER_PERCENT = 15U;
static constexpr uint8_t BATTERY_LOW_EXIT_PERCENT = 17U;
static constexpr uint8_t BATTERY_CRITICAL_ENTER_PERCENT = 5U;
static constexpr uint8_t BATTERY_CRITICAL_EXIT_PERCENT = 7U;

enum class BatteryLogState : uint8_t
{
    Unknown = 0,
    Normal,
    Low,
    Critical,
};

struct BatterySocPoint
{
    uint16_t mv;
    uint8_t percent;
};

// 1S Li-ion/LiPo 的 V1 经验 LUT。实机稳定满电端约 4.188~4.192V，
// 因此把 UI 的 100% 显示阈值收在 4.190V；不通过全局增益硬拉整条电压曲线。
static constexpr BatterySocPoint BATTERY_SOC_LUT[] = {
    {4190U, 100U},
    {4100U, 90U},
    {4000U, 80U},
    {3920U, 70U},
    {3850U, 60U},
    {3790U, 50U},
    {3730U, 40U},
    {3680U, 30U},
    {3620U, 20U},
    {3500U, 10U},
    {3350U, 5U},
    {3250U, 0U},
};

static adc_oneshot_unit_handle_t g_adc = nullptr;
static adc_cali_handle_t g_cali = nullptr;
static adc_channel_t g_channel = ADC_CHANNEL_0;
static bool g_ready = false;
static TickType_t g_next_sample_tick = 0;
static uint32_t g_filtered_mv = 0U;
static const char *g_calibration_scheme = "NONE";
static BatteryLogState g_log_state = BatteryLogState::Unknown;
static BatterySnapshot g_snapshot = {};
static portMUX_TYPE g_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t battery_soc_from_mv(uint32_t mv)
{
    if (mv >= BATTERY_SOC_LUT[0].mv) {
        return BATTERY_SOC_LUT[0].percent;
    }

    const size_t point_count = sizeof(BATTERY_SOC_LUT) / sizeof(BATTERY_SOC_LUT[0]);
    if (mv <= BATTERY_SOC_LUT[point_count - 1U].mv) {
        return BATTERY_SOC_LUT[point_count - 1U].percent;
    }

    for (size_t i = 0U; i + 1U < point_count; ++i) {
        const BatterySocPoint high = BATTERY_SOC_LUT[i];
        const BatterySocPoint low = BATTERY_SOC_LUT[i + 1U];
        if (mv <= high.mv && mv >= low.mv) {
            const uint32_t voltage_span = static_cast<uint32_t>(high.mv - low.mv);
            const uint32_t percent_span = static_cast<uint32_t>(high.percent - low.percent);
            const uint32_t voltage_from_low = mv - low.mv;
            const uint32_t interpolated = static_cast<uint32_t>(low.percent) +
                (voltage_from_low * percent_span + voltage_span / 2U) / voltage_span;
            return static_cast<uint8_t>(interpolated > 100U ? 100U : interpolated);
        }
    }

    return 0U;
}

static void battery_update_log_state(const BatterySnapshot &snapshot)
{
    BatteryLogState next = BatteryLogState::Normal;
    if (g_log_state == BatteryLogState::Critical && snapshot.percent < BATTERY_LOW_EXIT_PERCENT) {
        next = snapshot.percent >= BATTERY_CRITICAL_EXIT_PERCENT
            ? BatteryLogState::Low
            : BatteryLogState::Critical;
    } else if (g_log_state == BatteryLogState::Low && snapshot.percent < BATTERY_LOW_EXIT_PERCENT) {
        next = snapshot.percent <= BATTERY_CRITICAL_ENTER_PERCENT
            ? BatteryLogState::Critical
            : BatteryLogState::Low;
    } else if (snapshot.percent <= BATTERY_CRITICAL_ENTER_PERCENT) {
        next = BatteryLogState::Critical;
    } else if (snapshot.percent <= BATTERY_LOW_ENTER_PERCENT) {
        next = BatteryLogState::Low;
    }

    if (next == g_log_state) {
        return;
    }

    const BatteryLogState previous = g_log_state;
    g_log_state = next;

    if (next == BatteryLogState::Critical) {
        ESP_LOGW(TAG, "进入严重低电量：%u%%，%umV",
            static_cast<unsigned>(snapshot.percent),
            static_cast<unsigned>(snapshot.filtered_mv));
    } else if (next == BatteryLogState::Low) {
        if (previous == BatteryLogState::Critical) {
            ESP_LOGI(TAG, "电量已离开严重低电量区间：%u%%，%umV",
                static_cast<unsigned>(snapshot.percent),
                static_cast<unsigned>(snapshot.filtered_mv));
        } else {
            ESP_LOGW(TAG, "进入低电量：%u%%，%umV",
                static_cast<unsigned>(snapshot.percent),
                static_cast<unsigned>(snapshot.filtered_mv));
        }
    } else if (previous != BatteryLogState::Unknown) {
        ESP_LOGI(TAG, "低电量状态解除：%u%%，%umV",
            static_cast<unsigned>(snapshot.percent),
            static_cast<unsigned>(snapshot.filtered_mv));
    }
}

static void battery_sort_samples(int *samples)
{
    // 32 个元素的固定小数组，用 insertion sort 避免额外堆分配和 STL 依赖。
    for (size_t i = 1U; i < BATTERY_SAMPLE_COUNT; ++i) {
        const int value = samples[i];
        size_t pos = i;
        while (pos > 0U && samples[pos - 1U] > value) {
            samples[pos] = samples[pos - 1U];
            --pos;
        }
        samples[pos] = value;
    }
}

static esp_err_t battery_take_sample(BatterySnapshot *out_snapshot)
{
    if (out_snapshot == nullptr || g_adc == nullptr || g_cali == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    int raw_samples[BATTERY_SAMPLE_COUNT] = {};
    for (size_t i = 0U; i < BATTERY_SAMPLE_COUNT; ++i) {
        const esp_err_t read_ret = adc_oneshot_read(g_adc, g_channel, &raw_samples[i]);
        if (read_ret != ESP_OK) {
            return read_ret;
        }
    }

    battery_sort_samples(raw_samples);

    uint32_t raw_sum = 0U;
    uint32_t adc_mv_sum = 0U;
    for (size_t i = BATTERY_TRIM_COUNT; i < BATTERY_SAMPLE_COUNT - BATTERY_TRIM_COUNT; ++i) {
        int calibrated_mv = 0;
        const esp_err_t cal_ret = adc_cali_raw_to_voltage(g_cali, raw_samples[i], &calibrated_mv);
        if (cal_ret != ESP_OK || calibrated_mv < 0) {
            return cal_ret == ESP_OK ? ESP_FAIL : cal_ret;
        }
        raw_sum += static_cast<uint32_t>(raw_samples[i]);
        adc_mv_sum += static_cast<uint32_t>(calibrated_mv);
    }

    const uint32_t raw_average = (raw_sum + BATTERY_RETAINED_COUNT / 2U) / BATTERY_RETAINED_COUNT;
    const uint32_t adc_mv = (adc_mv_sum + BATTERY_RETAINED_COUNT / 2U) / BATTERY_RETAINED_COUNT;

    uint32_t battery_mv = (adc_mv * BATTERY_DIVIDER_NUM + BATTERY_DIVIDER_DEN / 2U) /
        BATTERY_DIVIDER_DEN;
    battery_mv = (battery_mv * BATTERY_BOARD_CAL_NUM + BATTERY_BOARD_CAL_DEN / 2U) /
        BATTERY_BOARD_CAL_DEN;

    if (g_filtered_mv == 0U) {
        g_filtered_mv = battery_mv;
    } else {
        g_filtered_mv = (g_filtered_mv * BATTERY_EMA_OLD_WEIGHT + battery_mv +
            BATTERY_EMA_DIVISOR / 2U) / BATTERY_EMA_DIVISOR;
    }

    BatterySnapshot snapshot = {};
    snapshot.valid = true;
    snapshot.calibrated = true;
    snapshot.raw_average = static_cast<uint16_t>(raw_average > UINT16_MAX ? UINT16_MAX : raw_average);
    snapshot.raw_min = static_cast<uint16_t>(raw_samples[0] < 0 ? 0 : raw_samples[0]);
    snapshot.raw_max = static_cast<uint16_t>(raw_samples[BATTERY_SAMPLE_COUNT - 1U] < 0
        ? 0
        : raw_samples[BATTERY_SAMPLE_COUNT - 1U]);
    snapshot.adc_mv = static_cast<uint16_t>(adc_mv > UINT16_MAX ? UINT16_MAX : adc_mv);
    snapshot.battery_mv = static_cast<uint16_t>(battery_mv > UINT16_MAX ? UINT16_MAX : battery_mv);
    snapshot.filtered_mv = static_cast<uint16_t>(g_filtered_mv > UINT16_MAX ? UINT16_MAX : g_filtered_mv);
    snapshot.percent = battery_soc_from_mv(g_filtered_mv);

    portENTER_CRITICAL(&g_snapshot_mux);
    snapshot.sequence = g_snapshot.sequence + 1U;
    g_snapshot = snapshot;
    portEXIT_CRITICAL(&g_snapshot_mux);

    *out_snapshot = snapshot;
    return ESP_OK;
}

static esp_err_t battery_init_calibration(adc_unit_t unit, adc_channel_t channel)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_config = {};
    curve_config.unit_id = unit;
    curve_config.chan = channel;
    curve_config.atten = BATTERY_ADC_ATTEN;
    curve_config.bitwidth = BATTERY_ADC_BITWIDTH;

    const esp_err_t curve_ret = adc_cali_create_scheme_curve_fitting(&curve_config, &g_cali);
    if (curve_ret == ESP_OK) {
        g_calibration_scheme = "CurveFitting";
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Curve Fitting校准不可用：%s", esp_err_to_name(curve_ret));
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t line_config = {};
    line_config.unit_id = unit;
    line_config.atten = BATTERY_ADC_ATTEN;
    line_config.bitwidth = BATTERY_ADC_BITWIDTH;

    const esp_err_t line_ret = adc_cali_create_scheme_line_fitting(&line_config, &g_cali);
    if (line_ret == ESP_OK) {
        g_calibration_scheme = "LineFitting";
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Line Fitting校准不可用：%s", esp_err_to_name(line_ret));
    return line_ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

} // namespace

esp_err_t battery_service_init()
{
    if (g_ready) {
        return ESP_OK;
    }

    adc_unit_t detected_unit = ADC_UNIT_1;
    adc_channel_t detected_channel = ADC_CHANNEL_0;
    const esp_err_t map_ret = adc_oneshot_io_to_channel(
        FAKEPOD_BATTERY_ADC,
        &detected_unit,
        &detected_channel);
    if (map_ret != ESP_OK || detected_unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "BAT_ADC GPIO%d映射失败：unit=%d channel=%d ret=%s",
            FAKEPOD_BATTERY_ADC,
            static_cast<int>(detected_unit),
            static_cast<int>(detected_channel),
            esp_err_to_name(map_ret));
        return map_ret == ESP_OK ? ESP_ERR_INVALID_ARG : map_ret;
    }
    g_channel = detected_channel;

    adc_oneshot_unit_init_cfg_t unit_config = {};
    unit_config.unit_id = detected_unit;
    const esp_err_t unit_ret = adc_oneshot_new_unit(&unit_config, &g_adc);
    if (unit_ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 OneShot初始化失败：%s", esp_err_to_name(unit_ret));
        return unit_ret;
    }

    adc_oneshot_chan_cfg_t channel_config = {};
    channel_config.atten = BATTERY_ADC_ATTEN;
    channel_config.bitwidth = BATTERY_ADC_BITWIDTH;
    const esp_err_t channel_ret = adc_oneshot_config_channel(g_adc, g_channel, &channel_config);
    if (channel_ret != ESP_OK) {
        ESP_LOGE(TAG, "BAT_ADC通道配置失败：%s", esp_err_to_name(channel_ret));
        (void)adc_oneshot_del_unit(g_adc);
        g_adc = nullptr;
        return channel_ret;
    }

    const esp_err_t cal_ret = battery_init_calibration(detected_unit, g_channel);
    if (cal_ret != ESP_OK || g_cali == nullptr) {
        ESP_LOGE(TAG, "BAT_ADC校准初始化失败：%s；拒绝用未校准RAW估算电压",
            esp_err_to_name(cal_ret));
        (void)adc_oneshot_del_unit(g_adc);
        g_adc = nullptr;
        return cal_ret == ESP_OK ? ESP_FAIL : cal_ret;
    }

    g_filtered_mv = 0U;
    g_log_state = BatteryLogState::Unknown;
    portENTER_CRITICAL(&g_snapshot_mux);
    g_snapshot = {};
    portEXIT_CRITICAL(&g_snapshot_mux);
    g_next_sample_tick = xTaskGetTickCount();
    g_ready = true;

    ESP_LOGI(TAG,
        "BatteryService就绪：GPIO%d ADC1_CH%d calibration=%s atten=12dB divider=1:2",
        FAKEPOD_BATTERY_ADC,
        static_cast<int>(g_channel),
        g_calibration_scheme);
    return ESP_OK;
}

void battery_service_update()
{
    if (!g_ready) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (static_cast<int32_t>(now - g_next_sample_tick) < 0) {
        return;
    }
    g_next_sample_tick = now + BATTERY_SAMPLE_INTERVAL;

    BatterySnapshot snapshot = {};
    const esp_err_t ret = battery_take_sample(&snapshot);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BAT_ADC采样失败：%s；2秒后重试", esp_err_to_name(ret));
        return;
    }

    battery_update_log_state(snapshot);

#if FAKEPOD_BATTERY_DIAGNOSTIC_LOG
    // 诊断模式才恢复每 2 秒 RAW / ADC / VBAT / 滤波 / SOC 详细遥测。
    ESP_LOGI(TAG,
        "ADC：raw=%u [%u..%u] adc=%umV vbat=%umV filtered=%umV soc=%u%% seq=%lu",
        static_cast<unsigned>(snapshot.raw_average),
        static_cast<unsigned>(snapshot.raw_min),
        static_cast<unsigned>(snapshot.raw_max),
        static_cast<unsigned>(snapshot.adc_mv),
        static_cast<unsigned>(snapshot.battery_mv),
        static_cast<unsigned>(snapshot.filtered_mv),
        static_cast<unsigned>(snapshot.percent),
        static_cast<unsigned long>(snapshot.sequence));
#endif
}

bool battery_service_get_snapshot(BatterySnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) {
        return false;
    }

    portENTER_CRITICAL(&g_snapshot_mux);
    *out_snapshot = g_snapshot;
    portEXIT_CRITICAL(&g_snapshot_mux);
    return out_snapshot->valid;
}
