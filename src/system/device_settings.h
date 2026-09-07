#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Settings V1：设备级设置数据模型。
// 这里只保存“用户意图”；硬件应用由对应服务完成，避免 Settings UI 直接碰驱动。
enum class DeviceUsbMode : uint8_t {
    Serial = 0,
    TfCard,
};

enum class DeviceAudioOutputMode : uint8_t {
    NormalHeadphones = 0,
    LineOut,
    HighImpedanceHeadphones,
};

enum class DeviceAuxKeyMode : uint8_t {
    Volume = 0,
    Track,
};

enum class DeviceAnimationMode : uint8_t {
    Auto = 0,
    Full,
    Eco,
};

enum class DeviceMusicListScope : uint8_t {
    All = 0,
    Level1,
    Level2,
};

static constexpr size_t DEVICE_MUSIC_FOLDER_PATH_MAX = 384U;

struct DeviceMusicListSelection {
    DeviceMusicListScope scope = DeviceMusicListScope::All;
    char level1_path[DEVICE_MUSIC_FOLDER_PATH_MAX] = {};
    char level2_path[DEVICE_MUSIC_FOLDER_PATH_MAX] = {};
};

struct DeviceSettingsSnapshot {
    bool ready = false;
    bool loaded_from_nvs = false;
    DeviceUsbMode usb_mode = DeviceUsbMode::Serial;
    DeviceAudioOutputMode audio_output_mode = DeviceAudioOutputMode::NormalHeadphones;
    uint8_t brightness_level = 60U;          // CO5300 正常显示亮度 5~100%；0 保留给熄屏。
    DeviceAuxKeyMode aux_key_mode = DeviceAuxKeyMode::Volume;
    uint16_t auto_screen_off_seconds = 0U;   // 0=永不。
    bool aod_enabled = true;
    DeviceAnimationMode animation_mode = DeviceAnimationMode::Auto;
    DeviceMusicListScope music_list_scope = DeviceMusicListScope::All;
    bool motion_controls_enabled = true;
    bool remember_volume = true;
};

// NVS namespace: device_cfg。初始化只加载设置，不主动应用硬件状态。
esp_err_t device_settings_init();
bool device_settings_get_snapshot(DeviceSettingsSnapshot *out_snapshot);

// 后续功能模块统一通过这些 setter 修改设置；每次只提交一个很小的 NVS 事务。
esp_err_t device_settings_set_usb_mode(DeviceUsbMode mode);
esp_err_t device_settings_set_audio_output_mode(DeviceAudioOutputMode mode);
esp_err_t device_settings_set_brightness_level(uint8_t level);
esp_err_t device_settings_set_aux_key_mode(DeviceAuxKeyMode mode);
esp_err_t device_settings_set_auto_screen_off_seconds(uint16_t seconds);
esp_err_t device_settings_set_aod_enabled(bool enabled);
esp_err_t device_settings_set_animation_mode(DeviceAnimationMode mode);
esp_err_t device_settings_set_music_list_scope(DeviceMusicListScope scope);
esp_err_t device_settings_set_motion_controls_enabled(bool enabled);

// 播放列表目录选择与范围一起提交到同一个 NVS 事务。路径使用 Catalog 中的规范化完整目录路径，
// 以 '/' 结尾；总列表可传空路径。这样切换一级/二级列表时不会出现 scope 已保存但目录未保存的半状态。
bool device_settings_get_music_list_selection(DeviceMusicListSelection *out_selection);
esp_err_t device_settings_set_music_list_selection(
    DeviceMusicListScope scope,
    const char *level1_path,
    const char *level2_path);
esp_err_t device_settings_set_remember_volume(bool enabled);

// 恢复 Settings V1 默认值；只重置 device_cfg namespace，不触碰音乐持久化/TF 卡文件。
esp_err_t device_settings_reset_defaults();

const char *device_settings_usb_mode_name(DeviceUsbMode mode);
const char *device_settings_audio_output_mode_name(DeviceAudioOutputMode mode);
const char *device_settings_audio_output_level_name(DeviceAudioOutputMode mode);
const char *device_settings_aux_key_mode_name(DeviceAuxKeyMode mode);
const char *device_settings_animation_mode_name(DeviceAnimationMode mode);
const char *device_settings_music_list_scope_name(DeviceMusicListScope scope);
