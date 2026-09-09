#include "power_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "board_pins.h"
#include "persistent_state.h"
#include "player/player_control.h"
#include "device_settings.h"
#include "screen_lock_simple.h"
#include "app/app_manager.h"

static const char *TAG = "电源";

// 网表：K2.2 / U21.3 -> D1.1，D1.2 -> BTN -> U3.36(GPIO48)。
// 实机测得 GPIO48 稳定按下后约 1.97 秒即触发 Brownout；NVS 保存阈值设为 1.0 秒，
// 按当前实测仍为 Flash commit 保留约 0.9 秒余量。BTN 侧无独立外部上拉，按键按下按低有效处理。
static constexpr int POWER_KEY_ACTIVE_LEVEL = 0;
static constexpr TickType_t POWER_KEY_DEBOUNCE_TICKS = pdMS_TO_TICKS(40);
static constexpr TickType_t POWER_KEY_SAVE_HOLD_TICKS = pdMS_TO_TICKS(1000);

// 释放分级：0 ~ 500ms 的稳定按压 → 按设置执行“音量+ / 上一曲”
//           ≥ 1000ms 按压 → NVS 保存（接着硬件 EC190707 ~2s 自动断电）
// 500~1000ms 之间的释放不动作（留给关机长按的判定间隔，避免临界误操作）
static constexpr uint32_t SHORT_ACTION_MAX_HOLD_MS = 500U;

static bool g_ready = false;
static bool g_armed = false;
static bool g_raw_pressed = false;
static bool g_stable_pressed = false;
static bool g_save_attempted_this_press = false;
static TickType_t g_raw_changed_tick = 0;
static TickType_t g_press_started_tick = 0;

static bool power_service_read_pressed()
{
    return gpio_get_level(static_cast<gpio_num_t>(FAKEPOD_POWER_KEY)) == POWER_KEY_ACTIVE_LEVEL;
}

esp_err_t power_service_init()
{
    if (g_ready) {
        return ESP_OK;
    }

    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << FAKEPOD_POWER_KEY;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    const esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO48 电源键初始化失败：%s", esp_err_to_name(ret));
        return ret;
    }

    const TickType_t now = xTaskGetTickCount();
    g_raw_pressed = power_service_read_pressed();
    g_stable_pressed = g_raw_pressed;
    g_raw_changed_tick = now;
    g_press_started_tick = 0;
    g_save_attempted_this_press = false;

    // 开机动作本身可能要求用户持续按住同一个 K2。READY 时若 GPIO48 仍为按下态，
    // 必须等用户先松开一次再布防，否则会把“开机长按尾巴”误当成关机请求。
    g_armed = !g_stable_pressed;
    g_ready = true;

    ESP_LOGI(
        TAG,
        "GPIO48 电源键就绪：level=%d pressed=%s armed=%s，长按%lums触发NVS保存",
        gpio_get_level(static_cast<gpio_num_t>(FAKEPOD_POWER_KEY)),
        g_stable_pressed ? "YES" : "NO",
        g_armed ? "YES" : "WAIT_RELEASE",
        static_cast<unsigned long>(POWER_KEY_SAVE_HOLD_TICKS * portTICK_PERIOD_MS)
    );
    return ESP_OK;
}

void power_service_update()
{
    if (!g_ready) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    const bool raw_pressed = power_service_read_pressed();
    if (raw_pressed != g_raw_pressed) {
        g_raw_pressed = raw_pressed;
        g_raw_changed_tick = now;
        return;
    }

    if (raw_pressed != g_stable_pressed) {
        if (now - g_raw_changed_tick < POWER_KEY_DEBOUNCE_TICKS) {
            return;
        }

        g_stable_pressed = raw_pressed;
        if (!g_stable_pressed) {
            const uint32_t held_ms = g_press_started_tick == 0
                ? 0U
                : static_cast<uint32_t>((now - g_press_started_tick) * portTICK_PERIOD_MS);

            if (!g_armed) {
                g_armed = true;
                ESP_LOGI(TAG, "GPIO48 已检测到首次松键，关机长按检测正式布防");
            } else if (g_press_started_tick != 0 && !g_save_attempted_this_press) {
                if (held_ms <= SHORT_ACTION_MAX_HOLD_MS) {
                    // 短按功能由“辅助键模式”统一决定；长按关机保存逻辑完全不变。
                    DeviceSettingsSnapshot settings = {};
                    const bool track_mode = device_settings_get_snapshot(&settings) &&
                        settings.aux_key_mode == DeviceAuxKeyMode::Track;
                    if (track_mode) {
                        // 切歌仅在音乐或电子书前台时生效，避免在其它 APP 误切歌。
                        const AppId foreground = app_manager_foreground();
                        if (foreground == AppId::Music || foreground == AppId::Ebook) {
                            ESP_LOGI(TAG, "电源键短按释放：hold=%lums → 上一曲",
                                (unsigned long)held_ms);
                            (void)player_control_previous();
                        } else {
                            ESP_LOGI(TAG, "电源键短按释放：hold=%lums → 非音乐/电子书前台，切歌不生效",
                                (unsigned long)held_ms);
                        }
                    } else {
                        ESP_LOGI(TAG, "电源键短按释放：hold=%lums → 音量+1",
                            (unsigned long)held_ms);
                        (void)player_control_volume_up(1U);
                    }
                } else {
                    ESP_LOGI(TAG, "电源键中按释放：hold=%lums，未触发NVS保存，不做动作",
                        (unsigned long)held_ms);
                }
            }

            g_press_started_tick = 0;
            g_save_attempted_this_press = false;
            return;
        }

        if (!g_armed) {
            // 尚未见过开机后的首次松键；只跟踪电平，不启动长按计时。
            return;
        }

        g_press_started_tick = now;
        g_save_attempted_this_press = false;
        screen_lock_simple_notify_user_activity();
        ESP_LOGI(TAG, "电源键按下：GPIO48，开始关机保存计时");
    }

    if (!g_armed || !g_stable_pressed || g_save_attempted_this_press || g_press_started_tick == 0) {
        return;
    }
    if (now - g_press_started_tick < POWER_KEY_SAVE_HOLD_TICKS) {
        return;
    }

    // 一次物理按压最多尝试一次。NVS 写入只发生在 loopTask 正常上下文，绝不在 GPIO ISR 中执行。
    g_save_attempted_this_press = true;

    PersistentStateStatus status = {};
    (void)persistent_state_get_status(&status);
    ESP_LOGI(TAG, "关机长按确认：hold>=%lums dirty=0x%08lX，开始显式NVS保存",
        static_cast<unsigned long>(POWER_KEY_SAVE_HOLD_TICKS * portTICK_PERIOD_MS),
        static_cast<unsigned long>(status.dirty_bits));

    const TickType_t flush_begin = xTaskGetTickCount();
    const esp_err_t ret = persistent_state_flush();
    const uint32_t flush_ms = static_cast<uint32_t>(
        (xTaskGetTickCount() - flush_begin) * portTICK_PERIOD_MS
    );

    if (ret == ESP_OK) {
        PersistentStateStatus after = {};
        (void)persistent_state_get_status(&after);
        ESP_LOGI(TAG, "关机NVS保存完成：耗时=%lums remaining_dirty=0x%08lX；等待EC190707硬件断电",
            static_cast<unsigned long>(flush_ms),
            static_cast<unsigned long>(after.dirty_bits));
    } else {
        ESP_LOGE(TAG, "关机NVS保存失败：耗时=%lums %s；仍等待EC190707硬件断电",
            static_cast<unsigned long>(flush_ms),
            esp_err_to_name(ret));
    }
}
