#pragma once

#include "esp_err.h"

// R46.0.56: Wi-Fi 局域网遥控。UDP 发现 + 单 TCP 客户端，
// 复用 BLE 手机协议的 0x01~0x06 命令与 Status/Metadata payload。
esp_err_t wifi_remote_service_start();
void wifi_remote_service_stop();
bool wifi_remote_service_is_running();
