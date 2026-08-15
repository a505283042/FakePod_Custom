#pragma once

#include "esp_err.h"

// Ebook：/sdcard/txt 文件浏览 + UTF-8 TXT 小说智能重排全屏分页 Reader。
// 支持退出 TXT/切出 APP 自动保存内容锚点；全屏/带标签各自维护规范页索引并按锚点互相映射。
// Reader 内可立即保存当前位置、暂停/继续后台音乐；Browser 根目录支持圆环 APP 切换。
esp_err_t ebook_app_register();
