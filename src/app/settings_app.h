#pragma once

#include "esp_err.h"

// Settings V1：注册“设置”APP。第一阶段只搭页面/数据框架，硬件动作分阶段接入。
esp_err_t settings_app_register();
