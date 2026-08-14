#pragma once

#include "esp_err.h"

// APP.3：Ebook V1 文件浏览与 TXT 打开。
// 根目录固定 /sdcard/BOOKS；支持子目录与 UTF-8/UTF-8 BOM .txt。
// 当前只读取前 8KB 作为滚动预览，不做分页/书签/阅读进度/NVS。
esp_err_t ebook_app_register();
