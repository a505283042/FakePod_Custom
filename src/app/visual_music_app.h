#pragma once

#include "esp_err.h"

// 注册第2个 Launcher 槽位的“电子音流” APP。
// 电子音流沿用 Video/Ebook 生命周期与文件浏览；当前接入 NSF 纯瀑布与 6502/2A03 音频合成。
esp_err_t visual_music_app_register();
