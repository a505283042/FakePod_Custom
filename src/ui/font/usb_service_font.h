#pragma once

#include "lvgl.h"

// USB MSC 服务模式专用精简中文字体。
// 仅包含服务页/重启提示需要的汉字，字形数据以 const 形式编译进 Flash。
const lv_font_t *usb_service_font_get();
