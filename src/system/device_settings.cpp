#include "device_settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "设备设置";

namespace {

static constexpr uint16_t kSchemaVersion = 1U;
static constexpr const char *kNamespace = "device_cfg";

static DeviceSettingsSnapshot g_settings = {};
static DeviceMusicListSelection g_music_selection = {};

static DeviceMusicListSelection make_music_selection_defaults()
{
    DeviceMusicListSelection defaults = {};
    defaults.scope = DeviceMusicListScope::All;
    return defaults;
}

static DeviceSettingsSnapshot make_defaults()
{
    DeviceSettingsSnapshot defaults = {};
    defaults.ready = false;
    defaults.loaded_from_nvs = false;
    defaults.usb_mode = DeviceUsbMode::Serial;
    defaults.audio_output_mode = DeviceAudioOutputMode::NormalHeadphones;
    defaults.brightness_level = 60U;
    defaults.aux_key_mode = DeviceAuxKeyMode::Volume;
    defaults.auto_screen_off_seconds = 0U;
    defaults.aod_enabled = true;
    defaults.animation_mode = DeviceAnimationMode::Auto;
    defaults.music_list_scope = DeviceMusicListScope::All;
    defaults.motion_controls_enabled = true;
    defaults.remember_volume = true;
    return defaults;
}

static bool usb_mode_valid(uint8_t raw)
{
    return raw <= static_cast<uint8_t>(DeviceUsbMode::TfCard);
}

static bool audio_output_mode_valid(uint8_t raw)
{
    return raw <= static_cast<uint8_t>(DeviceAudioOutputMode::HighImpedanceHeadphones);
}

static bool aux_key_mode_valid(uint8_t raw)
{
    return raw <= static_cast<uint8_t>(DeviceAuxKeyMode::Track);
}

static bool auto_screen_off_valid(uint16_t seconds)
{
    switch (seconds) {
        case 0U:
        case 10U:
        case 30U:
        case 60U:
        case 120U:
        case 180U:
            return true;
        default:
            return false;
    }
}

static bool animation_mode_valid(uint8_t raw)
{
    return raw <= static_cast<uint8_t>(DeviceAnimationMode::Eco);
}

static bool music_list_scope_valid(uint8_t raw)
{
    return raw <= static_cast<uint8_t>(DeviceMusicListScope::Level2);
}

static esp_err_t open_rw(nvs_handle_t *out_handle)
{
    if (out_handle == nullptr || !g_settings.ready) return ESP_ERR_INVALID_STATE;
    return nvs_open(kNamespace, NVS_READWRITE, out_handle);
}

static esp_err_t commit_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = open_rw(&handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u16(handle, "schema", kSchemaVersion);
    if (ret == ESP_OK) ret = nvs_set_u8(handle, key, value);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

static esp_err_t commit_u16(const char *key, uint16_t value)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = open_rw(&handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u16(handle, "schema", kSchemaVersion);
    if (ret == ESP_OK) ret = nvs_set_u16(handle, key, value);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

static void log_commit_failure(const char *key, esp_err_t ret)
{
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "保存设置失败：key=%s ret=%s", key, esp_err_to_name(ret));
    }
}

} // namespace

esp_err_t device_settings_init()
{
    if (g_settings.ready) return ESP_OK;
    g_settings = make_defaults();
    g_music_selection = make_music_selection_defaults();

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        g_settings.ready = true;
        ESP_LOGI(TAG, "Settings V1 尚无NVS数据，使用默认值");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "打开Settings NVS失败：%s；本轮只使用RAM默认值", esp_err_to_name(ret));
        return ret;
    }

    uint16_t schema = 0U;
    ret = nvs_get_u16(handle, "schema", &schema);
    if (ret != ESP_OK || schema != kSchemaVersion) {
        nvs_close(handle);
        g_settings.ready = true;
        ESP_LOGI(TAG, "Settings NVS无有效schema，保持V1默认值");
        return ESP_OK;
    }

    uint8_t u8 = 0U;
    uint16_t u16 = 0U;
    if (nvs_get_u8(handle, "usb", &u8) == ESP_OK && usb_mode_valid(u8)) {
        g_settings.usb_mode = static_cast<DeviceUsbMode>(u8);
    }
    if (nvs_get_u8(handle, "audioout", &u8) == ESP_OK && audio_output_mode_valid(u8)) {
        g_settings.audio_output_mode = static_cast<DeviceAudioOutputMode>(u8);
    }
    if (nvs_get_u8(handle, "bright", &u8) == ESP_OK && u8 >= 5U && u8 <= 100U) {
        g_settings.brightness_level = u8;
    }
    if (nvs_get_u8(handle, "aux", &u8) == ESP_OK && aux_key_mode_valid(u8)) {
        g_settings.aux_key_mode = static_cast<DeviceAuxKeyMode>(u8);
    }
    if (nvs_get_u16(handle, "screenoff", &u16) == ESP_OK) {
        if (auto_screen_off_valid(u16)) {
            g_settings.auto_screen_off_seconds = u16;
        } else if (u16 == 300U || u16 == 600U) {
            // 旧版允许 5/10 分钟；新版最长 3 分钟，加载时平滑收敛到 180 秒。
            g_settings.auto_screen_off_seconds = 180U;
        }
    }
    if (nvs_get_u8(handle, "aod", &u8) == ESP_OK && u8 <= 1U) {
        g_settings.aod_enabled = u8 != 0U;
    }
    if (nvs_get_u8(handle, "anim", &u8) == ESP_OK && animation_mode_valid(u8)) {
        g_settings.animation_mode = static_cast<DeviceAnimationMode>(u8);
    }
    if (nvs_get_u8(handle, "muscope", &u8) == ESP_OK && music_list_scope_valid(u8)) {
        g_settings.music_list_scope = static_cast<DeviceMusicListScope>(u8);
        g_music_selection.scope = g_settings.music_list_scope;
    }

    size_t path_bytes = sizeof(g_music_selection.level1_path);
    if (nvs_get_str(handle, "mul1", g_music_selection.level1_path, &path_bytes) != ESP_OK) {
        g_music_selection.level1_path[0] = '\0';
    }
    path_bytes = sizeof(g_music_selection.level2_path);
    if (nvs_get_str(handle, "mul2", g_music_selection.level2_path, &path_bytes) != ESP_OK) {
        g_music_selection.level2_path[0] = '\0';
    }
    if (nvs_get_u8(handle, "motion", &u8) == ESP_OK && u8 <= 1U) {
        g_settings.motion_controls_enabled = u8 != 0U;
    }
    if (nvs_get_u8(handle, "memvol", &u8) == ESP_OK && u8 <= 1U) {
        g_settings.remember_volume = u8 != 0U;
    }

    nvs_close(handle);
    g_settings.ready = true;
    g_settings.loaded_from_nvs = true;
    ESP_LOGI(TAG, "Settings V1加载完成：USB=%s audio=%s bright=%u aux=%s motion=%s",
        device_settings_usb_mode_name(g_settings.usb_mode),
        device_settings_audio_output_mode_name(g_settings.audio_output_mode),
        static_cast<unsigned>(g_settings.brightness_level),
        device_settings_aux_key_mode_name(g_settings.aux_key_mode),
        g_settings.motion_controls_enabled ? "开" : "关");
    return ESP_OK;
}

bool device_settings_get_snapshot(DeviceSettingsSnapshot *out_snapshot)
{
    if (out_snapshot == nullptr) return false;
    *out_snapshot = g_settings;
    return g_settings.ready;
}

esp_err_t device_settings_set_usb_mode(DeviceUsbMode mode)
{
    if (!usb_mode_valid(static_cast<uint8_t>(mode))) return ESP_ERR_INVALID_ARG;
    const DeviceUsbMode old = g_settings.usb_mode;
    g_settings.usb_mode = mode;
    const esp_err_t ret = commit_u8("usb", static_cast<uint8_t>(mode));
    if (ret != ESP_OK) g_settings.usb_mode = old;
    log_commit_failure("usb", ret);
    return ret;
}

esp_err_t device_settings_set_audio_output_mode(DeviceAudioOutputMode mode)
{
    if (!audio_output_mode_valid(static_cast<uint8_t>(mode))) return ESP_ERR_INVALID_ARG;
    const DeviceAudioOutputMode old = g_settings.audio_output_mode;
    g_settings.audio_output_mode = mode;
    const esp_err_t ret = commit_u8("audioout", static_cast<uint8_t>(mode));
    if (ret != ESP_OK) g_settings.audio_output_mode = old;
    log_commit_failure("audioout", ret);
    return ret;
}

esp_err_t device_settings_set_brightness_level(uint8_t level)
{
    // Normal亮度不允许写0；真正的AOD/熄屏由screen_lock_simple独立控制。
    if (level < 5U || level > 100U) return ESP_ERR_INVALID_ARG;
    const uint8_t old = g_settings.brightness_level;
    g_settings.brightness_level = level;
    const esp_err_t ret = commit_u8("bright", level);
    if (ret != ESP_OK) g_settings.brightness_level = old;
    log_commit_failure("bright", ret);
    return ret;
}

esp_err_t device_settings_set_aux_key_mode(DeviceAuxKeyMode mode)
{
    if (!aux_key_mode_valid(static_cast<uint8_t>(mode))) return ESP_ERR_INVALID_ARG;
    const DeviceAuxKeyMode old = g_settings.aux_key_mode;
    g_settings.aux_key_mode = mode;
    const esp_err_t ret = commit_u8("aux", static_cast<uint8_t>(mode));
    if (ret != ESP_OK) g_settings.aux_key_mode = old;
    log_commit_failure("aux", ret);
    return ret;
}

esp_err_t device_settings_set_auto_screen_off_seconds(uint16_t seconds)
{
    if (!auto_screen_off_valid(seconds)) return ESP_ERR_INVALID_ARG;
    const uint16_t old = g_settings.auto_screen_off_seconds;
    g_settings.auto_screen_off_seconds = seconds;
    const esp_err_t ret = commit_u16("screenoff", seconds);
    if (ret != ESP_OK) g_settings.auto_screen_off_seconds = old;
    log_commit_failure("screenoff", ret);
    return ret;
}

esp_err_t device_settings_set_aod_enabled(bool enabled)
{
    const bool old = g_settings.aod_enabled;
    g_settings.aod_enabled = enabled;
    const esp_err_t ret = commit_u8("aod", enabled ? 1U : 0U);
    if (ret != ESP_OK) g_settings.aod_enabled = old;
    log_commit_failure("aod", ret);
    return ret;
}

esp_err_t device_settings_set_animation_mode(DeviceAnimationMode mode)
{
    if (!animation_mode_valid(static_cast<uint8_t>(mode))) return ESP_ERR_INVALID_ARG;
    const DeviceAnimationMode old = g_settings.animation_mode;
    g_settings.animation_mode = mode;
    const esp_err_t ret = commit_u8("anim", static_cast<uint8_t>(mode));
    if (ret != ESP_OK) g_settings.animation_mode = old;
    log_commit_failure("anim", ret);
    return ret;
}

bool device_settings_get_music_list_selection(DeviceMusicListSelection *out_selection)
{
    if (out_selection == nullptr || !g_settings.ready) return false;
    *out_selection = g_music_selection;
    out_selection->scope = g_settings.music_list_scope;
    return true;
}

esp_err_t device_settings_set_music_list_selection(
    DeviceMusicListScope scope,
    const char *level1_path,
    const char *level2_path)
{
    if (!music_list_scope_valid(static_cast<uint8_t>(scope))) return ESP_ERR_INVALID_ARG;
    if (level1_path == nullptr) level1_path = "";
    if (level2_path == nullptr) level2_path = "";
    if (strlen(level1_path) >= sizeof(g_music_selection.level1_path) ||
        strlen(level2_path) >= sizeof(g_music_selection.level2_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const DeviceSettingsSnapshot old_settings = g_settings;
    const DeviceMusicListSelection old_selection = g_music_selection;
    g_settings.music_list_scope = scope;
    g_music_selection.scope = scope;
    snprintf(g_music_selection.level1_path, sizeof(g_music_selection.level1_path), "%s", level1_path);
    snprintf(g_music_selection.level2_path, sizeof(g_music_selection.level2_path), "%s", level2_path);

    nvs_handle_t handle = 0;
    esp_err_t ret = open_rw(&handle);
    if (ret == ESP_OK) ret = nvs_set_u16(handle, "schema", kSchemaVersion);
    if (ret == ESP_OK) ret = nvs_set_u8(handle, "muscope", static_cast<uint8_t>(scope));
    if (ret == ESP_OK) ret = nvs_set_str(handle, "mul1", g_music_selection.level1_path);
    if (ret == ESP_OK) ret = nvs_set_str(handle, "mul2", g_music_selection.level2_path);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);

    if (ret != ESP_OK) {
        g_settings = old_settings;
        g_music_selection = old_selection;
        log_commit_failure("music_list", ret);
    }
    return ret;
}

esp_err_t device_settings_set_music_list_scope(DeviceMusicListScope scope)
{
    return device_settings_set_music_list_selection(
        scope,
        g_music_selection.level1_path,
        g_music_selection.level2_path);
}

esp_err_t device_settings_set_motion_controls_enabled(bool enabled)
{
    const bool old = g_settings.motion_controls_enabled;
    g_settings.motion_controls_enabled = enabled;
    const esp_err_t ret = commit_u8("motion", enabled ? 1U : 0U);
    if (ret != ESP_OK) g_settings.motion_controls_enabled = old;
    log_commit_failure("motion", ret);
    return ret;
}

esp_err_t device_settings_set_remember_volume(bool enabled)
{
    const bool old = g_settings.remember_volume;
    g_settings.remember_volume = enabled;
    const esp_err_t ret = commit_u8("memvol", enabled ? 1U : 0U);
    if (ret != ESP_OK) g_settings.remember_volume = old;
    log_commit_failure("memvol", ret);
    return ret;
}

esp_err_t device_settings_reset_defaults()
{
    if (!g_settings.ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_erase_all(handle);
    if (ret == ESP_OK) ret = nvs_set_u16(handle, "schema", kSchemaVersion);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret == ESP_OK) {
        g_settings = make_defaults();
        g_music_selection = make_music_selection_defaults();
        g_settings.ready = true;
        g_settings.loaded_from_nvs = true;
        ESP_LOGI(TAG, "Settings V1已恢复默认值");
    }
    return ret;
}

const char *device_settings_usb_mode_name(DeviceUsbMode mode)
{
    switch (mode) {
        case DeviceUsbMode::Serial: return "串口";
        case DeviceUsbMode::TfCard: return "TF卡";
        default: return "未知";
    }
}

const char *device_settings_audio_output_mode_name(DeviceAudioOutputMode mode)
{
    switch (mode) {
        case DeviceAudioOutputMode::NormalHeadphones: return "普通耳机";
        case DeviceAudioOutputMode::LineOut: return "线路输出";
        case DeviceAudioOutputMode::HighImpedanceHeadphones: return "高阻耳机";
        default: return "未知";
    }
}

const char *device_settings_audio_output_level_name(DeviceAudioOutputMode mode)
{
    switch (mode) {
        // 三档由 AudioTask 安全切换 CS43131 OUT_FS/HV_EN；高阻耳机保持 HV_EN=0 的完整负载范围。
        case DeviceAudioOutputMode::NormalHeadphones: return "0.5 Vrms";
        case DeviceAudioOutputMode::LineOut: return "2.0 Vrms";
        case DeviceAudioOutputMode::HighImpedanceHeadphones: return "1.41 Vrms";
        default: return "--";
    }
}

const char *device_settings_aux_key_mode_name(DeviceAuxKeyMode mode)
{
    switch (mode) {
        case DeviceAuxKeyMode::Volume: return "音量";
        case DeviceAuxKeyMode::Track: return "切歌";
        default: return "未知";
    }
}

const char *device_settings_animation_mode_name(DeviceAnimationMode mode)
{
    switch (mode) {
        case DeviceAnimationMode::Auto: return "自动";
        case DeviceAnimationMode::Full: return "完整";
        case DeviceAnimationMode::Eco: return "省资源";
        default: return "未知";
    }
}

const char *device_settings_music_list_scope_name(DeviceMusicListScope scope)
{
    switch (scope) {
        case DeviceMusicListScope::All: return "总列表";
        case DeviceMusicListScope::Level1: return "一级列表";
        case DeviceMusicListScope::Level2: return "二级列表";
        default: return "未知";
    }
}
