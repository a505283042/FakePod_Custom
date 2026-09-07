#include "motion_service.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"

#include "app_manager.h"
#include "board_pins.h"
#include "device_settings.h"
#include "player_control.h"
#include "qmi8658.h"
#include "screen_lock_simple.h"


static const char *TAG = "运动控制";

#ifndef FAKEPOD_MOTION_DIAGNOSTIC_LOG
#define FAKEPOD_MOTION_DIAGNOSTIC_LOG 0
#endif

// Motion V2 首轮保留少量翻转调参日志，用于确认装壳后的前/后方向映射和动作角度。
#ifndef FAKEPOD_MOTION_FLIP_TUNING_LOG
#define FAKEPOD_MOTION_FLIP_TUNING_LOG 1
#endif


static constexpr TickType_t IMU_SAMPLE_INTERVAL = pdMS_TO_TICKS(10);  // 100Hz host-side read
static constexpr TickType_t GYRO_WARMUP = pdMS_TO_TICKS(250);
static constexpr TickType_t FLIP_SEQUENCE_TIMEOUT = pdMS_TO_TICKS(1500);
static constexpr TickType_t FLIP_OUTBOUND_TIMEOUT = pdMS_TO_TICKS(550);
static constexpr TickType_t FLIP_RETURN_TIMEOUT = pdMS_TO_TICKS(500);
static constexpr TickType_t FLIP_MIN_ACTION_SPAN = pdMS_TO_TICKS(180);
static constexpr TickType_t FLIP_RECENTER_STABLE = pdMS_TO_TICKS(150);
static constexpr TickType_t FLIP_COOLDOWN = pdMS_TO_TICKS(1300);
static constexpr TickType_t DOUBLE_TAP_COOLDOWN = pdMS_TO_TICKS(800);
static constexpr TickType_t CROSS_GESTURE_SUPPRESS = pdMS_TO_TICKS(700);
static constexpr TickType_t IMU_READ_ERROR_LOG_INTERVAL = pdMS_TO_TICKS(2000);
static constexpr TickType_t POCKET_MOTION_CONTINUE_GAP = pdMS_TO_TICKS(600);
static constexpr TickType_t POCKET_MOTION_CONFIRM = pdMS_TO_TICKS(1200);
static constexpr TickType_t POCKET_MOTION_HOLD = pdMS_TO_TICKS(600);

static constexpr int32_t FLIP_START_RATE_MDPS = 80000;       // 80 dps
static constexpr int32_t FLIP_RETURN_RATE_MDPS = 50000;      // 50 dps
static constexpr int32_t FLIP_MIN_EXCURSION_MDEG = 35000;    // 35 deg
static constexpr int32_t FLIP_FAILED_RETURN_MIN_MDEG = 8000; // early return below threshold -> failed gesture
static constexpr int32_t FLIP_RETURN_BAND_MDEG = 15000;      // ±15 deg, relaxed return-to-origin band
static constexpr int32_t FLIP_GRAVITY_XY_MIN_MG = 300;       // dynamic zero: allow relaxed natural holding posture
static constexpr int32_t FLIP_RECENTER_GYRO_MDPS = 30000;    // 30 dps, stable enough to establish next zero
static constexpr int32_t FLIP_RECENTER_ACCEL_MIN_MG = 750;
static constexpr int32_t FLIP_RECENTER_ACCEL_MAX_MG = 1250;
static constexpr int32_t FLIP_CROSS_AXIS_ABORT_MDPS = 450000;
static constexpr int32_t POCKET_MOTION_MG = 250;

// Adaptive pitch projection is mounting-rotation independent while the sensor Z axis
// remains normal to the PCB. If assembled-unit testing reports front/back reversed,
// change only this sign; do not touch the gesture detector.
static constexpr int8_t FLIP_FORWARD_PITCH_SIGN = 1;


enum class FlipPhase : uint8_t
{
    Idle = 0,
    Outbound,
    ReturningSuccess,
    ReturningFailed,
    Recenter,
};


static bool g_ready = false;
static volatile bool g_int1_pending = false;
static TickType_t g_last_imu_sample_tick = 0;
static TickType_t g_last_imu_read_error_log_tick = 0;
static TickType_t g_gyro_warmup_until = 0;
static TickType_t g_flip_cooldown_until = 0;
static TickType_t g_tap_cooldown_until = 0;
static TickType_t g_tap_suppress_until = 0;
static TickType_t g_flip_suppress_until = 0;
static TickType_t g_motion_run_started_tick = 0;
static TickType_t g_motion_run_last_tick = 0;
static TickType_t g_pocket_motion_until = 0;
static bool g_controls_allowed_last = false;

static bool g_accel_baseline_ready = false;
static int32_t g_accel_baseline_x = 0;
static int32_t g_accel_baseline_y = 0;
static int32_t g_accel_baseline_z = 0;

static FlipPhase g_flip_phase = FlipPhase::Idle;
static int8_t g_flip_direction = 0;
static int64_t g_flip_angle_mdeg = 0;
static int32_t g_flip_peak_excursion_mdeg = 0;
static bool g_flip_return_seen = false;
static bool g_flip_pending_success = false;
static int8_t g_flip_pending_direction = 0;
static int32_t g_flip_pending_excursion_mdeg = 0;
static TickType_t g_flip_recenter_stable_since = 0;
static int32_t g_flip_axis_x_mg = 0;
static int32_t g_flip_axis_y_mg = 0;
static int32_t g_flip_axis_norm_mg = 0;
static TickType_t g_flip_started_tick = 0;
static TickType_t g_flip_reached_tick = 0;
static TickType_t g_flip_last_sample_tick = 0;


static int32_t motion_abs_i32(int32_t value)
{
    return value < 0 ? -value : value;
}


static int64_t motion_abs_i64(int64_t value)
{
    return value < 0 ? -value : value;
}


static bool motion_tick_before(TickType_t now, TickType_t deadline)
{
    return deadline != 0 && static_cast<int32_t>(now - deadline) < 0;
}


static bool motion_controls_allowed()
{
    DeviceSettingsSnapshot settings = {};
    if (!device_settings_get_snapshot(&settings) || !settings.motion_controls_enabled) {
        return false;
    }
    if (!app_manager_is_ready() || app_manager_foreground() != AppId::Music) {
        return false;
    }

    return screen_lock_simple_get_power() == ScreenPowerNormal &&
           screen_lock_simple_get_lock() == ScreenLockUnlocked &&
           !screen_action_menu_is_open();
}


static void motion_reset_flip()
{
    g_flip_phase = FlipPhase::Idle;
    g_flip_direction = 0;
    g_flip_angle_mdeg = 0;
    g_flip_peak_excursion_mdeg = 0;
    g_flip_return_seen = false;
    g_flip_pending_success = false;
    g_flip_pending_direction = 0;
    g_flip_pending_excursion_mdeg = 0;
    g_flip_recenter_stable_since = 0;
    g_flip_axis_x_mg = 0;
    g_flip_axis_y_mg = 0;
    g_flip_axis_norm_mg = 0;
    g_flip_started_tick = 0;
    g_flip_reached_tick = 0;
    g_flip_last_sample_tick = 0;
}


static void motion_reset_activity_baseline()
{
    g_accel_baseline_ready = false;
    g_accel_baseline_x = 0;
    g_accel_baseline_y = 0;
    g_accel_baseline_z = 0;
    g_motion_run_started_tick = 0;
    g_motion_run_last_tick = 0;
    g_pocket_motion_until = 0;
}


static void motion_update_pocket_activity(int32_t dynamic_peak_mg, TickType_t now)
{
    if (dynamic_peak_mg < POCKET_MOTION_MG) {
        return;
    }

    if (g_motion_run_last_tick == 0 ||
        now - g_motion_run_last_tick > POCKET_MOTION_CONTINUE_GAP) {
        g_motion_run_started_tick = now;
    }
    g_motion_run_last_tick = now;

    if (g_motion_run_started_tick != 0 &&
        now - g_motion_run_started_tick >= POCKET_MOTION_CONFIRM) {
        g_pocket_motion_until = now + POCKET_MOTION_HOLD;
    }
}


static void motion_process_activity(const Qmi8658AccelSample &sample, TickType_t now)
{
    if (!g_accel_baseline_ready) {
        g_accel_baseline_x = sample.mg_x;
        g_accel_baseline_y = sample.mg_y;
        g_accel_baseline_z = sample.mg_z;
        g_accel_baseline_ready = true;
        return;
    }

    const int32_t dx = static_cast<int32_t>(sample.mg_x) - g_accel_baseline_x;
    const int32_t dy = static_cast<int32_t>(sample.mg_y) - g_accel_baseline_y;
    const int32_t dz = static_cast<int32_t>(sample.mg_z) - g_accel_baseline_z;

    g_accel_baseline_x += (static_cast<int32_t>(sample.mg_x) - g_accel_baseline_x) / 32;
    g_accel_baseline_y += (static_cast<int32_t>(sample.mg_y) - g_accel_baseline_y) / 32;
    g_accel_baseline_z += (static_cast<int32_t>(sample.mg_z) - g_accel_baseline_z) / 32;

    int32_t peak = motion_abs_i32(dx);
    const int32_t ay = motion_abs_i32(dy);
    const int32_t az = motion_abs_i32(dz);
    if (ay > peak) {
        peak = ay;
    }
    if (az > peak) {
        peak = az;
    }
    motion_update_pocket_activity(peak, now);
}


static void IRAM_ATTR motion_int1_isr(void *)
{
    g_int1_pending = true;
}


static void motion_handle_double_tap(TickType_t now)
{
    if (motion_tick_before(now, g_tap_cooldown_until) ||
        motion_tick_before(now, g_tap_suppress_until)) {
        return;
    }

    motion_reset_flip();
    g_flip_suppress_until = now + CROSS_GESTURE_SUPPRESS;
    g_tap_cooldown_until = now + DOUBLE_TAP_COOLDOWN;

    if (!motion_controls_allowed()) {
#if FAKEPOD_MOTION_DIAGNOSTIC_LOG
        ESP_LOGI(TAG, "DoubleTap已识别：亮屏/解锁/Music门控未满足，忽略播放器动作");
#endif
        return;
    }
    if (motion_tick_before(now, g_pocket_motion_until)) {
#if FAKEPOD_MOTION_DIAGNOSTIC_LOG
        ESP_LOGI(TAG, "DoubleTap已识别：持续运动中，Pocket Guard抑制");
#endif
        return;
    }

    if (player_control_toggle_play_pause()) {
        ESP_LOGI(TAG, "DoubleTap → 播放/暂停");
    } else {
        ESP_LOGW(TAG, "DoubleTap已识别，但播放/暂停请求未执行");
    }
}


static void motion_process_int1(TickType_t now)
{
    if (!g_int1_pending) {
        return;
    }
    g_int1_pending = false;

    Qmi8658TapEvent tap = {};
    const esp_err_t ret = qmi8658_read_tap_event(&tap);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取INT1/Tap状态失败：%s", esp_err_to_name(ret));
        return;
    }

#if FAKEPOD_MOTION_DIAGNOSTIC_LOG
    ESP_LOGI(
        TAG,
        "INT1：tap=%u axis=%u polarity=%s",
        static_cast<unsigned>(tap.count),
        static_cast<unsigned>(tap.axis),
        tap.negative ? "NEG" : "POS");
#endif

    if (tap.count == 2U) {
        motion_handle_double_tap(now);
    }
}


static int32_t motion_approx_norm2(int32_t x, int32_t y)
{
    const int32_t ax = motion_abs_i32(x);
    const int32_t ay = motion_abs_i32(y);
    const int32_t hi = ax >= ay ? ax : ay;
    const int32_t lo = ax >= ay ? ay : ax;
    return hi + (lo / 2);
}


static bool motion_capture_pitch_axis(
    const Qmi8658AccelSample &accel,
    int32_t *out_axis_x_mg,
    int32_t *out_axis_y_mg,
    int32_t *out_axis_norm_mg)
{
    if (out_axis_x_mg == nullptr || out_axis_y_mg == nullptr || out_axis_norm_mg == nullptr) {
        return false;
    }

    // Gravity is only trusted while the gesture is idle. Once a Flip starts, the
    // reference is frozen because hand acceleration during the outbound/return stroke
    // is no longer a clean gravity vector. Recomputing it mid-gesture can rotate the
    // Pitch coordinate system and misinterpret the return stroke as a new opposite Flip.
    const int32_t gravity_xy = motion_approx_norm2(accel.mg_x, accel.mg_y);
    if (gravity_xy < FLIP_GRAVITY_XY_MIN_MG) {
        return false;
    }

    *out_axis_x_mg = accel.mg_x;
    *out_axis_y_mg = accel.mg_y;
    *out_axis_norm_mg = gravity_xy;
    return true;
}


static void motion_compute_pitch_rates_on_axis(
    const Qmi8658GyroSample &gyro,
    int32_t axis_x_mg,
    int32_t axis_y_mg,
    int32_t axis_norm_mg,
    int32_t *out_pitch_mdps,
    int32_t *out_roll_mdps,
    int32_t *out_yaw_mdps)
{
    const int64_t pitch_num =
        static_cast<int64_t>(gyro.mdps_x) * axis_y_mg -
        static_cast<int64_t>(gyro.mdps_y) * axis_x_mg;
    const int64_t roll_num =
        static_cast<int64_t>(gyro.mdps_x) * axis_x_mg +
        static_cast<int64_t>(gyro.mdps_y) * axis_y_mg;

    *out_pitch_mdps = static_cast<int32_t>(pitch_num / axis_norm_mg);
    *out_roll_mdps = static_cast<int32_t>(roll_num / axis_norm_mg);
    *out_yaw_mdps = gyro.mdps_z;
}


static bool motion_pitch_dominant(int32_t pitch_mdps, int32_t roll_mdps, int32_t yaw_mdps)
{
    const int32_t pitch_abs = motion_abs_i32(pitch_mdps);
    int32_t other_abs = motion_abs_i32(roll_mdps);
    const int32_t yaw_abs = motion_abs_i32(yaw_mdps);
    if (yaw_abs > other_abs) {
        other_abs = yaw_abs;
    }

    // Pitch must be at least ~1.2x the strongest roll/yaw component at gesture start.
    return static_cast<int64_t>(pitch_abs) * 5 >= static_cast<int64_t>(other_abs) * 6;
}


static const char *motion_flip_name(int8_t direction)
{
    return direction == FLIP_FORWARD_PITCH_SIGN ? "前翻" : "后翻";
}


static void motion_start_flip(
    int32_t pitch_mdps,
    int32_t axis_x_mg,
    int32_t axis_y_mg,
    int32_t axis_norm_mg,
    TickType_t now)
{
    g_flip_phase = FlipPhase::Outbound;
    g_flip_direction = pitch_mdps >= 0 ? 1 : -1;
    g_flip_angle_mdeg = 0;
    g_flip_peak_excursion_mdeg = 0;
    g_flip_return_seen = false;
    g_flip_pending_success = false;
    g_flip_pending_direction = 0;
    g_flip_pending_excursion_mdeg = 0;
    g_flip_recenter_stable_since = 0;
    g_flip_axis_x_mg = axis_x_mg;
    g_flip_axis_y_mg = axis_y_mg;
    g_flip_axis_norm_mg = axis_norm_mg;
    g_flip_started_tick = now;
    g_flip_reached_tick = 0;
    g_flip_last_sample_tick = now;

#if FAKEPOD_MOTION_FLIP_TUNING_LOG
    ESP_LOGI(
        TAG,
        "Flip候选：%s pitch=%ld.%03lddps",
        motion_flip_name(g_flip_direction),
        static_cast<long>(pitch_mdps / 1000),
        static_cast<long>(motion_abs_i32(pitch_mdps % 1000)));
#endif
}


static void motion_trigger_flip(int8_t direction, int32_t excursion_mdeg, TickType_t now)
{
    motion_reset_flip();
    g_flip_cooldown_until = now + FLIP_COOLDOWN;
    g_tap_suppress_until = now + CROSS_GESTURE_SUPPRESS;

    if (!motion_controls_allowed()) {
#if FAKEPOD_MOTION_DIAGNOSTIC_LOG
        ESP_LOGI(TAG, "Flip已确认：亮屏/解锁/Music门控未满足，忽略播放器动作");
#endif
        return;
    }

    const bool forward = direction == FLIP_FORWARD_PITCH_SIGN;
    const bool ok = forward ? player_control_next() : player_control_previous();
    if (ok) {
        ESP_LOGI(
            TAG,
            "%s confirmed → %s（excursion=%ld.%01ld°）",
            forward ? "ForwardFlip" : "BackwardFlip",
            forward ? "下一首" : "上一首",
            static_cast<long>(excursion_mdeg / 1000),
            static_cast<long>((excursion_mdeg % 1000) / 100));
    } else {
        ESP_LOGW(
            TAG,
            "%s已确认，但%s请求未执行",
            forward ? "前翻" : "后翻",
            forward ? "下一首" : "上一首");
    }
}


static bool motion_accel_near_gravity(const Qmi8658AccelSample &accel)
{
    const int64_t norm_sq =
        static_cast<int64_t>(accel.mg_x) * accel.mg_x +
        static_cast<int64_t>(accel.mg_y) * accel.mg_y +
        static_cast<int64_t>(accel.mg_z) * accel.mg_z;
    const int64_t min_sq =
        static_cast<int64_t>(FLIP_RECENTER_ACCEL_MIN_MG) * FLIP_RECENTER_ACCEL_MIN_MG;
    const int64_t max_sq =
        static_cast<int64_t>(FLIP_RECENTER_ACCEL_MAX_MG) * FLIP_RECENTER_ACCEL_MAX_MG;
    return norm_sq >= min_sq && norm_sq <= max_sq;
}


static bool motion_gyro_stable(const Qmi8658GyroSample &gyro)
{
    return motion_abs_i32(gyro.mdps_x) <= FLIP_RECENTER_GYRO_MDPS &&
           motion_abs_i32(gyro.mdps_y) <= FLIP_RECENTER_GYRO_MDPS &&
           motion_abs_i32(gyro.mdps_z) <= FLIP_RECENTER_GYRO_MDPS;
}


static void motion_enter_recenter(bool success)
{
    g_flip_phase = FlipPhase::Recenter;
    g_flip_pending_success = success;
    g_flip_pending_direction = g_flip_direction;
    g_flip_pending_excursion_mdeg = g_flip_peak_excursion_mdeg;
    g_flip_recenter_stable_since = 0;

#if FAKEPOD_MOTION_FLIP_TUNING_LOG
    ESP_LOGI(
        TAG,
        "%s回位：进入动态零点确认（angle=%ld.%01ld°）",
        success ? "Flip成功" : "Flip失败",
        static_cast<long>(g_flip_angle_mdeg / 1000),
        static_cast<long>(motion_abs_i64(g_flip_angle_mdeg % 1000) / 100));
#endif
}


static void motion_enter_failed_return(const char *reason, TickType_t now)
{
    if (g_flip_phase == FlipPhase::ReturningFailed || g_flip_phase == FlipPhase::Recenter) {
        return;
    }

    g_flip_phase = FlipPhase::ReturningFailed;
    g_flip_return_seen = false;
#if FAKEPOD_MOTION_FLIP_TUNING_LOG
    ESP_LOGI(
        TAG,
        "Flip失败：%s；等待回到起始零点后再复位（peak=%ld.%01ld°）",
        reason,
        static_cast<long>(g_flip_peak_excursion_mdeg / 1000),
        static_cast<long>((g_flip_peak_excursion_mdeg % 1000) / 100));
#endif

    // If the failed gesture is already back in the origin band, do not invent another
    // return stroke. Recenter will still require a short stable hold before zeroing.
    if (motion_abs_i64(g_flip_angle_mdeg) <= FLIP_RETURN_BAND_MDEG) {
        motion_enter_recenter(false);
    }
}


static void motion_process_recenter(
    const Qmi8658AccelSample &accel,
    const Qmi8658GyroSample &gyro,
    TickType_t now)
{
    // Recenter does not demand a particular absolute holding angle. Once the user has
    // physically returned to the original relative-angle band, a short period of low
    // angular velocity + near-1g acceleration is enough. The next gesture captures its
    // own Pitch axis from whatever natural pose the user is holding then.
    const bool stable =
        motion_gyro_stable(gyro) &&
        motion_accel_near_gravity(accel);

    if (!stable) {
        g_flip_recenter_stable_since = 0;
        return;
    }

    if (g_flip_recenter_stable_since == 0) {
        g_flip_recenter_stable_since = now;
        return;
    }
    if (now - g_flip_recenter_stable_since < FLIP_RECENTER_STABLE) {
        return;
    }

    const bool success = g_flip_pending_success;
    const int8_t direction = g_flip_pending_direction;
    const int32_t excursion = g_flip_pending_excursion_mdeg;

#if FAKEPOD_MOTION_FLIP_TUNING_LOG
    ESP_LOGI(
        TAG,
        "Flip动态零点已重建：%s，当前自然握持姿态=0°",
        success ? "成功动作结算" : "失败动作清零");
#endif

    if (success) {
        motion_trigger_flip(direction, excursion, now);
    } else {
        motion_reset_flip();
    }
}


static void motion_process_flip(
    const Qmi8658AccelSample &accel,
    const Qmi8658GyroSample &gyro,
    TickType_t now)
{
    if (motion_tick_before(now, g_gyro_warmup_until) ||
        motion_tick_before(now, g_flip_cooldown_until) ||
        motion_tick_before(now, g_flip_suppress_until)) {
        motion_reset_flip();
        return;
    }

    if (g_flip_phase == FlipPhase::Recenter) {
        motion_process_recenter(accel, gyro, now);
        return;
    }

    int32_t pitch_mdps = 0;
    int32_t roll_mdps = 0;
    int32_t yaw_mdps = 0;

    if (g_flip_phase == FlipPhase::Idle) {
        int32_t axis_x_mg = 0;
        int32_t axis_y_mg = 0;
        int32_t axis_norm_mg = 0;
        if (!motion_capture_pitch_axis(accel, &axis_x_mg, &axis_y_mg, &axis_norm_mg)) {
            return;
        }

        motion_compute_pitch_rates_on_axis(
            gyro,
            axis_x_mg,
            axis_y_mg,
            axis_norm_mg,
            &pitch_mdps,
            &roll_mdps,
            &yaw_mdps);

        if (motion_abs_i32(pitch_mdps) >= FLIP_START_RATE_MDPS &&
            motion_pitch_dominant(pitch_mdps, roll_mdps, yaw_mdps)) {
            motion_start_flip(pitch_mdps, axis_x_mg, axis_y_mg, axis_norm_mg, now);
        }
        return;
    }

    // Outbound and both Returning states use the same Pitch axis frozen at gesture start.
    // A failed gesture is NOT zeroed at its failed posture; it keeps integrating on this
    // axis until the user returns to the original relative-angle band.
    motion_compute_pitch_rates_on_axis(
        gyro,
        g_flip_axis_x_mg,
        g_flip_axis_y_mg,
        g_flip_axis_norm_mg,
        &pitch_mdps,
        &roll_mdps,
        &yaw_mdps);

    const int32_t pitch_abs = motion_abs_i32(pitch_mdps);
    int32_t cross_abs = motion_abs_i32(roll_mdps);
    const int32_t yaw_abs = motion_abs_i32(yaw_mdps);
    if (yaw_abs > cross_abs) {
        cross_abs = yaw_abs;
    }

    TickType_t dt_ticks = now - g_flip_last_sample_tick;
    g_flip_last_sample_tick = now;
    if (dt_ticks == 0) {
        return;
    }
    const TickType_t max_dt = pdMS_TO_TICKS(30);
    if (dt_ticks > max_dt) {
        dt_ticks = max_dt;
    }
    const int32_t dt_ms = static_cast<int32_t>(dt_ticks * portTICK_PERIOD_MS);
    g_flip_angle_mdeg += (static_cast<int64_t>(pitch_mdps) * dt_ms) / 1000;

    const int64_t signed_excursion =
        static_cast<int64_t>(g_flip_direction) * g_flip_angle_mdeg;
    if (signed_excursion > g_flip_peak_excursion_mdeg) {
        g_flip_peak_excursion_mdeg = static_cast<int32_t>(signed_excursion);
    }

    if (g_flip_phase == FlipPhase::Outbound) {
        // Natural wrist Roll is tolerated. A clear tumble makes the gesture fail, but
        // even that failure must return to the original zero before the detector rearms.
        if (cross_abs >= FLIP_CROSS_AXIS_ABORT_MDPS &&
            static_cast<int64_t>(cross_abs) > static_cast<int64_t>(pitch_abs) * 2) {
            motion_enter_failed_return("明显多轴翻滚", now);
            return;
        }

        if (g_flip_peak_excursion_mdeg >= FLIP_MIN_EXCURSION_MDEG) {
            g_flip_phase = FlipPhase::ReturningSuccess;
            g_flip_reached_tick = now;
#if FAKEPOD_MOTION_FLIP_TUNING_LOG
            ESP_LOGI(
                TAG,
                "Flip到位：%s excursion=%ld.%01ld°，等待回位",
                motion_flip_name(g_flip_direction),
                static_cast<long>(g_flip_peak_excursion_mdeg / 1000),
                static_cast<long>((g_flip_peak_excursion_mdeg % 1000) / 100));
#endif
            return;
        }

        const bool early_return =
            g_flip_peak_excursion_mdeg >= FLIP_FAILED_RETURN_MIN_MDEG &&
            static_cast<int64_t>(g_flip_direction) * pitch_mdps <= -FLIP_RETURN_RATE_MDPS;
        if (early_return) {
            motion_enter_failed_return("未达到35°就开始回摆", now);
            return;
        }

        if (now - g_flip_started_tick > FLIP_OUTBOUND_TIMEOUT) {
            motion_enter_failed_return("前摆未在550ms内达到35°", now);
            return;
        }
        return;
    }

    // Once the outbound stroke has reached the target angle, the user must return
    // promptly. A late return is allowed to re-center the detector, but it must never
    // execute the track change. Keep the older full-sequence timeout as a secondary
    // safety guard for abnormal timing.
    if (g_flip_phase == FlipPhase::ReturningSuccess &&
        g_flip_reached_tick != 0 &&
        now - g_flip_reached_tick > FLIP_RETURN_TIMEOUT) {
        motion_enter_failed_return("到位后未在500ms内回位，取消切歌资格", now);
        return;
    }

    if (g_flip_phase == FlipPhase::ReturningSuccess &&
        now - g_flip_started_tick > FLIP_SEQUENCE_TIMEOUT) {
        motion_enter_failed_return("完整动作超时，取消切歌资格", now);
        return;
    }

    const bool returning_rate =
        static_cast<int64_t>(g_flip_direction) * pitch_mdps <= -FLIP_RETURN_RATE_MDPS;
    if (returning_rate && !g_flip_return_seen) {
        g_flip_return_seen = true;
#if FAKEPOD_MOTION_FLIP_TUNING_LOG
        ESP_LOGI(
            TAG,
            "Flip回摆：%s pitch=%ld.%03lddps",
            motion_flip_name(g_flip_direction),
            static_cast<long>(pitch_mdps / 1000),
            static_cast<long>(motion_abs_i32(pitch_mdps % 1000)));
#endif
    }

    const bool back_near_origin =
        motion_abs_i64(g_flip_angle_mdeg) <= FLIP_RETURN_BAND_MDEG;
    if (back_near_origin &&
        now - g_flip_started_tick >= FLIP_MIN_ACTION_SPAN &&
        (g_flip_return_seen || g_flip_phase == FlipPhase::ReturningFailed)) {
        motion_enter_recenter(g_flip_phase == FlipPhase::ReturningSuccess);
    }
}

esp_err_t motion_service_init()
{
    if (g_ready) {
        return ESP_OK;
    }
    if (!qmi8658_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = qmi8658_configure_motion_profile();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 Motion profile配置失败：%s", esp_err_to_name(ret));
        return ret;
    }

    Qmi8658TapEvent stale = {};
    (void)qmi8658_read_tap_event(&stale);

    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << FAKEPOD_IMU_INT1;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_POSEDGE;

    ret = gpio_config(&config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d IMU_INT1配置失败：%s", FAKEPOD_IMU_INT1, esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_isr_handler_add(
        static_cast<gpio_num_t>(FAKEPOD_IMU_INT1),
        motion_int1_isr,
        nullptr);
    if (ret == ESP_ERR_INVALID_STATE) {
        const esp_err_t install_ret = gpio_install_isr_service(0);
        if (install_ret != ESP_OK) {
            ESP_LOGW(TAG, "GPIO ISR service安装失败：%s", esp_err_to_name(install_ret));
            return install_ret;
        }
        ret = gpio_isr_handler_add(
            static_cast<gpio_num_t>(FAKEPOD_IMU_INT1),
            motion_int1_isr,
            nullptr);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d IMU_INT1 ISR注册失败：%s", FAKEPOD_IMU_INT1, esp_err_to_name(ret));
        return ret;
    }

    g_int1_pending = false;
    g_last_imu_sample_tick = 0;
    g_last_imu_read_error_log_tick = 0;
    g_flip_cooldown_until = 0;
    g_tap_cooldown_until = 0;
    g_tap_suppress_until = 0;
    g_flip_suppress_until = 0;
    g_controls_allowed_last = false;
    motion_reset_activity_baseline();
    motion_reset_flip();
    g_gyro_warmup_until = xTaskGetTickCount() + GYRO_WARMUP;
    g_ready = true;

    ESP_LOGI(
        TAG,
        "Motion Controls V2.2就绪：INT1=GPIO%d ForwardFlip=下一首 BackwardFlip=上一首 DoubleTap=播放/暂停；设置-系统可关闭",
        FAKEPOD_IMU_INT1);
    ESP_LOGI(TAG, "Flip：到位后500ms内回位才生效；超时仅回位复位；动态零点Recenter；Pocket Guard=Normal+Unlocked+Music");
    return ESP_OK;
}


void motion_service_update()
{
    if (!g_ready) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();

    // INT1 ISR只记边沿；I2C和Player动作仍在loopTask普通上下文执行。
    motion_process_int1(now);

    const bool controls_allowed = motion_controls_allowed();
    if (!controls_allowed) {
        if (g_controls_allowed_last) {
            motion_reset_flip();
            motion_reset_activity_baseline();
        }
        g_controls_allowed_last = false;
        return;
    }

    if (!g_controls_allowed_last) {
        g_controls_allowed_last = true;
        motion_reset_flip();
        motion_reset_activity_baseline();
        g_last_imu_sample_tick = 0;
        g_gyro_warmup_until = now + GYRO_WARMUP;
    }

    if (g_last_imu_sample_tick != 0 &&
        now - g_last_imu_sample_tick < IMU_SAMPLE_INTERVAL) {
        return;
    }
    g_last_imu_sample_tick = now;

    Qmi8658AccelSample accel = {};
    Qmi8658GyroSample gyro = {};
    const esp_err_t ret = qmi8658_read_motion(&accel, &gyro);
    if (ret != ESP_OK) {
        if (g_last_imu_read_error_log_tick == 0 ||
            now - g_last_imu_read_error_log_tick >= IMU_READ_ERROR_LOG_INTERVAL) {
            g_last_imu_read_error_log_tick = now;
            ESP_LOGW(TAG, "读取Accel/Gyro失败：%s（后续同类错误限频2s）", esp_err_to_name(ret));
        }
        return;
    }

    motion_process_activity(accel, now);
    motion_process_flip(accel, gyro, now);
}
