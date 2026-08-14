#pragma once

#include "esp_err.h"

// APP.1：把 Core Platform V1 已经存在的 Music 前台绑定到 App Manager。
// V1 只开放 Foreground <-> Background；Stopped 仍拒绝，直到 Music UI 支持真正 destroy/recreate。
esp_err_t music_app_adapter_bind();
