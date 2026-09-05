#pragma once

#include <stdint.h>

#include "lvgl.h"

enum class SettingsMenuIcon : uint8_t {
    Connection = 0,
    Applications,
    System,
    About,
    Count,
};

static constexpr uint16_t kSettingsMenuIconSize = 56U;

enum class SettingsDetailIcon : uint8_t {
    None = 0,
    Usb,
    TfFiles,
    Bluetooth,
    Airplane,
    Startup,
    Music,
    Video,
    Reader,
    Synth,
    AudioOutput,
    Brightness,
    ScreenOff,
    Aod,
    AuxKey,
    Firmware,
    Framework,
    InternalRam,
    Psram,
    SdCard,
    Count,
};

static constexpr uint16_t kSettingsDetailIconSize = 24U;

// 56x56 A8 alpha-only icons, stored in flash.
// LVGL uses image_recolor as the visible icon color, so one data set can follow any UI theme.
const lv_image_dsc_t *settings_menu_icon_dsc(SettingsMenuIcon icon);

// 24x24 A8 alpha-only detail-row icons. They are intentionally small and can be recolored
// independently from the row text without any runtime PNG/JPG decoding.
const lv_image_dsc_t *settings_detail_icon_dsc(SettingsDetailIcon icon);
