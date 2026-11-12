#pragma once

#include "esp_err.h"

// 图片浏览 APP：扫描 /sdcard 下的 JPEG/PNG，解码缩放到 460x460 居中显示。
// 左右滑动翻页，下滑返回 Launcher。
esp_err_t picture_app_register();
