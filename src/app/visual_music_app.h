#pragma once

#include "esp_err.h"

// 注册第2个 Launcher 槽位的“电子音流” APP。
// 电子音流沿用 Video/Ebook 生命周期与文件浏览；当前接入 MIDI 时间轴与纯瀑布，NSF/音频合成后续接入。
esp_err_t visual_music_app_register();
