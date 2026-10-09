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

// R46.0.75：曲库入口与播放本身解耦。这里仅保存主页下拉时要浏览的数据源，
// 不因切换浏览源停止/切换当前正在播放的歌曲。
enum class DeviceMusicLibrarySource : uint8_t {
    Local = 0,
    Nas,
};

static constexpr size_t DEVICE_UI_FONT_FILENAME_MAX = 128U;
static constexpr size_t DEVICE_MUSIC_FOLDER_PATH_MAX = 384U;

struct DeviceMusicListSelection {
    DeviceMusicListScope scope = DeviceMusicListScope::All;
    char level1_path[DEVICE_MUSIC_FOLDER_PATH_MAX] = {};
    char level2_path[DEVICE_MUSIC_FOLDER_PATH_MAX] = {};
};

// R46.0.107：本地/NAS 各自保存轻量浏览位置；搜索态不跨重启。
// browse_mode 固定映射：0=歌曲 1=歌手 2=专辑 3=年代；folder_view 映射 LibraryFolderView。
struct DeviceMusicBrowseState {
    bool valid = false;
    uint8_t browse_mode = 0U;
    uint8_t folder_view = 0U;
    int32_t top_scroll_y[4] = {};
    int32_t folder_scroll_y[3] = {};
};

struct DeviceSettingsSnapshot {
    bool ready = false;
    bool loaded_from_nvs = false;
    DeviceUsbMode usb_mode = DeviceUsbMode::Serial;
    bool ble_enabled = false;
    bool wifi_enabled = false;
    DeviceAudioOutputMode audio_output_mode = DeviceAudioOutputMode::NormalHeadphones;
    uint8_t brightness_level = 60U;          // CO5300 正常显示亮度 5~100%；0 保留给熄屏。
    DeviceAuxKeyMode aux_key_mode = DeviceAuxKeyMode::Volume;
    uint16_t auto_screen_off_seconds = 0U;   // 0=永不。
    bool aod_enabled = true;
    DeviceAnimationMode animation_mode = DeviceAnimationMode::Auto;
    DeviceMusicListScope music_list_scope = DeviceMusicListScope::All;
    DeviceMusicLibrarySource music_library_source = DeviceMusicLibrarySource::Local;
    bool cassette_dynamic_tint_enabled = false;
    bool motion_controls_enabled = true;
    uint8_t nsf_gain_compensation_db = 3U;  // 电子音流专用 CS43131 数字衰减补偿，0~6dB。
    bool remember_volume = true;
    char ui_font_file[DEVICE_UI_FONT_FILENAME_MAX] = {};  // /FONTS 下的 UTF-8 文件名；空=自动选择。
};

// NVS namespace: device_cfg。初始化只加载设置，不主动应用硬件状态。
esp_err_t device_settings_init();
bool device_settings_get_snapshot(DeviceSettingsSnapshot *out_snapshot);
// 轻量查询：Settings 未就绪时沿用历史默认行为（记住音量=true），避免调用方复制完整快照到栈。
bool device_settings_remember_volume_enabled();
// R46.0.68 迁移辅助：旧版 Settings V1 尚无 Wi-Fi 开关键。
bool device_settings_wifi_enabled_is_explicit();
// 主页手势热路径只读查询，避免为一个枚举复制完整 SettingsSnapshot。
DeviceMusicLibrarySource device_settings_music_library_source();
DeviceMusicListScope device_settings_music_list_scope();

// 后续功能模块统一通过这些 setter 修改设置；每次只提交一个很小的 NVS 事务。
esp_err_t device_settings_set_usb_mode(DeviceUsbMode mode);
esp_err_t device_settings_set_ble_enabled(bool enabled);
// 原子保存互斥无线意图：允许 BLE/Wi-Fi 都关闭，但不允许同时开启。
esp_err_t device_settings_set_wireless_enabled(bool ble_enabled, bool wifi_enabled);
esp_err_t device_settings_set_audio_output_mode(DeviceAudioOutputMode mode);
esp_err_t device_settings_set_brightness_level(uint8_t level);
esp_err_t device_settings_set_aux_key_mode(DeviceAuxKeyMode mode);
esp_err_t device_settings_set_auto_screen_off_seconds(uint16_t seconds);
esp_err_t device_settings_set_aod_enabled(bool enabled);
esp_err_t device_settings_set_animation_mode(DeviceAnimationMode mode);
esp_err_t device_settings_set_music_list_scope(DeviceMusicListScope scope);
esp_err_t device_settings_set_music_library_source(DeviceMusicLibrarySource source);
esp_err_t device_settings_set_cassette_dynamic_tint_enabled(bool enabled);
esp_err_t device_settings_set_motion_controls_enabled(bool enabled);
esp_err_t device_settings_set_nsf_gain_compensation_db(uint8_t db);
esp_err_t device_settings_set_ui_font_file(const char *filename);

// 播放列表目录选择与范围一起提交到同一个 NVS 事务。路径使用 Catalog 中的规范化完整目录路径，
// 以 '/' 结尾；总列表可传空路径。这样切换一级/二级列表时不会出现 scope 已保存但目录未保存的半状态。
bool device_settings_get_music_list_selection(DeviceMusicListSelection *out_selection);
// 明确来源版本：播放器 Local runtime 与 NAS 浏览恢复不得再共用同一个 scope/path。
bool device_settings_get_music_list_selection_for_source(
    DeviceMusicLibrarySource source,
    DeviceMusicListSelection *out_selection);
esp_err_t device_settings_set_music_list_selection(
    DeviceMusicListScope scope,
    const char *level1_path,
    const char *level2_path);
esp_err_t device_settings_set_music_list_selection_for_source(
    DeviceMusicLibrarySource source,
    DeviceMusicListScope scope,
    const char *level1_path,
    const char *level2_path);
bool device_settings_get_music_browse_state(
    DeviceMusicLibrarySource source,
    DeviceMusicBrowseState *out_state);
esp_err_t device_settings_set_music_browse_state(
    DeviceMusicLibrarySource source,
    const DeviceMusicBrowseState &state);
esp_err_t device_settings_set_remember_volume(bool enabled);

// 恢复 Settings V1 默认值；只重置 device_cfg namespace，不触碰音乐持久化/TF 卡文件。
esp_err_t device_settings_reset_defaults();

const char *device_settings_usb_mode_name(DeviceUsbMode mode);
const char *device_settings_audio_output_mode_name(DeviceAudioOutputMode mode);
const char *device_settings_audio_output_level_name(DeviceAudioOutputMode mode);
const char *device_settings_aux_key_mode_name(DeviceAuxKeyMode mode);
const char *device_settings_animation_mode_name(DeviceAnimationMode mode);
const char *device_settings_music_list_scope_name(DeviceMusicListScope scope);
const char *device_settings_music_library_source_name(DeviceMusicLibrarySource source);
