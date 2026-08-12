#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

// Display/LVGL 适配层内部接口。业务模块不应直接获取 Panel IO / Panel handle。
esp_lcd_panel_io_handle_t display_get_panel_io();
esp_lcd_panel_handle_t display_get_panel();

// CO5300 TE 同步只供 LVGL 刷新和 BoundedSPI transport 使用。
bool display_te_is_ready();
bool display_te_wait_next(uint32_t timeout_ms);
uint32_t display_te_get_period_us();
