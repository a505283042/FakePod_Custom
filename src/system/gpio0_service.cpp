#include "gpio0_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "board_pins.h"
#include "screen_lock_simple.h"
#include "player/player_control.h"
#include "app/app_manager.h"
#include "device_settings.h"

static const char *TAG = "GPIO0按键";

// 按板级说明：K1 按下 = GPIO0 = GND（低电平有效）
static constexpr int AUX_KEY_ACTIVE_LEVEL = 0;
static constexpr TickType_t KEY_DEBOUNCE_TICKS = pdMS_TO_TICKS(40);

// ============ 阈值（用户新交互） ============
static constexpr uint32_t SHORT_MAX_MS     = 400U;   // <400ms 释放 = 按设置执行音量- / 下一曲
static constexpr uint32_t LONG_PRESS_MS    = 400U;   // ≥400ms 仍按住 = 触发「长按」
                                                     //   暗态/锁定时：直接一键解锁
                                                     //   正常态 + 菜单未开：打开菜单（松手后保持）
                                                     //   正常态 + 菜单已开：关闭菜单（取消不执行）

static bool g_ready          = false;
static bool g_armed          = false;    // 首次松键后才布防
static bool g_raw_pressed    = false;
static bool g_stable_pressed = false;
static bool g_long_fired     = false;   // 本次按下中是否已达到 LONG_PRESS_MS
static TickType_t g_raw_changed_tick   = 0;
static TickType_t g_press_started_tick = 0;

static bool gpio0_read_pressed()
{
    return gpio_get_level(static_cast<gpio_num_t>(FAKEPOD_AUX_KEY)) == AUX_KEY_ACTIVE_LEVEL;
}

esp_err_t gpio0_service_init()
{
    if (g_ready) return ESP_OK;

    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << FAKEPOD_AUX_KEY;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO0 初始化失败：%s", esp_err_to_name(ret));
        return ret;
    }

    const TickType_t now = xTaskGetTickCount();
    g_raw_pressed = gpio0_read_pressed();
    g_stable_pressed = g_raw_pressed;
    g_raw_changed_tick = now;
    g_press_started_tick = 0;
    g_long_fired = false;

    // 开机若 GPIO0 被按着，等先松开一次再布防
    g_armed = !g_stable_pressed;
    g_ready = true;

    ESP_LOGI(
        TAG,
        "GPIO0 辅助键就绪：level=%d pressed=%s armed=%s"
        "；<400ms=按设置执行音量-/下一曲；≥400ms长按=暗态解锁/正打开菜单；菜单已开时长按=退出菜单",
        gpio_get_level(static_cast<gpio_num_t>(FAKEPOD_AUX_KEY)),
        g_stable_pressed ? "YES" : "NO",
        g_armed ? "YES" : "WAIT_RELEASE"
    );
    return ESP_OK;
}

static uint32_t held_ms(const TickType_t now, const TickType_t start_tick)
{
    return start_tick == 0
        ? 0U
        : static_cast<uint32_t>((now - start_tick) * portTICK_PERIOD_MS);
}

void gpio0_service_update()
{
    if (!g_ready) return;

    const TickType_t now = xTaskGetTickCount();
    const bool raw_pressed = gpio0_read_pressed();

    // ========== 1. 原始电平变化（去抖） ==========
    if (raw_pressed != g_raw_pressed) {
        g_raw_pressed = raw_pressed;
        g_raw_changed_tick = now;
        return;
    }

    // ========== 2. 稳定边沿 ==========
    if (raw_pressed != g_stable_pressed) {
        if (now - g_raw_changed_tick < KEY_DEBOUNCE_TICKS) return;

        g_stable_pressed = raw_pressed;

        if (!g_stable_pressed) {
            // 🔻 释放 RELEASE
            const uint32_t held = held_ms(now, g_press_started_tick);
            if (!g_armed) {
                g_armed = true;
                ESP_LOGI(TAG, "GPIO0 首次松键，布防 hold=%lums", (unsigned long)held);
            } else if (g_press_started_tick != 0 && held <= SHORT_MAX_MS) {
                // 短按功能由设置决定；长按仍保留现有解锁/屏幕动作菜单。
                DeviceSettingsSnapshot settings = {};
                const bool track_mode = device_settings_get_snapshot(&settings) &&
                    settings.aux_key_mode == DeviceAuxKeyMode::Track;
                if (track_mode) {
                    // 切歌仅在音乐或电子书前台时生效，避免在其它 APP 误切歌。
                    const AppId foreground = app_manager_foreground();
                    if (foreground == AppId::Music || foreground == AppId::Ebook) {
                        ESP_LOGI(TAG, "GPIO0 释放：短按 %lums → 下一曲", (unsigned long)held);
                        (void)player_control_next();
                    } else {
                        ESP_LOGI(TAG, "GPIO0 释放：短按 %lums → 非音乐/电子书前台，切歌不生效",
                            (unsigned long)held);
                    }
                } else {
                    ESP_LOGI(TAG, "GPIO0 释放：短按 %lums → 音量-1", (unsigned long)held);
                    (void)player_control_volume_down(1U);
                }
            } else if (g_long_fired) {
                // 长按档位已经在按住期间触发过了，这里不重复
                // (不做任何动作)
            }
            g_press_started_tick = 0;
            g_long_fired = false;
            return;
        }

        // 🔺 稳定按下 PRESS
        if (!g_armed) return;
        g_press_started_tick = now;
        g_long_fired = false;
        screen_lock_simple_notify_user_activity();
        ESP_LOGI(TAG, "GPIO0 按下：短按按设置执行音量-/下一曲；≥400ms保持现有长按档位");
        return;
    }

    // ========== 3. 按下中：达到 LONG_PRESS_MS → 只触发一次长按档位 ==========
    if (!g_armed || !g_stable_pressed || g_press_started_tick == 0) return;
    if (g_long_fired) return;

    const uint32_t held = held_ms(now, g_press_started_tick);
    if (held < LONG_PRESS_MS) return;
    g_long_fired = true;

    // ---- 判断当前屏幕状态 ----
    const ScreenLockState lock = screen_lock_simple_get_lock();
    const ScreenPowerState power = screen_lock_simple_get_power();
    const bool is_dark_or_locked = (lock == ScreenLockLocked) || (power != ScreenPowerNormal);

    if (is_dark_or_locked) {
        // ① 暗态或锁定（Locked / AOD / 熄屏，任意组合）：
        //    直接「一键解锁」= 亮回 Normal 并解除锁定
        ESP_LOGI(TAG,
            "GPIO0 长按 %lums [屏态=%s 锁=%s] → 一键 Normal + Unlocked",
            (unsigned long)held,
            power == ScreenPowerNormal ? "NORMAL" :
            power == ScreenPowerAOD ? "AOD" : "OFF",
            lock == ScreenLockLocked ? "LOCKED" : "FREE");
        // 先亮回来（Normal），再解锁
        screen_lock_simple_set_power(ScreenPowerNormal);
        screen_lock_simple_set_lock(ScreenLockUnlocked);
        // 同时关闭残留菜单（如果之前有打开过）
        screen_action_menu_close(false);
    } else {
        // ② 正常亮屏 + 未锁定：打开 / 关闭 菜单（循环）
        // 锁屏 / AOD / 熄屏 只应在音乐 APP 生效；其他 APP 前台时长按不响应
        if (screen_action_menu_is_open()) {
            ESP_LOGI(TAG, "GPIO0 长按 %lums → 菜单已开：关闭（取消不执行）",
                (unsigned long)held);
            screen_action_menu_close(false);
        } else if (app_manager_foreground() != AppId::Music) {
            ESP_LOGI(TAG, "GPIO0 长按 %lums → 当前为 %s，非音乐 APP，不打开锁屏菜单",
                (unsigned long)held,
                app_manager_name(app_manager_foreground()));
        } else {
            ESP_LOGI(TAG, "GPIO0 长按 %lums → 打开屏幕动作菜单（松手后保留，点击行才执行）",
                (unsigned long)held);
            screen_action_menu_open();
        }
    }
}
